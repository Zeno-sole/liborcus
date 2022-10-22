/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "orcus/parser_base.hpp"
#include "orcus/parser_global.hpp"
#include "cpu_features.hpp"

#include <sstream>
#include <cstring>
#include <limits>
#include <cassert>
#include <algorithm>

#ifdef __ORCUS_CPU_FEATURES
#include <immintrin.h>
#endif

namespace orcus {

namespace {

std::string build_offset_msg(std::ptrdiff_t offset)
{
    std::ostringstream os;
    os << " (offset=" << offset << ')';
    return os.str();
}

}

parse_error::parse_error(const std::string& msg, std::ptrdiff_t offset) :
    general_error(msg), m_offset(offset)
{
    append_msg(build_offset_msg(offset));
}

parse_error::parse_error(const std::string& cls, const std::string& msg, std::ptrdiff_t offset) :
    general_error(cls, msg), m_offset(offset)
{
    append_msg(build_offset_msg(offset));
}

std::ptrdiff_t parse_error::offset() const
{
    return m_offset;
}

std::string parse_error::build_message(const char* msg_before, char c, const char* msg_after)
{
    std::ostringstream os;

    if (msg_before)
        os << msg_before;

    os << c;

    if (msg_after)
        os << msg_after;

    return os.str();
}

std::string parse_error::build_message(
    const char* msg_before, const char* p, size_t n, const char* msg_after)
{
    std::ostringstream os;

    if (msg_before)
        os << msg_before;

    os << std::string_view(p, n);

    if (msg_after)
        os << msg_after;

    return os.str();
}

parser_base::parser_base(const char* p, size_t n, bool transient_stream) :
    mp_begin(p), mp_char(p), mp_end(p+n),
    m_transient_stream(transient_stream),
    m_func_parse_numeric(parse_numeric)
{
}

void parser_base::prev(size_t dec)
{
    mp_char -= dec;
}

char parser_base::next_char() const
{
    return *(mp_char+1);
}

void parser_base::skip(const char* chars_to_skip, size_t n_chars_to_skip)
{
#if defined(__ORCUS_CPU_FEATURES) && defined(__SSE4_2__)
    __m128i match = _mm_loadu_si128((const __m128i*)chars_to_skip);
    const int mode = _SIDD_LEAST_SIGNIFICANT | _SIDD_CMP_EQUAL_ANY | _SIDD_UBYTE_OPS | _SIDD_NEGATIVE_POLARITY;

    int n_total = available_size();

    while (n_total)
    {
        __m128i char_block = _mm_loadu_si128((const __m128i*)mp_char);

        // Find position of the first character that is NOT any of the
        // characters to skip.
        int n = std::min<int>(16, n_total);
        int r = _mm_cmpestri(match, n_chars_to_skip, char_block, n, mode);

        if (!r)
            // No characters to skip. Bail out.
            break;

        mp_char += r; // Move the current char position.

        if (r < 16)
            // No need to move to the next segment. Stop here.
            break;

        // Skip 16 chars to the next segment.
        n_total -= 16;
    }
#else
    for (; has_char(); next())
    {
        if (!is_in(*mp_char, {chars_to_skip, n_chars_to_skip}))
            break;
    }
#endif
}

void parser_base::skip_space_and_control()
{
#if defined(__ORCUS_CPU_FEATURES) && defined(__AVX2__)
    size_t n_total = available_size();
    const __m256i ws = _mm256_set1_epi8(' '); // whitespaces
    const __m256i sb = _mm256_set1_epi8(0x80); // signed bit on.

    while (n_total)
    {
        // The 'results' stores (for each 8-bit int) 0x00 if the char is less
        // than or equal to whitespace, or the char is "negative" i.e. the
        // signed bit is on IOW greater than 127.
        __m256i char_block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mp_char));
        __m256i results = _mm256_cmpgt_epi8(char_block, ws); // NB: this is a signed comparison.
        results = _mm256_or_si256(results, _mm256_and_si256(char_block, sb));
        int r = _mm256_movemask_epi8(results);
        r = _tzcnt_u32(r);
        r = std::min<size_t>(r, n_total);

        if (!r)
            // No characters to skip. Bail out.
            break;

        mp_char += r; // Move the current char position.

        if (r < 32)
            // No need to move to the next segment. Stop here.
            break;

        n_total -= 32;
    }

#elif defined(__ORCUS_CPU_FEATURES) && defined(__SSE4_2__)
    __m128i match = _mm_loadu_si128((const __m128i*)"\0 ");
    const int mode = _SIDD_LEAST_SIGNIFICANT | _SIDD_CMP_RANGES | _SIDD_UBYTE_OPS | _SIDD_NEGATIVE_POLARITY;

    size_t n_total = available_size();

    while (n_total)
    {
        __m128i char_block = _mm_loadu_si128((const __m128i*)mp_char);

        // Find position of the first character that is NOT any of the
        // characters to skip.
        int n = std::min<size_t>(16u, n_total);
        int r = _mm_cmpestri(match, 2, char_block, n, mode);

        if (!r)
            // No characters to skip. Bail out.
            break;

        mp_char += r; // Move the current char position.

        if (r < 16)
            // No need to move to the next segment. Stop here.
            break;

        // Skip 16 chars to the next segment.
        n_total -= 16;
    }
#else
    for (; mp_char != mp_end && ((unsigned char)*mp_char) <= (unsigned char)' '; ++mp_char)
        ;
#endif
}

bool parser_base::parse_expected(const char* expected, size_t n_expected)
{
    if (n_expected > available_size())
        return false;

#if defined(__ORCUS_CPU_FEATURES) && defined(__SSE4_2__)
    __m128i v_expected = _mm_loadu_si128((const __m128i*)expected);
    __m128i v_char_block = _mm_loadu_si128((const __m128i*)mp_char);

    const int mode = _SIDD_CMP_EQUAL_ORDERED | _SIDD_UBYTE_OPS | _SIDD_BIT_MASK;
    __m128i res = _mm_cmpestrm(v_expected, n_expected, v_char_block, n_expected, mode);
    int mask = _mm_cvtsi128_si32(res);

    if (mask)
        mp_char += n_expected;

    return mask;
#else
    for (size_t i = 0; i < n_expected; ++i, ++expected, next())
    {
        if (cur_char() != *expected)
            return false;
    }

    return true;
#endif
}

double parser_base::parse_double()
{
    size_t max_length = available_size();
    const char* p = mp_char;
    double val = m_func_parse_numeric(p, max_length);
    if (p == mp_char)
        return std::numeric_limits<double>::quiet_NaN();

    mp_char = p;
    return val;
}

size_t parser_base::remaining_size() const
{
    size_t n = available_size();
    return n ? (n - 1) : 0;
}

std::ptrdiff_t parser_base::offset() const
{
    return std::distance(mp_begin, mp_char);
}

}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

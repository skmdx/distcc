/* Streaming C/C++ lexical features for compile-cost estimation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DCC_SOURCE_SCAN_H
#define DCC_SOURCE_SCAN_H
#include <string.h>

struct source_scan {
    enum { CODE, SLASH, LINE_COMMENT, BLOCK_COMMENT, BLOCK_STAR, STRING, CHARACTER,
           RAW_DELIMITER, RAW_BODY, RAW_END } state;
    unsigned occupied, escaped, splice, number, word_length;
    char word[3]; /* u8R is the longest raw-string prefix. */
    char delimiter[16]; /* C++ d-char-seq limit */
    unsigned length, matched;
};

/* Return only declaration/statement terminators and directive introductions.
 * A pending slash may turn out to be whitespace (a comment). */
static inline char source_token(struct source_scan *s, char c)
{
    switch (s->state) {
    case SLASH:
        if (c == '/') { s->state = LINE_COMMENT; return 0; }
        if (c == '*') { s->state = BLOCK_COMMENT; return 0; }
        s->occupied = 1;
        s->state = CODE;
        break;
    case LINE_COMMENT:
        if (c != '\n') return 0;
        s->state = CODE;
        break;
    case BLOCK_COMMENT:
    case BLOCK_STAR:
        if (c == '/' && s->state == BLOCK_STAR) s->state = CODE;
        else s->state = c == '*' ? BLOCK_STAR : BLOCK_COMMENT;
        if (c == '\n') s->occupied = 0;
        return 0;
    case STRING:
    case CHARACTER:
        if (s->escaped) s->escaped = 0;
        else if (c == '\\') s->escaped = 1;
        else if (c == (s->state == STRING ? '"' : '\'')) s->state = CODE;
        return 0;
    case RAW_DELIMITER:
        if (c == '(') s->state = RAW_BODY;
        else if (s->length < sizeof(s->delimiter)) s->delimiter[s->length++] = c;
        else s->state = STRING;
        return 0;
    case RAW_BODY:
    case RAW_END:
        if (s->state == RAW_END) {
            if (s->matched == s->length && c == '"') { s->state = CODE; return 0; }
            if (s->matched < s->length && c == s->delimiter[s->matched]) {
                ++s->matched;
                return 0;
            }
        }
        s->matched = 0;
        s->state = c == ')' ? RAW_END : RAW_BODY;
        return 0;
    case CODE:
        break;
    }

    int digit = c >= '0' && c <= '9';
    if (digit || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
        (s->number && (c == '\'' || c == '.'))) {
        if (!s->word_length) s->number = digit;
        if (s->word_length < sizeof(s->word)) s->word[s->word_length] = c;
        if (s->word_length <= sizeof(s->word)) ++s->word_length;
        s->occupied = 1;
        return 0;
    }
    if (c == '"') {
        int raw = (s->word_length == 1 && s->word[0] == 'R') ||
                  (s->word_length == 2 && strchr("uUL", s->word[0]) && s->word[1] == 'R') ||
                  (s->word_length == 3 && !memcmp(s->word, "u8R", 3));
        s->state = raw ? RAW_DELIMITER : STRING;
        s->length = 0;
    }
    s->word_length = s->number = 0;
    if (c == '/') { s->state = SLASH; return 0; }
    if (c == '\'') s->state = CHARACTER;
    if (c == '\n') { s->occupied = 0; return 0; }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') return 0;
    char token = c == ';' || (c == '#' && !s->occupied) ? c : 0;
    s->occupied = 1;
    return token;
}

/* Remove escaped newlines before tokenization, even across read boundaries.
 * Raw string bodies preserve their physical bytes. */
static inline char source_byte(struct source_scan *s, char c)
{
    if (s->splice) {
        if (s->splice == 1 && c == '\r') { s->splice = 2; return 0; }
        if (c != '\n') {
            source_token(s, '\\');
            if (s->splice == 2) source_token(s, '\r');
        }
        s->splice = 0;
        if (c == '\n') return 0;
    }
    if (c == '\\' && s->state < RAW_DELIMITER) { s->splice = 1; return 0; }
    return source_token(s, c);
}
#endif

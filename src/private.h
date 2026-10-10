#ifndef DMVS_JS_PRIVATE_H
#define DMVS_JS_PRIVATE_H

#include "dmod.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/*
 * A dmod module's data is not relocated when it is loaded (only its GOT
 * is): no tables of pointers here - names are text, looked up in it.
 */

/* ---- Memory: every node and string of a tree, in chunks freed together ---- */

typedef struct chunk chunk_t;
struct chunk
{
    chunk_t*    next;
    size_t      used;
    size_t      size;
    /* the memory follows */
};

typedef struct
{
    chunk_t*    chunks;
    bool        failed;
} arena_t;

void*   arena_alloc(arena_t* a, size_t size);                      /* Zeroed, 8-byte aligned; NULL: out of memory */
char*   arena_strndup(arena_t* a, const char* s, size_t length);
void    arena_release(arena_t* a);

/* ---- Tokens ---- */

#define T_EOF           0u
#define T_NAME          1u          /* An identifier or a keyword (text: as written) */
#define T_PRIVATE       2u          /* #name */
#define T_NUMBER        3u
#define T_STRING        4u
#define T_TEMPLATE      5u          /* A chunk of a template literal: `...${  }...${  }...` */
#define T_REGEX         6u
#define T_PUNCT         7u          /* An operator or punctuation (text: as written) */

typedef struct
{
    uint8_t     kind;               /* T_* */
    bool        newline;            /* A line break before it (automatic semicolons) */
    bool        tail;               /* T_TEMPLATE: it ends the literal (`), not an ${ */
    uint32_t    start;              /* Its source: [start, end) */
    uint32_t    end;
    uint32_t    line;
    uint32_t    column;
    const char* text;               /* T_STRING, T_TEMPLATE: the value, decoded (in the arena); else NULL */
    size_t      length;
    int64_t     number;             /* T_NUMBER: number / 10^decimals */
    uint8_t     decimals;
    bool        inexact;
} token_t;

typedef struct
{
    const char*     src;
    uint32_t        length;
    uint32_t        pos;
    uint32_t        line;
    uint32_t        line_start;     /* Offset of the current line */
    bool            regex_ok;       /* A '/' here starts a regular expression (not a division) */
    token_t         tok;            /* The current token */
    arena_t*        arena;
    dmvs_js_error_t* error;
} lexer_t;

void    lex_init(lexer_t* l, const char* src, size_t length, arena_t* arena, dmvs_js_error_t* error);
void    lex_next(lexer_t* l);                                       /* The next token into l->tok */
void    lex_template_continue(lexer_t* l);                          /* At a '}' closing an ${: the template's next chunk */
bool    lex_failed(const lexer_t* l);
void    lex_fail(lexer_t* l, uint32_t offset, const char* message);
void    lex_fail_at(lexer_t* l, const token_t* t, int status, const char* message);
bool    tok_is(const lexer_t* l, const char* text);                 /* The current token is this punctuation / name */
bool    tok_name(const lexer_t* l, const char* name);               /* ... this name (an identifier or a keyword) */
bool    is_keyword(const char* s, size_t n);                        /* A reserved word (not a name) */

/* ---- Parser ---- */

dmvs_js_node_t* parse_program(arena_t* arena, const char* source, size_t length, dmvs_js_error_t* error);

#endif /* DMVS_JS_PRIVATE_H */

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

/* ---- The source: in memory, or a stream through a window ---- */

#define SOURCE_MARKS    8u

typedef struct
{
    dmvs_js_read_fn read;           /* A stream (NULL: all of it is in buf) */
    void*           ctx;
    bool            owned;          /* buf is the window (else the caller's text) */
    char*           buf;            /* The bytes [base, base + len) of the source */
    uint32_t        base;
    uint32_t        len;
    uint32_t        cap;
    bool            eof;            /* Nothing more to read */
    int             failed;         /* -EIO, -ENOMEM: reading stopped */
    uint32_t        keep;           /* The first byte the lexer still needs ... */
    uint32_t        marks[SOURCE_MARKS];    /* ... and where the look aheads started */
    uint32_t        mark_count;
    uint32_t        peak;           /* The largest the window was */
} source_t;

void        source_memory(source_t* s, const char* text, size_t length);
void        source_stream(source_t* s, dmvs_js_read_fn read, void* ctx);
void        source_release(source_t* s);
bool        source_more(source_t* s, uint32_t offset);             /* The byte at offset is there (read it if need be) */
char        source_at(source_t* s, uint32_t offset);               /* '\0' past the end */
const char* source_text(source_t* s, uint32_t offset);             /* Valid until the next read: a token's bytes */
void        source_mark(source_t* s, uint32_t offset);
void        source_unmark(source_t* s);

typedef struct
{
    source_t*       src;
    uint32_t        pos;
    uint32_t        line;
    uint32_t        line_start;     /* Offset of the current line */
    bool            regex_ok;       /* A '/' here starts a regular expression (not a division) */
    token_t         tok;            /* The current token */
    arena_t*        arena;
    dmvs_js_error_t* error;
} lexer_t;

void    lex_init(lexer_t* l, source_t* src, arena_t* arena, dmvs_js_error_t* error);
void    lex_next(lexer_t* l);                                       /* The next token into l->tok */
void    lex_template_continue(lexer_t* l);                          /* At a '}' closing an ${: the template's next chunk */
bool    lex_failed(const lexer_t* l);
void    lex_fail(lexer_t* l, uint32_t offset, const char* message);     /* At offset of the current line */
void    lex_fail_at(lexer_t* l, const token_t* t, int status, const char* message);
bool    tok_is(const lexer_t* l, const char* text);                 /* The current token is this punctuation / name */
bool    tok_name(const lexer_t* l, const char* name);               /* ... this name (an identifier or a keyword) */
bool    is_keyword(const char* s, size_t n);                        /* A reserved word (not a name) */
const char* lex_token_text(const lexer_t* l);                        /* The current token's bytes (tok.end - tok.start) */
void    lex_save(const lexer_t* l, lexer_t* saved);                 /* Before looking ahead ... */
void    lex_restore(lexer_t* l, const lexer_t* saved);              /* ... back to where it was */

/* ---- Parser ---- */

dmvs_js_node_t* parse_program(arena_t* arena, source_t* source, dmvs_js_error_t* error);

#endif /* DMVS_JS_PRIVATE_H */

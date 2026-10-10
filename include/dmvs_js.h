#ifndef DMVS_JS_H
#define DMVS_JS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmvs_js_defs.h"

/*
 * dmvs_js - JavaScript for dmview views: a parser of the JavaScript that
 * pages use (ES2020: let / const, arrow functions, template literals,
 * classes, destructuring, spread, optional chaining, ...) into a syntax
 * tree, which a converter compiles into a view's code (dmvs).
 *
 * The tree is ESTree-like but compact: every node has the same shape - its
 * kind, an operator, flags, up to four children `a` ... `d` (what they are
 * depends on the kind, see below) and `next`, the next node of a list. All
 * of it lives in the tree (dmvs_js_parse() ... dmvs_js_free()).
 */

/* ---- Nodes ---- */

/* Statements                       a               b               c               d */
#define DMVS_JS_PROGRAM     1u  /*  first statement */
#define DMVS_JS_VAR         2u  /*  first DECLARATOR - op: DMVS_JS_VAR_* */
#define DMVS_JS_DECLARATOR  3u  /*  target          initializer (NULL) */
#define DMVS_JS_FUNCTION    4u  /*  first parameter body (BLOCK, or an expression: DMVS_JS_F_EXPRESSION) - text: its name (NULL) */
#define DMVS_JS_RETURN      5u  /*  value (NULL) */
#define DMVS_JS_IF          6u  /*  condition       then            else (NULL) */
#define DMVS_JS_FOR         7u  /*  init (NULL)     test (NULL)     update (NULL)   body */
#define DMVS_JS_FOR_IN      8u  /*  left            right           body */
#define DMVS_JS_FOR_OF      9u  /*  left            right           body */
#define DMVS_JS_WHILE       10u /*  condition       body */
#define DMVS_JS_DO_WHILE    11u /*  body            condition */
#define DMVS_JS_BREAK       12u /*  - text: its label (NULL) */
#define DMVS_JS_CONTINUE    13u /*  - text: its label (NULL) */
#define DMVS_JS_SWITCH      14u /*  discriminant    first CASE */
#define DMVS_JS_CASE        15u /*  test (NULL: default) first statement */
#define DMVS_JS_BLOCK       16u /*  first statement */
#define DMVS_JS_EXPRESSION  17u /*  the expression */
#define DMVS_JS_EMPTY       18u
#define DMVS_JS_THROW       19u /*  value */
#define DMVS_JS_TRY         20u /*  block           catch parameter (NULL) catch BLOCK (NULL) finally BLOCK (NULL) */
#define DMVS_JS_LABELED     21u /*  statement - text: the label */
#define DMVS_JS_CLASS       22u /*  superclass (NULL) first FIELD - text: its name (NULL) */
#define DMVS_JS_FIELD       23u /*  key             value (NULL; a FUNCTION for a method) - flags: _STATIC, _COMPUTED, _METHOD, _GETTER, _SETTER */

/* Expressions */
#define DMVS_JS_IDENT       32u /*  - text: the name */
#define DMVS_JS_NUMBER      33u /*  - number / 10^decimals; text: as written */
#define DMVS_JS_STRING      34u /*  - text: the value (escapes decoded, UTF-8) */
#define DMVS_JS_TEMPLATE    35u /*  first part      tag (NULL) - parts: STRING (the chunks, cooked) and expressions, in order */
#define DMVS_JS_REGEX       36u /*  - text: /pattern/flags as written */
#define DMVS_JS_BOOL        37u /*  - op: 0 false, 1 true */
#define DMVS_JS_NULL        38u
#define DMVS_JS_THIS        39u
#define DMVS_JS_ARRAY       40u /*  first element (HOLE for an elision) */
#define DMVS_JS_OBJECT      41u /*  first PROPERTY (or SPREAD) */
#define DMVS_JS_PROPERTY    42u /*  key (IDENT / STRING / NUMBER, an expression: _COMPUTED) value - flags: _SHORTHAND, _METHOD, _GETTER, _SETTER */
#define DMVS_JS_UNARY       43u /*  operand - op: !, -, +, ~, typeof, void, delete */
#define DMVS_JS_UPDATE      44u /*  operand - op: ++, --; flags: DMVS_JS_F_PREFIX */
#define DMVS_JS_BINARY      45u /*  left            right - op: arithmetic, comparison, bitwise, in, instanceof */
#define DMVS_JS_LOGICAL     46u /*  left            right - op: &&, ||, ?? */
#define DMVS_JS_ASSIGN      47u /*  target          value - op: =, or the operator of +=, ...; in a pattern: a default */
#define DMVS_JS_CONDITIONAL 48u /*  test            consequent      alternate */
#define DMVS_JS_CALL        49u /*  callee          first argument - flags: _OPTIONAL (f?.()) */
#define DMVS_JS_NEW         50u /*  callee          first argument */
#define DMVS_JS_MEMBER      51u /*  object          property (IDENT, an expression: _COMPUTED) - flags: _OPTIONAL (a?.b) */
#define DMVS_JS_SEQUENCE    52u /*  first expression */
#define DMVS_JS_SPREAD      53u /*  argument - also a rest element / parameter */
#define DMVS_JS_HOLE        54u /*  an elision of an array: [a, , b] */
#define DMVS_JS_AWAIT       55u /*  argument */
#define DMVS_JS_YIELD       56u /*  argument (NULL) - flags: _DELEGATE (yield*) */

/* DMVS_JS_VAR's op */
#define DMVS_JS_VAR_VAR     0u
#define DMVS_JS_VAR_LET     1u
#define DMVS_JS_VAR_CONST   2u

/* Flags */
#define DMVS_JS_F_COMPUTED      0x0001u     /* a[b], { [k]: v } */
#define DMVS_JS_F_OPTIONAL      0x0002u     /* a?.b, f?.() */
#define DMVS_JS_F_PREFIX        0x0004u     /* ++a */
#define DMVS_JS_F_SHORTHAND     0x0008u     /* { a } */
#define DMVS_JS_F_METHOD        0x0010u     /* { f() {} } */
#define DMVS_JS_F_GETTER        0x0020u
#define DMVS_JS_F_SETTER        0x0040u
#define DMVS_JS_F_ARROW         0x0080u     /* () => ... */
#define DMVS_JS_F_EXPRESSION    0x0100u     /* An arrow function with an expression for its body */
#define DMVS_JS_F_ASYNC         0x0200u
#define DMVS_JS_F_GENERATOR     0x0400u
#define DMVS_JS_F_STATIC        0x0800u
#define DMVS_JS_F_DELEGATE      0x1000u     /* yield* */
#define DMVS_JS_F_PATTERN       0x2000u     /* An ARRAY / OBJECT that is a destructuring target */
#define DMVS_JS_F_DECLARATION   0x4000u     /* A FUNCTION / CLASS that is a declaration */
#define DMVS_JS_F_INEXACT       0x8000u     /* A NUMBER that number / 10^decimals does not hold exactly */

/* Operators (op of UNARY, UPDATE, BINARY, LOGICAL, ASSIGN) */
#define DMVS_JS_OP_NONE         0u
#define DMVS_JS_OP_ADD          1u      /* + */
#define DMVS_JS_OP_SUB          2u      /* - */
#define DMVS_JS_OP_MUL          3u      /* * */
#define DMVS_JS_OP_DIV          4u      /* / */
#define DMVS_JS_OP_MOD          5u      /* % */
#define DMVS_JS_OP_POW          6u      /* ** */
#define DMVS_JS_OP_SHL          7u      /* << */
#define DMVS_JS_OP_SHR          8u      /* >> */
#define DMVS_JS_OP_USHR         9u      /* >>> */
#define DMVS_JS_OP_BITAND       10u     /* & */
#define DMVS_JS_OP_BITOR        11u     /* | */
#define DMVS_JS_OP_BITXOR       12u     /* ^ */
#define DMVS_JS_OP_EQ           13u     /* == */
#define DMVS_JS_OP_NE           14u     /* != */
#define DMVS_JS_OP_SEQ          15u     /* === */
#define DMVS_JS_OP_SNE          16u     /* !== */
#define DMVS_JS_OP_LT           17u     /* < */
#define DMVS_JS_OP_LE           18u     /* <= */
#define DMVS_JS_OP_GT           19u     /* > */
#define DMVS_JS_OP_GE           20u     /* >= */
#define DMVS_JS_OP_IN           21u     /* in */
#define DMVS_JS_OP_INSTANCEOF   22u     /* instanceof */
#define DMVS_JS_OP_AND          23u     /* && */
#define DMVS_JS_OP_OR           24u     /* || */
#define DMVS_JS_OP_COALESCE     25u     /* ?? */
#define DMVS_JS_OP_NOT          26u     /* ! */
#define DMVS_JS_OP_BITNOT       27u     /* ~ */
#define DMVS_JS_OP_TYPEOF       28u     /* typeof */
#define DMVS_JS_OP_VOID         29u     /* void */
#define DMVS_JS_OP_DELETE       30u     /* delete */
#define DMVS_JS_OP_INC          31u     /* ++ */
#define DMVS_JS_OP_DEC          32u     /* -- */
#define DMVS_JS_OP_ASSIGN       33u     /* = (a compound assignment's op is its operator: += is DMVS_JS_OP_ADD) */

typedef struct dmvs_js_node dmvs_js_node_t;
struct dmvs_js_node
{
    uint8_t                 kind;       /**< DMVS_JS_* */
    uint8_t                 op;         /**< DMVS_JS_OP_*, DMVS_JS_VAR_*, a BOOL's value */
    uint16_t                flags;      /**< DMVS_JS_F_* */
    const dmvs_js_node_t*   a;          /**< Children, by kind (see above) */
    const dmvs_js_node_t*   b;
    const dmvs_js_node_t*   c;
    const dmvs_js_node_t*   d;
    const dmvs_js_node_t*   next;       /**< The next node of a list: statements, arguments, elements, ... */
    const char*             text;       /**< A name, a string's value, ... (NUL-terminated) - NULL when the kind has none */
    size_t                  length;     /**< Bytes of `text` (a string may hold NULs) */
    int64_t                 number;     /**< NUMBER: its value is number / 10^decimals */
    uint8_t                 decimals;
    uint32_t                start;      /**< Its source: the bytes [start, end) */
    uint32_t                end;
    uint32_t                line;       /**< Of `start`, from 1 */
    uint32_t                column;     /**< Of `start`, from 1, in bytes */
};

/** A parsed script: its tree (opaque). */
typedef struct dmvs_js_ast* dmvs_js_ast_t;

/** Why a script was not parsed. */
typedef struct
{
    int         status;         /**< 0, -EBADMSG (a syntax error), -ENOMEM, -E2BIG (nested too deeply), -EIO (not read), -EINVAL */
    uint32_t    offset;         /**< Where, in the source */
    uint32_t    line;           /**< From 1 */
    uint32_t    column;         /**< From 1, in bytes */
    char        message[96];    /**< e.g. "expected ')'" */
} dmvs_js_error_t;

/**
 * Reads the next bytes of a script into `buffer` (at most `size`): how many,
 * 0 at its end, < 0 when it cannot be read.
 */
typedef int32_t (*dmvs_js_read_fn)(void* ctx, char* buffer, size_t size);

/** What a tree took. */
typedef struct
{
    uint32_t    source;         /**< Bytes of the script */
    uint32_t    window;         /**< The most of it held at once while it was parsed (all of it in memory: its size) */
    uint32_t    tree;           /**< Bytes of the tree's memory (its nodes and strings) */
    uint32_t    nodes;
} dmvs_js_info_t;

/* ---- API ---- */

/**
 * @brief Parse a script (a classic script, not a module).
 * @param source Its text, UTF-8 (not NUL-terminated necessarily) - the tree does not refer to it
 * @param error  Receives why it was not parsed (may be NULL)
 * @return The tree, NULL on failure
 */
dmod_dmvs_js_api(1.0, dmvs_js_ast_t, _parse, ( const char* source, size_t length, dmvs_js_error_t* error ));

/**
 * @brief Parse a script read in pieces - never all of it at once: the
 *        parser keeps the bytes from the current token (or where a look
 *        ahead started, at most to the ')' of an arrow function's
 *        parameters) on. The tree does not refer to the script.
 * @param read Called for more until it returns 0 (the end) or < 0 (-EIO)
 */
dmod_dmvs_js_api(1.0, dmvs_js_ast_t, _parse_stream, ( dmvs_js_read_fn read, void* ctx, dmvs_js_error_t* error ));

/** @brief What a tree took (window, memory, nodes). */
dmod_dmvs_js_api(1.0, int, _info, ( dmvs_js_ast_t ast, dmvs_js_info_t* info ));

/** @brief The tree's PROGRAM node. */
dmod_dmvs_js_api(1.0, const dmvs_js_node_t*, _root, ( dmvs_js_ast_t ast ));

/** @brief Release a tree. Safe on NULL. */
dmod_dmvs_js_api(1.0, void, _free, ( dmvs_js_ast_t ast ));

/**
 * @brief The tree of a node as an S-expression, e.g.
 *        (call (member (ident document) getElementById) (string "a")).
 * @return Bytes of all of it (as snprintf: `buffer` gets as much as fits, NUL-terminated)
 */
dmod_dmvs_js_api(1.0, size_t, _dump, ( const dmvs_js_node_t* node, char* buffer, size_t size ));

/** @brief A kind's name, e.g. "call" - "?" for none. */
dmod_dmvs_js_api(1.0, const char*, _kind_name, ( uint8_t kind ));

/** @brief An operator as written, e.g. "+" ("+=" is DMVS_JS_OP_ADD of an ASSIGN) - "?" for none. */
dmod_dmvs_js_api(1.0, const char*, _op_name, ( uint8_t op ));

#endif /* DMVS_JS_H */

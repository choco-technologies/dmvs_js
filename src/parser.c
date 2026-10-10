#include "private.h"

/*
 * The parser: recursive descent over the lexer's tokens into the tree.
 *
 * - Arrow functions are told from parenthesized expressions by looking
 *   ahead to the token after the matching ')' (the lexer is copied and
 *   put back).
 * - Destructuring targets are parsed as array / object literals and then
 *   marked as patterns (DMVS_JS_F_PATTERN); { a = 1 } is allowed in an
 *   object literal for that (a shorthand property with an ASSIGN).
 * - Semicolons are inserted where the language does (a line break, '}',
 *   the end), and return / break / continue / throw end at a line break.
 */

#define MAX_DEPTH   64u                 /* Statements / expressions nested at most (the stack of a small target) */

typedef dmvs_js_node_t node_t;

typedef struct
{
    lexer_t     lx;
    arena_t*    arena;
    uint32_t    prev_end;               /* Where the token before the current one ended */
    uint32_t    depth;
    bool        in_generator;
    bool        in_async;
} parser_t;

static node_t* expression(parser_t* p, bool no_in);
static node_t* assignment(parser_t* p, bool no_in);
static node_t* statement(parser_t* p);
static node_t* function_rest(parser_t* p, node_t* f, bool method);
static node_t* class_rest(parser_t* p, const token_t* start, bool declaration);
static node_t* binding(parser_t* p);
static node_t* array_literal(parser_t* p);
static node_t* object_literal(parser_t* p);

/* ---- Helpers ---- */

static bool failed(const parser_t* p)
{
    return lex_failed(&p->lx);
}

static void fail(parser_t* p, const char* message)
{
    lex_fail_at(&p->lx, &p->lx.tok, -EBADMSG, message);
}

static void advance(parser_t* p)
{
    p->prev_end = p->lx.tok.end;
    lex_next(&p->lx);
}

static bool is(const parser_t* p, const char* text)
{
    return tok_is(&p->lx, text);
}

static bool accept(parser_t* p, const char* text)
{
    if (!is(p, text))
        return false;
    advance(p);
    return true;
}

static void expect(parser_t* p, const char* text)
{
    if (accept(p, text))
        return;
    char message[48];
    Dmod_SnPrintf(message, sizeof(message), "expected '%s'", text);
    fail(p, message);
}

static node_t* node_at(parser_t* p, uint8_t kind, const token_t* from)
{
    node_t* n = arena_alloc(p->arena, sizeof(node_t));
    if (n == NULL)
    {
        lex_fail_at(&p->lx, &p->lx.tok, -ENOMEM, "out of memory");
        return NULL;
    }
    n->kind = kind;
    n->start = from->start;
    n->line = from->line;
    n->column = from->column;
    n->end = from->end;
    return n;
}

static node_t* node(parser_t* p, uint8_t kind)
{
    return node_at(p, kind, &p->lx.tok);
}

/* A node that starts where `first` does */
static node_t* node_from(parser_t* p, uint8_t kind, const node_t* first)
{
    node_t* n = node(p, kind);
    if (n != NULL && first != NULL)
    {
        n->start = first->start;
        n->line = first->line;
        n->column = first->column;
    }
    return n;
}

static node_t* done(parser_t* p, node_t* n)
{
    if (n != NULL && p->prev_end > n->start)
        n->end = p->prev_end;
    return n;
}

/* The current token's source as the node's text */
static void take_text(parser_t* p, node_t* n)
{
    if (n == NULL)
        return;
    size_t len = p->lx.tok.end - p->lx.tok.start;
    n->text = arena_strndup(p->arena, p->lx.src + p->lx.tok.start, len);
    n->length = len;
    if (n->text == NULL)
        lex_fail_at(&p->lx, &p->lx.tok, -ENOMEM, "out of memory");
}

static void append(const node_t** head, const node_t** tail, const node_t* n)
{
    if (n == NULL)
        return;
    if (*head == NULL)
        *head = n;
    else
        ((node_t*)*tail)->next = n;
    *tail = n;
}

static bool enter(parser_t* p)
{
    if (++p->depth > MAX_DEPTH)
    {
        lex_fail_at(&p->lx, &p->lx.tok, -E2BIG, "nested too deeply");
        return false;
    }
    return true;
}

static void leave(parser_t* p)
{
    p->depth--;
}

/* A name that can be a variable: not a reserved word */
static bool at_identifier(const parser_t* p)
{
    if (p->lx.tok.kind != T_NAME)
        return false;
    const char* s = p->lx.src + p->lx.tok.start;
    size_t n = p->lx.tok.end - p->lx.tok.start;
    if (is_keyword(s, n))
        return false;
    if (p->in_generator && n == 5 && strncmp(s, "yield", 5) == 0)
        return false;
    if (p->in_async && n == 5 && strncmp(s, "await", 5) == 0)
        return false;
    return true;
}

static node_t* identifier(parser_t* p)
{
    if (!at_identifier(p))
    {
        fail(p, (p->lx.tok.kind == T_EOF) ? "unexpected end of the script" : "expected a name");
        return NULL;
    }
    node_t* n = node(p, DMVS_JS_IDENT);
    take_text(p, n);
    advance(p);
    return n;
}

/* Statements end at ';', or where one is inserted: before '}', at the end, at a line break */
static void semicolon(parser_t* p)
{
    if (accept(p, ";"))
        return;
    if (is(p, "}") || p->lx.tok.kind == T_EOF || p->lx.tok.newline)
        return;
    fail(p, "expected ';'");
}

/* ---- Patterns ---- */

static void mark_pattern(const node_t* c)
{
    node_t* n = (node_t*)c;
    if (n == NULL)
        return;
    if (n->kind == DMVS_JS_ARRAY || n->kind == DMVS_JS_OBJECT)
    {
        n->flags |= DMVS_JS_F_PATTERN;
        for (const node_t* k = n->a; k != NULL; k = k->next)
        {
            if (k->kind == DMVS_JS_PROPERTY)
                mark_pattern(k->b);
            else
                mark_pattern(k);
        }
    }
    else if (n->kind == DMVS_JS_ASSIGN)
        mark_pattern(n->a);                         /* A target with its default */
    else if (n->kind == DMVS_JS_SPREAD)
        mark_pattern(n->a);                         /* A rest element */
}

static bool valid_target(const node_t* n)
{
    return n != NULL && (n->kind == DMVS_JS_IDENT || n->kind == DMVS_JS_MEMBER ||
                         ((n->kind == DMVS_JS_ARRAY || n->kind == DMVS_JS_OBJECT) && (n->flags & DMVS_JS_F_PATTERN) != 0));
}

/* ---- Looking ahead ---- */

/* At '(': whether the matching ')' is followed by '=>' (an arrow function's parameters) */
static __attribute__((noinline)) bool arrow_ahead(parser_t* p)
{
    lexer_t saved = p->lx;
    dmvs_js_error_t error = *p->lx.error;
    char stack[MAX_DEPTH];
    uint32_t depth = 0;
    bool arrow = false;
    for (;;)
    {
        const token_t* t = &p->lx.tok;
        if (t->kind == T_EOF)
            break;
        if (t->kind == T_TEMPLATE && !t->tail)
        {
            if (depth >= MAX_DEPTH)
                break;
            stack[depth++] = '`';
        }
        else if (t->kind == T_PUNCT && (is(p, "(") || is(p, "[") || is(p, "{")))
        {
            if (depth >= MAX_DEPTH)
                break;
            stack[depth++] = p->lx.src[t->start];
        }
        else if (t->kind == T_PUNCT && (is(p, ")") || is(p, "]") || is(p, "}")))
        {
            if (depth == 0)
                break;
            depth--;
            if (stack[depth] == '`')
            {
                lex_template_continue(&p->lx);
                if (!p->lx.tok.tail)
                    stack[depth++] = '`';
                lex_next(&p->lx);
                continue;
            }
            if (depth == 0)
            {
                lex_next(&p->lx);
                arrow = is(p, "=>") && !p->lx.tok.newline;
                break;
            }
        }
        lex_next(&p->lx);
    }
    p->lx = saved;
    *p->lx.error = error;
    return arrow;
}

/* The token after the current one is this punctuation / name */
static __attribute__((noinline)) bool next_is(parser_t* p, const char* text, bool same_line)
{
    lexer_t saved = p->lx;
    dmvs_js_error_t error = *p->lx.error;
    lex_next(&p->lx);
    bool yes = is(p, text) && (!same_line || !p->lx.tok.newline);
    p->lx = saved;
    *p->lx.error = error;
    return yes;
}

/* At 'async': an async arrow function follows - async (...) => / async x => */
static __attribute__((noinline)) bool async_arrow_ahead(parser_t* p)
{
    lexer_t saved = p->lx;
    dmvs_js_error_t error = *p->lx.error;
    lex_next(&p->lx);
    bool yes = !p->lx.tok.newline && ((is(p, "(") && arrow_ahead(p)) || (at_identifier(p) && next_is(p, "=>", true)));
    p->lx = saved;
    *p->lx.error = error;
    return yes;
}

/* ---- Functions ---- */

/* A parameter or a declared target: a name or a pattern, with its default */
static node_t* parameter(parser_t* p)
{
    if (is(p, "..."))
    {
        node_t* s = node(p, DMVS_JS_SPREAD);
        advance(p);
        if (s != NULL)
            s->a = binding(p);
        return done(p, s);
    }
    node_t* target = binding(p);
    if (is(p, "="))
    {
        node_t* a = node_from(p, DMVS_JS_ASSIGN, target);
        advance(p);
        if (a != NULL)
        {
            a->op = DMVS_JS_OP_ASSIGN;
            a->a = target;
            a->b = assignment(p, false);
        }
        return done(p, a);
    }
    return target;
}

/* A name, or [ ... ] / { ... } as a pattern */
static node_t* binding(parser_t* p)
{
    if (is(p, "[") || is(p, "{"))
    {
        if (!enter(p))
            return NULL;
        node_t* n = is(p, "[") ? array_literal(p) : object_literal(p);     /* A literal ... */
        leave(p);
        mark_pattern(n);                                                    /* ... as a pattern */
        return n;
    }
    return identifier(p);
}

/* (a, b = 1, ...rest) */
static const node_t* parameters(parser_t* p)
{
    const node_t* head = NULL;
    const node_t* tail = NULL;
    expect(p, "(");
    while (!failed(p) && !is(p, ")"))
    {
        append(&head, &tail, parameter(p));
        if (!accept(p, ","))
            break;
    }
    expect(p, ")");
    return head;
}

/* { statements } of a function */
static node_t* body(parser_t* p)
{
    node_t* b = node(p, DMVS_JS_BLOCK);
    const node_t* head = NULL;
    const node_t* tail = NULL;
    expect(p, "{");
    while (!failed(p) && !is(p, "}") && p->lx.tok.kind != T_EOF)
        append(&head, &tail, statement(p));
    expect(p, "}");
    if (b != NULL)
        b->a = head;
    return done(p, b);
}

/* Parameters and body of a function f (its name and flags set); method: no name before them */
static node_t* function_rest(parser_t* p, node_t* f, bool method)
{
    (void)method;
    if (f == NULL)
        return NULL;
    bool generator = p->in_generator, async = p->in_async;
    p->in_generator = (f->flags & DMVS_JS_F_GENERATOR) != 0;
    p->in_async = (f->flags & DMVS_JS_F_ASYNC) != 0;
    f->a = parameters(p);
    f->b = body(p);
    p->in_generator = generator;
    p->in_async = async;
    return done(p, f);
}

/* function [*] [name] (...) { ... } - at 'function' */
static node_t* function_expression(parser_t* p, const token_t* start, bool async, bool declaration)
{
    node_t* f = node_at(p, DMVS_JS_FUNCTION, start);
    advance(p);                                     /* function */
    if (f == NULL)
        return NULL;
    if (async)
        f->flags |= DMVS_JS_F_ASYNC;
    if (declaration)
        f->flags |= DMVS_JS_F_DECLARATION;
    if (accept(p, "*"))
        f->flags |= DMVS_JS_F_GENERATOR;
    if (at_identifier(p) || (p->lx.tok.kind == T_NAME && !is(p, "(")))
    {
        take_text(p, f);
        advance(p);
    }
    else if (declaration)
        fail(p, "expected the function's name");
    return function_rest(p, f, false);
}

/* An arrow function at its parameters: a name or ( ... ) */
static node_t* arrow(parser_t* p, const token_t* start, bool async)
{
    node_t* f = node_at(p, DMVS_JS_FUNCTION, start);
    if (f == NULL)
        return NULL;
    f->flags = DMVS_JS_F_ARROW | (async ? DMVS_JS_F_ASYNC : 0u);
    bool generator = p->in_generator, was_async = p->in_async;
    p->in_async = async;
    if (is(p, "("))
        f->a = parameters(p);
    else
        f->a = identifier(p);
    if (p->lx.tok.newline)
        fail(p, "a line break before '=>'");
    expect(p, "=>");
    p->in_generator = false;
    if (is(p, "{"))
        f->b = body(p);
    else
    {
        f->flags |= DMVS_JS_F_EXPRESSION;
        f->b = assignment(p, false);
    }
    p->in_generator = generator;
    p->in_async = was_async;
    return done(p, f);
}

/* ---- Literals ---- */

/* `...${ a }...` - at its first chunk */
static node_t* template_literal(parser_t* p, const node_t* tag)
{
    node_t* t = node(p, DMVS_JS_TEMPLATE);
    if (t == NULL)
        return NULL;
    if (tag != NULL)
    {
        t->start = tag->start;
        t->line = tag->line;
        t->column = tag->column;
        t->b = tag;
    }
    const node_t* head = NULL;
    const node_t* tail = NULL;
    for (;;)
    {
        node_t* chunk = node(p, DMVS_JS_STRING);
        if (chunk == NULL)
            return NULL;
        chunk->text = p->lx.tok.text;
        chunk->length = p->lx.tok.length;
        append(&head, &tail, chunk);
        if (p->lx.tok.tail)
        {
            advance(p);
            break;
        }
        advance(p);
        append(&head, &tail, expression(p, false));
        if (failed(p))
            return NULL;
        if (!is(p, "}"))
        {
            fail(p, "expected '}' in a template");
            return NULL;
        }
        lex_template_continue(&p->lx);
        if (failed(p))
            return NULL;
    }
    t->a = head;
    return done(p, t);
}

static node_t* array_literal(parser_t* p)
{
    node_t* a = node(p, DMVS_JS_ARRAY);
    const node_t* head = NULL;
    const node_t* tail = NULL;
    advance(p);                                     /* [ */
    while (!failed(p) && !is(p, "]"))
    {
        if (is(p, ","))
        {
            append(&head, &tail, done(p, node(p, DMVS_JS_HOLE)));
            advance(p);
            continue;
        }
        if (is(p, "..."))
        {
            node_t* s = node(p, DMVS_JS_SPREAD);
            advance(p);
            if (s != NULL)
                s->a = assignment(p, false);
            append(&head, &tail, done(p, s));
        }
        else
            append(&head, &tail, assignment(p, false));
        if (!accept(p, ","))
            break;
    }
    expect(p, "]");
    if (a != NULL)
        a->a = head;
    return done(p, a);
}

/* A property's key: a name (any, reserved words too), a string, a number, [ computed ] */
static node_t* property_key(parser_t* p, uint16_t* flags)
{
    node_t* k = NULL;
    if (accept(p, "["))
    {
        *flags |= DMVS_JS_F_COMPUTED;
        k = assignment(p, false);
        expect(p, "]");
        return k;
    }
    switch (p->lx.tok.kind)
    {
        case T_NAME:
        case T_PRIVATE:
            k = node(p, DMVS_JS_IDENT);
            take_text(p, k);
            break;
        case T_STRING:
            k = node(p, DMVS_JS_STRING);
            if (k != NULL)
            {
                k->text = p->lx.tok.text;
                k->length = p->lx.tok.length;
            }
            break;
        case T_NUMBER:
            k = node(p, DMVS_JS_NUMBER);
            if (k != NULL)
            {
                take_text(p, k);
                k->number = p->lx.tok.number;
                k->decimals = p->lx.tok.decimals;
            }
            break;
        default:
            fail(p, "expected a property name");
            return NULL;
    }
    advance(p);
    return done(p, k);
}

/* get / set / async / static before a key - when a key follows (not "get: 1", "get() {}") */
static __attribute__((noinline)) bool modifier(parser_t* p, const char* word)
{
    if (!tok_name(&p->lx, word))
        return false;
    lexer_t saved = p->lx;
    dmvs_js_error_t error = *p->lx.error;
    lex_next(&p->lx);
    bool key = !p->lx.tok.newline && !(is(p, ",") || is(p, ":") || is(p, "(") || is(p, "}") || is(p, "=") || is(p, ";")) &&
               p->lx.tok.kind != T_EOF;
    p->lx = saved;
    *p->lx.error = error;
    if (key)
        advance(p);
    return key;
}

/* A method's FUNCTION after its key (flags: async, generator, get, set) */
static node_t* method(parser_t* p, uint16_t flags)
{
    node_t* f = node(p, DMVS_JS_FUNCTION);
    if (f == NULL)
        return NULL;
    f->flags = DMVS_JS_F_METHOD | (flags & (DMVS_JS_F_ASYNC | DMVS_JS_F_GENERATOR | DMVS_JS_F_GETTER | DMVS_JS_F_SETTER));
    return function_rest(p, f, true);
}

static node_t* object_literal(parser_t* p)
{
    node_t* o = node(p, DMVS_JS_OBJECT);
    const node_t* head = NULL;
    const node_t* tail = NULL;
    advance(p);                                     /* { */
    while (!failed(p) && !is(p, "}"))
    {
        if (is(p, "..."))
        {
            node_t* s = node(p, DMVS_JS_SPREAD);
            advance(p);
            if (s != NULL)
                s->a = assignment(p, false);
            append(&head, &tail, done(p, s));
        }
        else
        {
            node_t* prop = node(p, DMVS_JS_PROPERTY);
            uint16_t flags = 0;
            if (modifier(p, "async"))
                flags |= DMVS_JS_F_ASYNC;
            if (accept(p, "*"))
                flags |= DMVS_JS_F_GENERATOR;
            if (flags == 0 && modifier(p, "get"))
                flags |= DMVS_JS_F_GETTER;
            else if (flags == 0 && modifier(p, "set"))
                flags |= DMVS_JS_F_SETTER;
            bool name = p->lx.tok.kind == T_NAME;
            bool usable = at_identifier(p);
            node_t* key = property_key(p, &flags);
            if (prop == NULL || failed(p))
                return NULL;
            prop->a = key;
            if (is(p, "("))
            {
                prop->b = method(p, flags);
                flags |= DMVS_JS_F_METHOD;
            }
            else if (accept(p, ":"))
                prop->b = assignment(p, false);
            else if (name && usable && (flags & ~DMVS_JS_F_COMPUTED) == 0)
            {
                flags |= DMVS_JS_F_SHORTHAND;
                prop->b = key;
                if (is(p, "="))                     /* { a = 1 }: only as a pattern */
                {
                    node_t* d = node_from(p, DMVS_JS_ASSIGN, key);
                    advance(p);
                    if (d != NULL)
                    {
                        d->op = DMVS_JS_OP_ASSIGN;
                        d->a = key;
                        d->b = assignment(p, false);
                    }
                    prop->b = done(p, d);
                }
            }
            else
            {
                fail(p, "expected ':'");
                return NULL;
            }
            prop->flags = flags;
            append(&head, &tail, done(p, prop));
        }
        if (!accept(p, ","))
            break;
    }
    expect(p, "}");
    if (o != NULL)
        o->a = head;
    return done(p, o);
}

/* ---- Expressions ---- */

static node_t* primary(parser_t* p)
{
    token_t t = p->lx.tok;
    node_t* n = NULL;
    switch (t.kind)
    {
        case T_NUMBER:
            n = node(p, DMVS_JS_NUMBER);
            if (n != NULL)
            {
                take_text(p, n);
                n->number = t.number;
                n->decimals = t.decimals;
                if (t.inexact)
                    n->flags |= DMVS_JS_F_INEXACT;
            }
            advance(p);
            return n;
        case T_STRING:
            n = node(p, DMVS_JS_STRING);
            if (n != NULL)
            {
                n->text = t.text;
                n->length = t.length;
            }
            advance(p);
            return n;
        case T_TEMPLATE:
            return template_literal(p, NULL);
        case T_REGEX:
            n = node(p, DMVS_JS_REGEX);
            take_text(p, n);
            advance(p);
            return n;
        case T_PRIVATE:
            n = node(p, DMVS_JS_IDENT);             /* #x in object */
            take_text(p, n);
            advance(p);
            return n;
        case T_PUNCT:
            if (is(p, "("))
            {
                if (arrow_ahead(p))
                    return arrow(p, &t, false);
                advance(p);
                n = expression(p, false);
                expect(p, ")");
                return n;
            }
            if (is(p, "["))
                return array_literal(p);
            if (is(p, "{"))
                return object_literal(p);
            break;
        case T_NAME:
            if (tok_name(&p->lx, "this"))
            {
                n = node(p, DMVS_JS_THIS);
                advance(p);
                return n;
            }
            if (tok_name(&p->lx, "super"))
            {
                n = node(p, DMVS_JS_IDENT);         /* super(...), super.m: a name the compiler knows */
                take_text(p, n);
                advance(p);
                if (!is(p, "(") && !is(p, ".") && !is(p, "["))
                    fail(p, "expected super(...) or super.name");
                return n;
            }
            if (tok_name(&p->lx, "null"))
            {
                n = node(p, DMVS_JS_NULL);
                advance(p);
                return n;
            }
            if (tok_name(&p->lx, "true") || tok_name(&p->lx, "false"))
            {
                n = node(p, DMVS_JS_BOOL);
                if (n != NULL)
                    n->op = tok_name(&p->lx, "true") ? 1u : 0u;
                advance(p);
                return n;
            }
            if (tok_name(&p->lx, "function"))
                return function_expression(p, &t, false, false);
            if (tok_name(&p->lx, "class"))
                return class_rest(p, &t, false);
            if (tok_name(&p->lx, "async") && !next_is(p, "=>", true))
            {
                if (next_is(p, "function", true))
                {
                    advance(p);
                    return function_expression(p, &t, true, false);
                }
                if (async_arrow_ahead(p))
                {
                    advance(p);
                    return arrow(p, &t, true);
                }
            }
            if (at_identifier(p))
            {
                if (next_is(p, "=>", true))
                    return arrow(p, &t, false);
                return identifier(p);
            }
            break;
        default:
            break;
    }
    fail(p, (t.kind == T_EOF) ? "unexpected end of the script" : "unexpected token");
    return NULL;
}

/* Arguments: ( a, ...b ) */
static const node_t* arguments(parser_t* p)
{
    const node_t* head = NULL;
    const node_t* tail = NULL;
    expect(p, "(");
    while (!failed(p) && !is(p, ")"))
    {
        if (is(p, "..."))
        {
            node_t* s = node(p, DMVS_JS_SPREAD);
            advance(p);
            if (s != NULL)
                s->a = assignment(p, false);
            append(&head, &tail, done(p, s));
        }
        else
            append(&head, &tail, assignment(p, false));
        if (!accept(p, ","))
            break;
    }
    expect(p, ")");
    return head;
}

/* .name, ?.name, [ expr ], ( args ), `tagged` after `object`; calls: false within new X ... ( ) */
static node_t* suffixes(parser_t* p, node_t* object, bool calls)
{
    while (!failed(p) && object != NULL)
    {
        bool optional = false;
        if (is(p, "?."))
        {
            optional = true;
            advance(p);
            if (is(p, "(") && calls)
            {
                node_t* c = node_from(p, DMVS_JS_CALL, object);
                if (c != NULL)
                {
                    c->flags = DMVS_JS_F_OPTIONAL;
                    c->a = object;
                    c->b = arguments(p);
                }
                object = done(p, c);
                continue;
            }
            if (!is(p, "["))
            {
                node_t* m = node_from(p, DMVS_JS_MEMBER, object);
                node_t* name = node(p, DMVS_JS_IDENT);
                if (p->lx.tok.kind != T_NAME && p->lx.tok.kind != T_PRIVATE)
                {
                    fail(p, "expected a property name");
                    return NULL;
                }
                take_text(p, name);
                advance(p);
                if (m != NULL)
                {
                    m->flags = DMVS_JS_F_OPTIONAL;
                    m->a = object;
                    m->b = name;
                }
                object = done(p, m);
                continue;
            }
        }
        if (accept(p, "."))
        {
            if (p->lx.tok.kind != T_NAME && p->lx.tok.kind != T_PRIVATE)
            {
                fail(p, "expected a property name");
                return NULL;
            }
            node_t* m = node_from(p, DMVS_JS_MEMBER, object);
            node_t* name = node(p, DMVS_JS_IDENT);
            take_text(p, name);
            advance(p);
            if (m != NULL)
            {
                m->a = object;
                m->b = name;
            }
            object = done(p, m);
        }
        else if (is(p, "["))
        {
            node_t* m = node_from(p, DMVS_JS_MEMBER, object);
            advance(p);
            if (m != NULL)
            {
                m->flags = DMVS_JS_F_COMPUTED | (optional ? DMVS_JS_F_OPTIONAL : 0u);
                m->a = object;
                m->b = expression(p, false);
            }
            expect(p, "]");
            object = done(p, m);
        }
        else if (is(p, "(") && calls)
        {
            node_t* c = node_from(p, DMVS_JS_CALL, object);
            if (c != NULL)
            {
                c->a = object;
                c->b = arguments(p);
            }
            object = done(p, c);
        }
        else if (p->lx.tok.kind == T_TEMPLATE && !optional)
            object = template_literal(p, object);
        else
            break;
    }
    return object;
}

/* new X(args), new X, new.target */
static node_t* new_expression(parser_t* p)
{
    token_t t = p->lx.tok;
    advance(p);                                     /* new */
    if (accept(p, "."))
    {
        node_t* n = node_at(p, DMVS_JS_IDENT, &t);
        if (n != NULL)
        {
            n->text = "new.target";
            n->length = 10;
        }
        if (!tok_name(&p->lx, "target"))
            fail(p, "expected 'target'");
        advance(p);
        return done(p, n);
    }
    node_t* callee = tok_name(&p->lx, "new") ? new_expression(p) : primary(p);
    callee = suffixes(p, callee, false);
    node_t* n = node_at(p, DMVS_JS_NEW, &t);
    if (n == NULL)
        return NULL;
    n->a = callee;
    if (is(p, "("))
        n->b = arguments(p);
    return done(p, n);
}

static node_t* left_hand(parser_t* p)
{
    node_t* n = tok_name(&p->lx, "new") ? new_expression(p) : primary(p);
    return suffixes(p, n, true);
}

static node_t* unary(parser_t* p);

static node_t* postfix(parser_t* p)
{
    node_t* n = left_hand(p);
    if (n != NULL && !p->lx.tok.newline && (is(p, "++") || is(p, "--")))
    {
        if (!valid_target(n))
        {
            fail(p, "invalid ++ / -- target");
            return NULL;
        }
        node_t* u = node_from(p, DMVS_JS_UPDATE, n);
        if (u != NULL)
        {
            u->op = is(p, "++") ? DMVS_JS_OP_INC : DMVS_JS_OP_DEC;
            u->a = n;
        }
        advance(p);
        return done(p, u);
    }
    return n;
}

static node_t* unary(parser_t* p)
{
    uint8_t op = DMVS_JS_OP_NONE;
    if (is(p, "!")) op = DMVS_JS_OP_NOT;
    else if (is(p, "-")) op = DMVS_JS_OP_SUB;
    else if (is(p, "+")) op = DMVS_JS_OP_ADD;
    else if (is(p, "~")) op = DMVS_JS_OP_BITNOT;
    else if (tok_name(&p->lx, "typeof")) op = DMVS_JS_OP_TYPEOF;
    else if (tok_name(&p->lx, "void")) op = DMVS_JS_OP_VOID;
    else if (tok_name(&p->lx, "delete")) op = DMVS_JS_OP_DELETE;
    else if (is(p, "++")) op = DMVS_JS_OP_INC;
    else if (is(p, "--")) op = DMVS_JS_OP_DEC;
    else if (p->in_async && tok_name(&p->lx, "await"))
    {
        node_t* a = node(p, DMVS_JS_AWAIT);
        advance(p);
        if (!enter(p))
            return NULL;
        if (a != NULL)
            a->a = unary(p);
        leave(p);
        return done(p, a);
    }
    if (op == DMVS_JS_OP_NONE)
        return postfix(p);

    node_t* n = node(p, (op == DMVS_JS_OP_INC || op == DMVS_JS_OP_DEC) ? DMVS_JS_UPDATE : DMVS_JS_UNARY);
    advance(p);
    if (!enter(p))
        return NULL;
    node_t* operand = unary(p);
    leave(p);
    if (n == NULL)
        return NULL;
    n->op = op;
    n->a = operand;
    if (n->kind == DMVS_JS_UPDATE)
    {
        n->flags = DMVS_JS_F_PREFIX;
        if (!valid_target(operand))
            fail(p, "invalid ++ / -- target");
    }
    return done(p, n);
}

/* The binary operator at the token: its op, its precedence (0: none) */
static int binary_op(parser_t* p, bool no_in, uint8_t* op)
{
    const token_t* t = &p->lx.tok;
    if (t->kind == T_NAME)
    {
        if (tok_name(&p->lx, "instanceof")) { *op = DMVS_JS_OP_INSTANCEOF; return 8; }
        if (!no_in && tok_name(&p->lx, "in")) { *op = DMVS_JS_OP_IN; return 8; }
        return 0;
    }
    if (t->kind != T_PUNCT)
        return 0;
    if (is(p, "??")) { *op = DMVS_JS_OP_COALESCE; return 1; }
    if (is(p, "||")) { *op = DMVS_JS_OP_OR; return 2; }
    if (is(p, "&&")) { *op = DMVS_JS_OP_AND; return 3; }
    if (is(p, "|")) { *op = DMVS_JS_OP_BITOR; return 4; }
    if (is(p, "^")) { *op = DMVS_JS_OP_BITXOR; return 5; }
    if (is(p, "&")) { *op = DMVS_JS_OP_BITAND; return 6; }
    if (is(p, "===")) { *op = DMVS_JS_OP_SEQ; return 7; }
    if (is(p, "!==")) { *op = DMVS_JS_OP_SNE; return 7; }
    if (is(p, "==")) { *op = DMVS_JS_OP_EQ; return 7; }
    if (is(p, "!=")) { *op = DMVS_JS_OP_NE; return 7; }
    if (is(p, "<=")) { *op = DMVS_JS_OP_LE; return 8; }
    if (is(p, ">=")) { *op = DMVS_JS_OP_GE; return 8; }
    if (is(p, "<")) { *op = DMVS_JS_OP_LT; return 8; }
    if (is(p, ">")) { *op = DMVS_JS_OP_GT; return 8; }
    if (is(p, "<<")) { *op = DMVS_JS_OP_SHL; return 9; }
    if (is(p, ">>>")) { *op = DMVS_JS_OP_USHR; return 9; }
    if (is(p, ">>")) { *op = DMVS_JS_OP_SHR; return 9; }
    if (is(p, "+")) { *op = DMVS_JS_OP_ADD; return 10; }
    if (is(p, "-")) { *op = DMVS_JS_OP_SUB; return 10; }
    if (is(p, "*")) { *op = DMVS_JS_OP_MUL; return 11; }
    if (is(p, "/")) { *op = DMVS_JS_OP_DIV; return 11; }
    if (is(p, "%")) { *op = DMVS_JS_OP_MOD; return 11; }
    if (is(p, "**")) { *op = DMVS_JS_OP_POW; return 12; }
    return 0;
}

/* Operators of at least `min` precedence, by precedence climbing (** is right-associative) */
static node_t* binary(parser_t* p, int min, bool no_in)
{
    node_t* left = unary(p);
    for (;;)
    {
        uint8_t op = 0;
        int prec = binary_op(p, no_in, &op);
        if (prec == 0 || prec < min || left == NULL || failed(p))
            return left;
        advance(p);
        if (!enter(p))
            return NULL;
        node_t* right = binary(p, (op == DMVS_JS_OP_POW) ? prec : prec + 1, no_in);
        leave(p);
        uint8_t kind = (op == DMVS_JS_OP_AND || op == DMVS_JS_OP_OR || op == DMVS_JS_OP_COALESCE) ? DMVS_JS_LOGICAL : DMVS_JS_BINARY;
        node_t* n = node_from(p, kind, left);
        if (n == NULL)
            return NULL;
        n->op = op;
        n->a = left;
        n->b = right;
        left = done(p, n);
    }
}

static node_t* conditional(parser_t* p, bool no_in)
{
    node_t* test = binary(p, 1, no_in);
    if (test == NULL || !is(p, "?"))
        return test;
    node_t* n = node_from(p, DMVS_JS_CONDITIONAL, test);
    advance(p);
    if (n == NULL)
        return NULL;
    n->a = test;
    n->b = assignment(p, false);
    expect(p, ":");
    n->c = assignment(p, no_in);
    return done(p, n);
}

/* The assignment operator at the token: its op (DMVS_JS_OP_ASSIGN for =), 0 for none */
static uint8_t assign_op(parser_t* p)
{
    if (p->lx.tok.kind != T_PUNCT)
        return 0;
    if (is(p, "=")) return DMVS_JS_OP_ASSIGN;
    if (is(p, "+=")) return DMVS_JS_OP_ADD;
    if (is(p, "-=")) return DMVS_JS_OP_SUB;
    if (is(p, "*=")) return DMVS_JS_OP_MUL;
    if (is(p, "/=")) return DMVS_JS_OP_DIV;
    if (is(p, "%=")) return DMVS_JS_OP_MOD;
    if (is(p, "**=")) return DMVS_JS_OP_POW;
    if (is(p, "<<=")) return DMVS_JS_OP_SHL;
    if (is(p, ">>=")) return DMVS_JS_OP_SHR;
    if (is(p, ">>>=")) return DMVS_JS_OP_USHR;
    if (is(p, "&=")) return DMVS_JS_OP_BITAND;
    if (is(p, "|=")) return DMVS_JS_OP_BITOR;
    if (is(p, "^=")) return DMVS_JS_OP_BITXOR;
    if (is(p, "&&=")) return DMVS_JS_OP_AND;
    if (is(p, "||=")) return DMVS_JS_OP_OR;
    if (is(p, "?\?=")) return DMVS_JS_OP_COALESCE;
    return 0;
}

static node_t* assignment(parser_t* p, bool no_in)
{
    if (!enter(p))
        return NULL;
    if (p->in_generator && tok_name(&p->lx, "yield"))
    {
        node_t* y = node(p, DMVS_JS_YIELD);
        advance(p);
        if (y != NULL && accept(p, "*"))
            y->flags = DMVS_JS_F_DELEGATE;
        if (y != NULL && !p->lx.tok.newline && !is(p, ")") && !is(p, "]") && !is(p, "}") && !is(p, ",") &&
            !is(p, ";") && !is(p, ":") && p->lx.tok.kind != T_EOF)
            y->a = assignment(p, no_in);
        leave(p);
        return done(p, y);
    }
    node_t* left = conditional(p, no_in);
    uint8_t op = (left != NULL) ? assign_op(p) : 0u;
    if (op != 0)
    {
        if (op == DMVS_JS_OP_ASSIGN)
            mark_pattern(left);
        if (!valid_target(left))
        {
            fail(p, "invalid assignment target");
            leave(p);
            return NULL;
        }
        node_t* n = node_from(p, DMVS_JS_ASSIGN, left);
        advance(p);
        if (n != NULL)
        {
            n->op = op;
            n->a = left;
            n->b = assignment(p, no_in);
        }
        left = done(p, n);
    }
    leave(p);
    return left;
}

static node_t* expression(parser_t* p, bool no_in)
{
    node_t* first = assignment(p, no_in);
    if (first == NULL || !is(p, ","))
        return first;
    node_t* seq = node_from(p, DMVS_JS_SEQUENCE, first);
    const node_t* head = NULL;
    const node_t* tail = NULL;
    append(&head, &tail, first);
    while (!failed(p) && accept(p, ","))
        append(&head, &tail, assignment(p, no_in));
    if (seq != NULL)
        seq->a = head;
    return done(p, seq);
}

/* ---- Classes ---- */

static node_t* class_rest(parser_t* p, const token_t* start, bool declaration)
{
    node_t* c = node_at(p, DMVS_JS_CLASS, start);
    advance(p);                                     /* class */
    if (c == NULL)
        return NULL;
    if (declaration)
        c->flags |= DMVS_JS_F_DECLARATION;
    if (at_identifier(p))
    {
        take_text(p, c);
        advance(p);
    }
    else if (declaration)
        fail(p, "expected the class's name");
    if (tok_name(&p->lx, "extends"))
    {
        advance(p);
        c->a = left_hand(p);
    }
    expect(p, "{");
    const node_t* head = NULL;
    const node_t* tail = NULL;
    while (!failed(p) && !is(p, "}") && p->lx.tok.kind != T_EOF)
    {
        if (accept(p, ";"))
            continue;
        node_t* m = node(p, DMVS_JS_FIELD);
        uint16_t flags = 0;
        if (modifier(p, "static"))
            flags |= DMVS_JS_F_STATIC;
        if (modifier(p, "async"))
            flags |= DMVS_JS_F_ASYNC;
        if (accept(p, "*"))
            flags |= DMVS_JS_F_GENERATOR;
        if ((flags & (DMVS_JS_F_ASYNC | DMVS_JS_F_GENERATOR)) == 0 && modifier(p, "get"))
            flags |= DMVS_JS_F_GETTER;
        else if ((flags & (DMVS_JS_F_ASYNC | DMVS_JS_F_GENERATOR)) == 0 && modifier(p, "set"))
            flags |= DMVS_JS_F_SETTER;
        node_t* key = property_key(p, &flags);
        if (m == NULL || failed(p))
            return NULL;
        m->a = key;
        if (is(p, "("))
        {
            m->b = method(p, flags);
            flags |= DMVS_JS_F_METHOD;
        }
        else
        {
            if (accept(p, "="))
                m->b = assignment(p, false);
            semicolon(p);
        }
        m->flags = flags & (DMVS_JS_F_STATIC | DMVS_JS_F_COMPUTED | DMVS_JS_F_METHOD | DMVS_JS_F_GETTER | DMVS_JS_F_SETTER);
        append(&head, &tail, done(p, m));
    }
    expect(p, "}");
    c->b = head;
    return done(p, c);
}

/* ---- Statements ---- */

/* var / let / const a = 1, b - at the keyword */
static node_t* declarations(parser_t* p, bool no_in)
{
    node_t* v = node(p, DMVS_JS_VAR);
    if (v == NULL)
        return NULL;
    v->op = tok_name(&p->lx, "var") ? DMVS_JS_VAR_VAR : tok_name(&p->lx, "let") ? DMVS_JS_VAR_LET : DMVS_JS_VAR_CONST;
    advance(p);
    const node_t* head = NULL;
    const node_t* tail = NULL;
    do
    {
        node_t* d = node(p, DMVS_JS_DECLARATOR);
        node_t* target = binding(p);
        if (d == NULL || failed(p))
            return NULL;
        d->a = target;
        if (accept(p, "="))
            d->b = assignment(p, no_in);
        append(&head, &tail, done(p, d));
    } while (!failed(p) && accept(p, ","));
    v->a = head;
    return done(p, v);
}

/* let as a declaration (not a variable named let) */
static __attribute__((noinline)) bool at_let(parser_t* p)
{
    if (!tok_name(&p->lx, "let"))
        return false;
    lexer_t saved = p->lx;
    dmvs_js_error_t error = *p->lx.error;
    lex_next(&p->lx);
    bool yes = p->lx.tok.kind == T_NAME || is(p, "[") || is(p, "{");
    p->lx = saved;
    *p->lx.error = error;
    return yes;
}

static node_t* block(parser_t* p)
{
    return body(p);                                 /* { statements } */
}

/* if (test) then [else other] */
static node_t* if_statement(parser_t* p)
{
    node_t* n = node(p, DMVS_JS_IF);
    advance(p);
    expect(p, "(");
    node_t* test = expression(p, false);
    expect(p, ")");
    node_t* then = statement(p);
    node_t* other = NULL;
    if (tok_name(&p->lx, "else"))
    {
        advance(p);
        other = statement(p);
    }
    if (n == NULL)
        return NULL;
    n->a = test;
    n->b = then;
    n->c = other;
    return done(p, n);
}

static node_t* for_statement(parser_t* p)
{
    token_t t = p->lx.tok;
    advance(p);                                     /* for */
    bool await = p->in_async && tok_name(&p->lx, "await");
    if (await)
        advance(p);
    expect(p, "(");
    node_t* init = NULL;
    if (!is(p, ";"))
    {
        if (tok_name(&p->lx, "var") || tok_name(&p->lx, "const") || at_let(p))
            init = declarations(p, true);
        else
            init = expression(p, true);
    }
    if (failed(p))
        return NULL;
    if (tok_name(&p->lx, "of") || tok_name(&p->lx, "in"))
    {
        bool of = tok_name(&p->lx, "of");
        node_t* n = node_at(p, of ? DMVS_JS_FOR_OF : DMVS_JS_FOR_IN, &t);
        advance(p);
        if (init != NULL && init->kind != DMVS_JS_VAR)
        {
            mark_pattern(init);
            if (!valid_target(init))
                fail(p, "invalid for target");
        }
        node_t* right = of ? assignment(p, false) : expression(p, false);
        expect(p, ")");
        node_t* body_statement = statement(p);
        if (n == NULL)
            return NULL;
        n->a = init;
        n->b = right;
        n->c = body_statement;
        return done(p, n);
    }
    node_t* n = node_at(p, DMVS_JS_FOR, &t);
    expect(p, ";");
    node_t* test = is(p, ";") ? NULL : expression(p, false);
    expect(p, ";");
    node_t* update = is(p, ")") ? NULL : expression(p, false);
    expect(p, ")");
    node_t* body_statement = statement(p);
    if (n == NULL)
        return NULL;
    n->a = init;
    n->b = test;
    n->c = update;
    n->d = body_statement;
    return done(p, n);
}

static node_t* switch_statement(parser_t* p)
{
    node_t* n = node(p, DMVS_JS_SWITCH);
    advance(p);
    expect(p, "(");
    node_t* discriminant = expression(p, false);
    expect(p, ")");
    expect(p, "{");
    const node_t* head = NULL;
    const node_t* tail = NULL;
    while (!failed(p) && !is(p, "}"))
    {
        node_t* c = node(p, DMVS_JS_CASE);
        if (tok_name(&p->lx, "case"))
        {
            advance(p);
            if (c != NULL)
                c->a = expression(p, false);
        }
        else if (tok_name(&p->lx, "default"))
            advance(p);
        else
        {
            fail(p, "expected 'case' or 'default'");
            return NULL;
        }
        expect(p, ":");
        const node_t* sh = NULL;
        const node_t* st = NULL;
        while (!failed(p) && !is(p, "}") && !tok_name(&p->lx, "case") && !tok_name(&p->lx, "default") &&
               p->lx.tok.kind != T_EOF)
            append(&sh, &st, statement(p));
        if (c != NULL)
            c->b = sh;
        append(&head, &tail, done(p, c));
    }
    expect(p, "}");
    if (n == NULL)
        return NULL;
    n->a = discriminant;
    n->b = head;
    return done(p, n);
}

static node_t* try_statement(parser_t* p)
{
    node_t* n = node(p, DMVS_JS_TRY);
    advance(p);
    node_t* tried = block(p);
    node_t* param = NULL;
    node_t* handler = NULL;
    node_t* finalizer = NULL;
    if (tok_name(&p->lx, "catch"))
    {
        advance(p);
        if (accept(p, "("))
        {
            param = binding(p);
            expect(p, ")");
        }
        handler = block(p);
    }
    if (tok_name(&p->lx, "finally"))
    {
        advance(p);
        finalizer = block(p);
    }
    if (handler == NULL && finalizer == NULL && !failed(p))
        fail(p, "expected 'catch' or 'finally'");
    if (n == NULL)
        return NULL;
    n->a = tried;
    n->b = param;
    n->c = handler;
    n->d = finalizer;
    return done(p, n);
}

/* return / throw [value]; break / continue [label]; - nothing after a line break */
static node_t* jump(parser_t* p, uint8_t kind)
{
    node_t* n = node(p, kind);
    advance(p);
    bool more = !p->lx.tok.newline && !is(p, ";") && !is(p, "}") && p->lx.tok.kind != T_EOF;
    if (n == NULL)
        return NULL;
    if (kind == DMVS_JS_BREAK || kind == DMVS_JS_CONTINUE)
    {
        if (more && at_identifier(p))
        {
            take_text(p, n);
            advance(p);
        }
    }
    else if (more)
        n->a = expression(p, false);
    else if (kind == DMVS_JS_THROW)
        fail(p, "expected what is thrown");
    semicolon(p);
    return done(p, n);
}

static node_t* statement_inner(parser_t* p)
{
    token_t t = p->lx.tok;
    if (t.kind == T_PUNCT)
    {
        if (is(p, "{"))
            return block(p);
        if (is(p, ";"))
        {
            node_t* n = node(p, DMVS_JS_EMPTY);
            advance(p);
            return n;
        }
    }
    else if (t.kind == T_NAME)
    {
        if (tok_name(&p->lx, "var") || tok_name(&p->lx, "const") || at_let(p))
        {
            node_t* n = declarations(p, false);
            semicolon(p);
            return done(p, n);
        }
        if (tok_name(&p->lx, "function"))
            return function_expression(p, &t, false, true);
        if (tok_name(&p->lx, "async") && next_is(p, "function", true))
        {
            advance(p);
            return function_expression(p, &t, true, true);
        }
        if (tok_name(&p->lx, "class"))
            return class_rest(p, &t, true);
        if (tok_name(&p->lx, "if"))
            return if_statement(p);
        if (tok_name(&p->lx, "for"))
            return for_statement(p);
        if (tok_name(&p->lx, "while"))
        {
            node_t* n = node(p, DMVS_JS_WHILE);
            advance(p);
            expect(p, "(");
            node_t* test = expression(p, false);
            expect(p, ")");
            node_t* loop = statement(p);
            if (n == NULL)
                return NULL;
            n->a = test;
            n->b = loop;
            return done(p, n);
        }
        if (tok_name(&p->lx, "do"))
        {
            node_t* n = node(p, DMVS_JS_DO_WHILE);
            advance(p);
            node_t* loop = statement(p);
            if (!tok_name(&p->lx, "while"))
            {
                fail(p, "expected 'while'");
                return NULL;
            }
            advance(p);
            expect(p, "(");
            node_t* test = expression(p, false);
            expect(p, ")");
            (void)accept(p, ";");
            if (n == NULL)
                return NULL;
            n->a = loop;
            n->b = test;
            return done(p, n);
        }
        if (tok_name(&p->lx, "return"))
            return jump(p, DMVS_JS_RETURN);
        if (tok_name(&p->lx, "throw"))
            return jump(p, DMVS_JS_THROW);
        if (tok_name(&p->lx, "break"))
            return jump(p, DMVS_JS_BREAK);
        if (tok_name(&p->lx, "continue"))
            return jump(p, DMVS_JS_CONTINUE);
        if (tok_name(&p->lx, "switch"))
            return switch_statement(p);
        if (tok_name(&p->lx, "try"))
            return try_statement(p);
        if (tok_name(&p->lx, "debugger"))
        {
            node_t* n = node(p, DMVS_JS_EMPTY);
            advance(p);
            semicolon(p);
            return n;
        }
        if (at_identifier(p) && next_is(p, ":", false))
        {
            node_t* n = node(p, DMVS_JS_LABELED);
            take_text(p, n);
            advance(p);
            advance(p);                             /* : */
            node_t* labeled = statement(p);
            if (n == NULL)
                return NULL;
            n->a = labeled;
            return done(p, n);
        }
    }

    node_t* n = node(p, DMVS_JS_EXPRESSION);
    node_t* e = expression(p, false);
    semicolon(p);
    if (n == NULL)
        return NULL;
    n->a = e;
    return done(p, n);
}

static node_t* statement(parser_t* p)
{
    if (!enter(p))
        return NULL;
    node_t* n = statement_inner(p);
    leave(p);
    return n;
}

/* ---- The program ---- */

node_t* parse_program(arena_t* arena, const char* source, size_t length, dmvs_js_error_t* error)
{
    parser_t* p = Dmod_Malloc(sizeof(*p));          /* Not on the stack of a small target */
    if (p == NULL)
    {
        error->status = -ENOMEM;
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    lex_init(&p->lx, source, length, arena, error);

    token_t start = p->lx.tok;
    start.start = 0;
    start.line = 1;
    start.column = 1;
    node_t* program = node_at(p, DMVS_JS_PROGRAM, &start);
    const node_t* head = NULL;
    const node_t* tail = NULL;
    while (!failed(p) && p->lx.tok.kind != T_EOF)
        append(&head, &tail, statement(p));
    if (program != NULL)
    {
        program->a = head;
        program->end = (uint32_t)length;
    }
    bool ok = !failed(p) && !arena->failed;
    Dmod_Free(p);
    return ok ? program : NULL;
}

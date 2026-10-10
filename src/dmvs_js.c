#define DMOD_ENABLE_REGISTRATION ON
#include "private.h"

/*
 * dmvs_js - the API: parsing into a tree that owns its memory (an arena of
 * chunks), the tree as an S-expression, the names of kinds and operators.
 */

#define CHUNK_SIZE      (16u * 1024u)

struct dmvs_js_ast
{
    arena_t                 arena;
    const dmvs_js_node_t*   root;
    dmvs_js_info_t          info;
};

/* ---- Arena ---- */

void* arena_alloc(arena_t* a, size_t size)
{
    size = (size + 7u) & ~(size_t)7u;
    chunk_t* c = a->chunks;
    if (c == NULL || c->size - c->used < size)
    {
        size_t room = (size > CHUNK_SIZE) ? size : CHUNK_SIZE;
        c = Dmod_Malloc(sizeof(chunk_t) + room);
        if (c == NULL)
        {
            a->failed = true;
            return NULL;
        }
        c->next = a->chunks;
        c->used = 0;
        c->size = room;
        a->chunks = c;
    }
    uint8_t* p = (uint8_t*)(c + 1) + c->used;
    c->used += size;
    memset(p, 0, size);
    return p;
}

char* arena_strndup(arena_t* a, const char* s, size_t length)
{
    char* out = arena_alloc(a, length + 1U);
    if (out != NULL)
    {
        memcpy(out, s, length);
        out[length] = '\0';
    }
    return out;
}

void arena_release(arena_t* a)
{
    chunk_t* c = a->chunks;
    while (c != NULL)
    {
        chunk_t* next = c->next;
        Dmod_Free(c);
        c = next;
    }
    a->chunks = NULL;
}

/* ---- Names: text, not pointers ---- */

#define NAME_WIDTH  12u

static const char g_kinds[][NAME_WIDTH] = {
    "?", "program", "var", "declarator", "function", "return", "if", "for", "for-in", "for-of", "while",
    "do-while", "break", "continue", "switch", "case", "block", "expression", "empty", "throw", "try",
    "labeled", "class", "field", "?", "?", "?", "?", "?", "?", "?", "?",
    "ident", "number", "string", "template", "regex", "bool", "null", "this", "array", "object",
    "property", "unary", "update", "binary", "logical", "assign", "conditional", "call", "new", "member",
    "sequence", "spread", "hole", "await", "yield",
};

static const char g_ops[][NAME_WIDTH] = {
    "?", "+", "-", "*", "/", "%", "**", "<<", ">>", ">>>", "&", "|", "^", "==", "!=", "===", "!==",
    "<", "<=", ">", ">=", "in", "instanceof", "&&", "||", "??", "!", "~", "typeof", "void", "delete",
    "++", "--", "=",
};

static const char g_flags[][NAME_WIDTH] = {
    "computed", "optional", "prefix", "shorthand", "method", "get", "set", "arrow",
    "expr", "async", "generator", "static", "delegate", "pattern", "decl", "inexact",
};

dmod_dmvs_js_api_declaration(1.0, const char*, _kind_name, ( uint8_t kind ))
{
    return (kind < sizeof(g_kinds) / sizeof(g_kinds[0])) ? g_kinds[kind] : g_kinds[0];
}

dmod_dmvs_js_api_declaration(1.0, const char*, _op_name, ( uint8_t op ))
{
    return (op < sizeof(g_ops) / sizeof(g_ops[0])) ? g_ops[op] : g_ops[0];
}

/* ---- Parsing ---- */

/* Nodes of a tree */
static uint32_t count(const dmvs_js_node_t* n, uint32_t depth)
{
    uint32_t total = 0;
    for (; n != NULL && depth < 256u; n = n->next)
        total += 1u + count(n->a, depth + 1U) + count(n->b, depth + 1U) + count(n->c, depth + 1U) + count(n->d, depth + 1U);
    return total;
}

static dmvs_js_ast_t parse(source_t* source, dmvs_js_error_t* error)
{
    struct dmvs_js_ast* ast = Dmod_Malloc(sizeof(*ast));
    if (ast == NULL)
    {
        error->status = -ENOMEM;
        return NULL;
    }
    memset(ast, 0, sizeof(*ast));
    ast->root = parse_program(&ast->arena, source, error);
    if (ast->root == NULL)
    {
        if (error->status == 0)
            error->status = -ENOMEM;
        arena_release(&ast->arena);
        Dmod_Free(ast);
        return NULL;
    }
    ast->info.source = ast->root->end;
    ast->info.window = source->peak;
    for (const chunk_t* c = ast->arena.chunks; c != NULL; c = c->next)
        ast->info.tree += (uint32_t)(sizeof(chunk_t) + c->size);
    ast->info.nodes = count(ast->root, 0);
    return ast;
}

dmod_dmvs_js_api_declaration(1.0, dmvs_js_ast_t, _parse, ( const char* source, size_t length, dmvs_js_error_t* error ))
{
    dmvs_js_error_t ignored;
    if (error == NULL)
        error = &ignored;
    memset(error, 0, sizeof(*error));
    if (source == NULL || length > 0x7FFFFFFFu)
    {
        error->status = -EINVAL;
        return NULL;
    }
    source_t s;
    source_memory(&s, source, length);
    return parse(&s, error);
}

dmod_dmvs_js_api_declaration(1.0, dmvs_js_ast_t, _parse_stream, ( dmvs_js_read_fn read, void* ctx, dmvs_js_error_t* error ))
{
    dmvs_js_error_t ignored;
    if (error == NULL)
        error = &ignored;
    memset(error, 0, sizeof(*error));
    if (read == NULL)
    {
        error->status = -EINVAL;
        return NULL;
    }
    source_t s;
    source_stream(&s, read, ctx);
    dmvs_js_ast_t ast = parse(&s, error);
    source_release(&s);
    return ast;
}

dmod_dmvs_js_api_declaration(1.0, int, _info, ( dmvs_js_ast_t ast, dmvs_js_info_t* info ))
{
    if (ast == NULL || info == NULL)
        return -EINVAL;
    *info = ast->info;
    return 0;
}

dmod_dmvs_js_api_declaration(1.0, const dmvs_js_node_t*, _root, ( dmvs_js_ast_t ast ))
{
    return (ast != NULL) ? ast->root : NULL;
}

dmod_dmvs_js_api_declaration(1.0, void, _free, ( dmvs_js_ast_t ast ))
{
    if (ast == NULL)
        return;
    arena_release(&ast->arena);
    Dmod_Free(ast);
}

/* ---- Dumping ---- */

typedef struct
{
    char*   out;
    size_t  size;
    size_t  n;                      /* Bytes of all of it */
} writer_t;

static void put(writer_t* w, const char* s, size_t length)
{
    for (size_t i = 0; i < length; i++, w->n++)
    {
        if (w->n + 1U < w->size)
            w->out[w->n] = s[i];
    }
}

static void puts_(writer_t* w, const char* s)
{
    put(w, s, strlen(s));
}

static void put_int(writer_t* w, uint64_t v)
{
    char digits[24];
    size_t n = 0;
    do
    {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n > 0)
        put(w, &digits[--n], 1);
}

/* number / 10^decimals in decimal */
static void put_number(writer_t* w, int64_t number, uint8_t decimals)
{
    uint64_t v = (number < 0) ? (uint64_t)(-(number + 1)) + 1u : (uint64_t)number;
    if (number < 0)
        puts_(w, "-");
    if (decimals > 19u)
    {
        puts_(w, "0.");                     /* Smaller than 1e-19: its digits after the zeros */
        for (uint8_t i = 19; i < decimals; i++)
            puts_(w, "0");
        decimals = 19;
    }
    uint64_t scale = 1;
    for (uint8_t i = 0; i < decimals; i++)
        scale *= 10u;
    put_int(w, v / scale);
    if (decimals == 0)
        return;
    puts_(w, ".");
    uint64_t f = v % scale;
    for (uint64_t s = scale / 10u; s > 0; s /= 10u)
    {
        char d = (char)('0' + (int)((f / s) % 10u));
        put(w, &d, 1);
    }
}

static void put_string(writer_t* w, const char* s, size_t length)
{
    static const char hex[] = "0123456789ABCDEF";
    puts_(w, "\"");
    for (size_t i = 0; i < length; i++)
    {
        uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\')
        {
            char e[2] = { '\\', (char)c };
            put(w, e, 2);
        }
        else if (c == '\n')
            puts_(w, "\\n");
        else if (c < 0x20u || c == 0x7Fu)
        {
            char e[4] = { '\\', 'x', hex[c >> 4], hex[c & 15u] };
            put(w, e, 4);
        }
        else
            put(w, (const char*)&c, 1);
    }
    puts_(w, "\"");
}

/* Slot `slot` of a kind holds a list (printed as [ ... ]) */
static bool list_slot(uint8_t kind, int slot)
{
    switch (kind)
    {
        case DMVS_JS_PROGRAM: case DMVS_JS_VAR: case DMVS_JS_BLOCK: case DMVS_JS_TEMPLATE: case DMVS_JS_ARRAY:
        case DMVS_JS_OBJECT: case DMVS_JS_SEQUENCE: case DMVS_JS_FUNCTION:
            return slot == 0;               /* A function's: its parameters */
        case DMVS_JS_SWITCH: case DMVS_JS_CASE: case DMVS_JS_CLASS: case DMVS_JS_CALL: case DMVS_JS_NEW:
            return slot == 1;
        default:
            return false;
    }
}

/*
 * Names, numbers, strings, true / false / null / this as they are;
 * everything else as (kind [operator] [flags] ["text"] children), a list
 * of children as [ ... ], a missing child before others as _
 */
static void dump(writer_t* w, const dmvs_js_node_t* n, uint32_t depth)
{
    if (n == NULL)
    {
        puts_(w, "_");
        return;
    }
    if (depth > 200u)
    {
        puts_(w, "...");
        return;
    }
    switch (n->kind)
    {
        case DMVS_JS_IDENT:
            put(w, n->text, n->length);
            return;
        case DMVS_JS_NUMBER:
            put_number(w, n->number, n->decimals);
            if (n->flags & DMVS_JS_F_INEXACT)
                puts_(w, "~");
            return;
        case DMVS_JS_STRING:
            put_string(w, n->text, n->length);
            return;
        case DMVS_JS_BOOL:
            puts_(w, (n->op != 0) ? "true" : "false");
            return;
        case DMVS_JS_NULL:
            puts_(w, "null");
            return;
        case DMVS_JS_THIS:
            puts_(w, "this");
            return;
        case DMVS_JS_HOLE:
            puts_(w, "<hole>");
            return;
        case DMVS_JS_EMPTY:
            puts_(w, "(empty)");
            return;
        default:
            break;
    }

    puts_(w, "(");
    puts_(w, dmvs_js_kind_name(n->kind));
    if (n->kind == DMVS_JS_VAR)
        puts_(w, (n->op == DMVS_JS_VAR_LET) ? " let" : (n->op == DMVS_JS_VAR_CONST) ? " const" : " var");
    else if (n->kind == DMVS_JS_UNARY || n->kind == DMVS_JS_UPDATE || n->kind == DMVS_JS_BINARY ||
             n->kind == DMVS_JS_LOGICAL || n->kind == DMVS_JS_ASSIGN)
    {
        puts_(w, " ");
        puts_(w, dmvs_js_op_name(n->op));
        if (n->kind == DMVS_JS_ASSIGN && n->op != DMVS_JS_OP_ASSIGN)
            puts_(w, "=");
    }
    for (uint32_t i = 0; i < 16u; i++)
    {
        if (n->flags & (1u << i))
        {
            puts_(w, " ");
            puts_(w, g_flags[i]);
        }
    }
    if (n->text != NULL)
    {
        puts_(w, " ");
        if (n->kind == DMVS_JS_REGEX)
            put(w, n->text, n->length);
        else
            put_string(w, n->text, n->length);
    }

    const dmvs_js_node_t* slots[4] = { n->a, n->b, n->c, n->d };
    int last = -1;
    for (int i = 0; i < 4; i++)
        if (slots[i] != NULL)
            last = i;
    for (int i = 0; i <= last; i++)
    {
        puts_(w, " ");
        if (list_slot(n->kind, i))
        {
            puts_(w, "[");
            for (const dmvs_js_node_t* k = slots[i]; k != NULL; k = k->next)
            {
                dump(w, k, depth + 1U);
                if (k->next != NULL)
                    puts_(w, " ");
            }
            puts_(w, "]");
        }
        else
            dump(w, slots[i], depth + 1U);
    }
    puts_(w, ")");
}

dmod_dmvs_js_api_declaration(1.0, size_t, _dump, ( const dmvs_js_node_t* node, char* buffer, size_t size ))
{
    writer_t w = { buffer, (buffer != NULL) ? size : 0u, 0 };
    dump(&w, node, 0);
    if (w.size > 0)
        w.out[(w.n < w.size) ? w.n : w.size - 1U] = '\0';
    return w.n;
}

/* ---- Module ---- */

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}

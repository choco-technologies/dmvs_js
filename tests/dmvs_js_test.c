#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/*
 * The parser: scripts and the trees they make (dmvs_js_dump()), errors
 * and where they are, and two real pages' scripts against the trees acorn
 * makes of them (the .tree files in fixtures, written by acorn/tree.js).
 */

#ifndef DMVS_JS_FIXTURES_DIR
#define DMVS_JS_FIXTURES_DIR "fixtures"
#endif

static char g_tree[16384];
static char g_file[32768];
static char g_expected[32768];

/* The tree of a script; NULL when it is not parsed (error filled in) */
static const char* tree(const char* source, dmvs_js_error_t* error)
{
    dmvs_js_error_t ignored;
    dmvs_js_ast_t ast = dmvs_js_parse(source, strlen(source), (error != NULL) ? error : &ignored);
    if (ast == NULL)
        return NULL;
    dmvs_js_dump(dmvs_js_root(ast), g_tree, sizeof(g_tree));
    dmvs_js_free(ast);
    return g_tree;
}

/* The script makes this program (its statements, without "(program [" ... "])") */
static bool parses(const char* source, const char* statements)
{
    static char expected[4096];
    Dmod_SnPrintf(expected, sizeof(expected), "(program [%s])", statements);
    dmvs_js_error_t e;
    const char* t = tree(source, &e);
    if (t == NULL)
    {
        Dmod_Printf("    %s\n    error %d at %u:%u: %s\n", source, e.status, e.line, e.column, e.message);
        return false;
    }
    if (strcmp(t, expected) != 0)
    {
        Dmod_Printf("    %s\n    is   %s\n    want %s\n", source, t, expected);
        return false;
    }
    return true;
}

/* The script is not parsed: this error, there */
static bool fails(const char* source, int status, uint32_t line, uint32_t column, const char* message)
{
    dmvs_js_error_t e;
    if (tree(source, &e) != NULL)
    {
        Dmod_Printf("    %s: parsed\n", source);
        return false;
    }
    bool ok = e.status == status && e.line == line && e.column == column && strcmp(e.message, message) == 0;
    if (!ok)
        Dmod_Printf("    %s: %d at %u:%u: %s\n", source, e.status, e.line, e.column, e.message);
    return ok;
}

DMOD_TEST_STEP(dmvs_js_parses_declarations_and_expressions)
{
    DMOD_TEST_EXPECT_TRUE(parses("let a = 1, b;", "(var let [(declarator a 1) (declarator b)])"));
    DMOD_TEST_EXPECT_TRUE(parses("x = 1 + 2 * 3 ** 2 ** 2;",
        "(expression (assign = x (binary + 1 (binary * 2 (binary ** 3 (binary ** 2 2))))))"));
    DMOD_TEST_EXPECT_TRUE(parses("x = a ? b : c ? d : e; y = !a && -b || typeof c === 'x';",
        "(expression (assign = x (conditional a b (conditional c d e)))) "
        "(expression (assign = y (logical || (logical && (unary ! a) (unary - b)) (binary === (unary typeof c) \"x\"))))"));
    DMOD_TEST_EXPECT_TRUE(parses("v = a?.b?.[c] ?? f?.(1);",
        "(expression (assign = v (logical ?? (member computed optional (member optional a b) c) (call optional f [1]))))"));
    DMOD_TEST_EXPECT_TRUE(parses("d = new Date(); e = new Foo; g = new a.B(1).c;",
        "(expression (assign = d (new Date))) (expression (assign = e (new Foo))) "
        "(expression (assign = g (member (new (member a B) [1]) c)))"));
    DMOD_TEST_EXPECT_TRUE(parses("let async = 1; a.if = b.class; i += 2;",
        "(var let [(declarator async 1)]) (expression (assign = (member a if) (member b class))) (expression (assign += i 2))"));
}

DMOD_TEST_STEP(dmvs_js_parses_functions_and_classes)
{
    DMOD_TEST_EXPECT_TRUE(parses("const f = (a, b = 2) => a + b;",
        "(var const [(declarator f (function arrow expr [a (assign = b 2)] (binary + a b)))])"));
    DMOD_TEST_EXPECT_TRUE(parses("x => x * 2; async () => {}; f(async x => await x);",
        "(expression (function arrow expr [x] (binary * x 2))) (expression (function arrow async [] (block))) "
        "(expression (call f [(function arrow expr async [x] (await x))]))"));
    DMOD_TEST_EXPECT_TRUE(parses("async function f() { await g(); } function* h() { yield 1; yield* k(); }",
        "(function async decl \"f\" [] (block [(expression (await (call g)))])) "
        "(function generator decl \"h\" [] (block [(expression (yield 1)) (expression (yield delegate (call k)))]))"));
    DMOD_TEST_EXPECT_TRUE(parses("class A extends B { static x = 1; get y() { return 2 } m(a) { super.m(a) } #p = 3; }",
        "(class decl \"A\" B [(field static x 1) (field method get y (function method get [] (block [(return 2)]))) "
        "(field method m (function method [a] (block [(expression (call (member super m) [a]))]))) (field #p 3)])"));
    DMOD_TEST_EXPECT_TRUE(parses("o = { a, b: 1, [k]: 2, ...p, get g() { return 1 }, m(x) {}, 'q': 3, 4: 5 };",
        "(expression (assign = o (object [(property shorthand a a) (property b 1) (property computed k 2) (spread p) "
        "(property method get g (function method get [] (block [(return 1)]))) (property method m (function method [x] (block))) "
        "(property \"q\" 3) (property 4 5)])))"));
}

DMOD_TEST_STEP(dmvs_js_parses_patterns)
{
    DMOD_TEST_EXPECT_TRUE(parses("const { a, b: [c, d = 1], ...rest } = o;",
        "(var const [(declarator (object pattern [(property shorthand a a) (property b (array pattern [c (assign = d 1)])) "
        "(spread rest)]) o)])"));
    DMOD_TEST_EXPECT_TRUE(parses("function f({ a, b = 2 }, [c, ...d], ...e) {} for (const [k, v] of m) {} [a, b] = [b, a];",
        "(function decl \"f\" [(object pattern [(property shorthand a a) (property shorthand b (assign = b 2))]) "
        "(array pattern [c (spread d)]) (spread e)] (block)) "
        "(for-of (var const [(declarator (array pattern [k v]))]) m (block)) "
        "(expression (assign = (array pattern [a b]) (array [b a])))"));
}

DMOD_TEST_STEP(dmvs_js_parses_statements)
{
    DMOD_TEST_EXPECT_TRUE(parses("for (const x of xs) { if (x) continue; } for (k in o) ; for (let i = 0; i < 3; i++) {}",
        "(for-of (var const [(declarator x)]) xs (block [(if x (continue))])) (for-in k o (empty)) "
        "(for (var let [(declarator i 0)]) (binary < i 3) (update ++ i) (block))"));
    DMOD_TEST_EXPECT_TRUE(parses("switch (a) { case 1: b(); break; default: c(); }",
        "(switch a [(case 1 [(expression (call b)) (break)]) (case _ [(expression (call c))])])"));
    DMOD_TEST_EXPECT_TRUE(parses("try { a() } catch (e) { b(e) } finally { c() }",
        "(try (block [(expression (call a))]) e (block [(expression (call b [e]))]) (block [(expression (call c))]))"));
    DMOD_TEST_EXPECT_TRUE(parses("outer: for (;;) { break outer; } while (i < 10) i++",
        "(labeled \"outer\" (for _ _ _ (block [(break \"outer\")]))) (while (binary < i 10) (expression (update ++ i)))"));
    DMOD_TEST_EXPECT_TRUE(parses("if (a) b(); else if (c) d(); else { e() }",
        "(if a (expression (call b)) (if c (expression (call d)) (block [(expression (call e))])))"));
}

DMOD_TEST_STEP(dmvs_js_inserts_semicolons)
{
    DMOD_TEST_EXPECT_TRUE(parses("a = 1\nb = 2", "(expression (assign = a 1)) (expression (assign = b 2))"));
    DMOD_TEST_EXPECT_TRUE(parses("function f(x) { return\nx }", "(function decl \"f\" [x] (block [(return) (expression x)]))"));
    DMOD_TEST_EXPECT_TRUE(parses("a\n++b", "(expression a) (expression (update ++ prefix b))"));
    DMOD_TEST_EXPECT_TRUE(parses("x = a\n(b)", "(expression (assign = x (call a [b])))"));        /* as JavaScript does */
    DMOD_TEST_EXPECT_TRUE(parses("do x++; while (x < 3) y = 1",
        "(do-while (expression (update ++ x)) (binary < x 3)) (expression (assign = y 1))"));
    DMOD_TEST_EXPECT_TRUE(parses("let x = 1 // c\n/* d */ let y = 2", "(var let [(declarator x 1)]) (var let [(declarator y 2)])"));
}

DMOD_TEST_STEP(dmvs_js_reads_literals)
{
    /* Numbers exactly, as number / 10^decimals ('~': more digits than an int64_t holds) */
    DMOD_TEST_EXPECT_TRUE(parses("n = [0x1F, 0b101, 0o17, 1.50, .5, 1e3, 2.5e-2, 1_000, 12345678901234567890, , 7];",
        "(expression (assign = n (array [31 5 15 1.5 0.5 1000 0.025 1000 1234567890123456780~ <hole> 7])))"));
    DMOD_TEST_EXPECT_TRUE(parses("s = '\\u00e9\\n\\x41' + \"\\\"q\\\"\" + '\\u{1F600}' + '\\uD83D\\uDE00';",
        "(expression (assign = s (binary + (binary + (binary + \"\xC3\xA9\\nA\" \"\\\"q\\\"\") \"\xF0\x9F\x98\x80\") \"\xF0\x9F\x98\x80\")))"));
    DMOD_TEST_EXPECT_TRUE(parses("t = `a${x}b${y + 1}`; tag`a${1}`;",
        "(expression (assign = t (template [\"a\" x \"b\" (binary + y 1) \"\"]))) (expression (template [\"a\" 1 \"\"] tag))"));
    DMOD_TEST_EXPECT_TRUE(parses("x = `line\\nnext ${ `inner ${y}` } ${ {a: 1}.a }`;",
        "(expression (assign = x (template [\"line\\nnext \" (template [\"inner \" y \"\"]) \" \" "
        "(member (object [(property a 1)]) a) \"\"])))"));
    DMOD_TEST_EXPECT_TRUE(parses("r = a / b / c; s = /ab+c/g.test(x); u = /[/]/;",
        "(expression (assign = r (binary / (binary / a b) c))) "
        "(expression (assign = s (call (member (regex /ab+c/g) test) [x]))) (expression (assign = u (regex /[/]/)))"));
}

DMOD_TEST_STEP(dmvs_js_reports_errors)
{
    DMOD_TEST_EXPECT_TRUE(fails("a = ;", -EBADMSG, 1, 5, "unexpected token"));
    DMOD_TEST_EXPECT_TRUE(fails("x = 1;\ns = 'abc", -EBADMSG, 2, 5, "unterminated string"));
    DMOD_TEST_EXPECT_TRUE(fails("f(a", -EBADMSG, 1, 4, "expected ')'"));
    DMOD_TEST_EXPECT_TRUE(fails("1 = 2", -EBADMSG, 1, 3, "invalid assignment target"));
    DMOD_TEST_EXPECT_TRUE(fails("a b", -EBADMSG, 1, 3, "expected ';'"));
    DMOD_TEST_EXPECT_TRUE(fails("/* x", -EBADMSG, 1, 1, "unterminated comment"));
    DMOD_TEST_EXPECT_TRUE(fails("n = 10n", -EBADMSG, 1, 5, "BigInt is not supported"));

    /* Nested deeper than a small target's stack takes */
    static char deep[400];
    size_t n = 0;
    deep[n++] = 'x';
    deep[n++] = '=';
    for (int i = 0; i < 150; i++)
        deep[n++] = '[';
    for (int i = 0; i < 150; i++)
        deep[n++] = ']';
    deep[n] = '\0';
    dmvs_js_error_t e;
    DMOD_TEST_EXPECT_TRUE(tree(deep, &e) == NULL);
    DMOD_TEST_EXPECT_EQ(e.status, -E2BIG);
}

/* A fixture's text into `buffer`; its length, 0 when there is none */
static size_t read_fixture(const char* name, char* buffer, size_t size)
{
    char path[256];
    Dmod_SnPrintf(path, sizeof(path), "%s/%s", DMVS_JS_FIXTURES_DIR, name);
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return 0;
    size_t n = Dmod_FileRead(buffer, 1, size - 1U, f);
    Dmod_FileClose(f);
    buffer[n] = '\0';
    return n;
}

/* A page's script makes the tree acorn makes of it */
static bool as_acorn(const char* script, const char* expected_tree)
{
    size_t n = read_fixture(script, g_file, sizeof(g_file));
    size_t m = read_fixture(expected_tree, g_expected, sizeof(g_expected));
    if (n == 0 || m == 0)
    {
        Dmod_Printf("    no %s or %s\n", script, expected_tree);
        return false;
    }
    dmvs_js_error_t e;
    dmvs_js_ast_t ast = dmvs_js_parse(g_file, n, &e);
    if (ast == NULL)
    {
        Dmod_Printf("    %s:%u:%u: %s\n", script, e.line, e.column, e.message);
        return false;
    }
    static char mine[32768];
    size_t length = dmvs_js_dump(dmvs_js_root(ast), mine, sizeof(mine));
    dmvs_js_free(ast);
    if (length != m || memcmp(mine, g_expected, m) != 0)
    {
        size_t i = 0;
        while (i < m && i < length && mine[i] == g_expected[i])
            i++;
        Dmod_Printf("    %s: differs from acorn's tree at byte %u\n", script, (unsigned)i);
        return false;
    }
    return true;
}

DMOD_TEST_STEP(dmvs_js_parses_pages_as_acorn_does)
{
    DMOD_TEST_EXPECT_TRUE(as_acorn("dmodos.js", "dmodos.tree"));
    DMOD_TEST_EXPECT_TRUE(as_acorn("car_hmi.js", "car_hmi.tree"));
}

DMOD_TEST_STEP(dmvs_js_keeps_where_nodes_are)
{
    const char* src = "let a = 1;\n  foo(bar);";
    dmvs_js_ast_t ast = dmvs_js_parse(src, strlen(src), NULL);
    DMOD_TEST_EXPECT_TRUE(ast != NULL);
    if (ast == NULL)
        return;
    const dmvs_js_node_t* second = dmvs_js_root(ast)->a->next;
    DMOD_TEST_EXPECT_TRUE(second != NULL && second->kind == DMVS_JS_EXPRESSION);
    if (second != NULL)
    {
        const dmvs_js_node_t* call = second->a;
        DMOD_TEST_EXPECT_TRUE(call->kind == DMVS_JS_CALL && call->line == 2 && call->column == 3);
        DMOD_TEST_EXPECT_TRUE(call->start == 13 && call->end == 21);        /* "foo(bar)" */
        DMOD_TEST_EXPECT_TRUE(strcmp(dmvs_js_kind_name(call->kind), "call") == 0);
        DMOD_TEST_EXPECT_TRUE(strcmp(dmvs_js_op_name(DMVS_JS_OP_COALESCE), "??") == 0);
    }
    dmvs_js_free(ast);
}

/* ---- Streams ---- */

typedef struct
{
    const char* text;
    size_t      length;
    size_t      pos;
    uint32_t    turn;           /* Pieces of 1 ... 7 bytes, by turns */
    size_t      fail_at;        /* Reading fails there (0: never) */
} stream_t;

static int32_t read_piece(void* ctx, char* buffer, size_t size)
{
    stream_t* s = ctx;
    if (s->fail_at != 0 && s->pos >= s->fail_at)
        return -EIO;
    size_t n = 1u + (s->turn++ % 7u);
    if (n > size)
        n = size;
    if (n > s->length - s->pos)
        n = s->length - s->pos;
    memcpy(buffer, s->text + s->pos, n);
    s->pos += n;
    return (int32_t)n;
}

DMOD_TEST_STEP(dmvs_js_parses_streams)
{
    /* The car HMI's script, in pieces of 1 ... 7 bytes: acorn's tree, never all of it at once */
    size_t n = read_fixture("car_hmi.js", g_file, sizeof(g_file));
    size_t m = read_fixture("car_hmi.tree", g_expected, sizeof(g_expected));
    DMOD_TEST_EXPECT_TRUE(n > 0 && m > 0);
    stream_t s = { g_file, n, 0, 0, 0 };
    dmvs_js_error_t e;
    dmvs_js_ast_t ast = dmvs_js_parse_stream(read_piece, &s, &e);
    DMOD_TEST_EXPECT_TRUE(ast != NULL);
    if (ast == NULL)
        return;
    static char mine[32768];
    size_t length = dmvs_js_dump(dmvs_js_root(ast), mine, sizeof(mine));
    DMOD_TEST_EXPECT_TRUE(length == m && memcmp(mine, g_expected, m) == 0);
    dmvs_js_info_t info;
    DMOD_TEST_EXPECT_EQ(dmvs_js_info(ast, &info), 0);
    DMOD_TEST_EXPECT_EQ(info.source, (uint32_t)n);
    DMOD_TEST_EXPECT_TRUE(info.window < 1024u);            /* Of 10.9 KB */
    DMOD_TEST_EXPECT_EQ(info.nodes, 1014u);
    dmvs_js_free(ast);

    /* A program wrapped in parentheses is not read whole to tell it from arrow parameters */
    static char wrapped[8192];
    size_t k = 0;
    k += (size_t)Dmod_SnPrintf(wrapped + k, sizeof(wrapped) - k, "(function () {\n");
    for (int i = 0; i < 200 && k + 64 < sizeof(wrapped); i++)
        k += (size_t)Dmod_SnPrintf(wrapped + k, sizeof(wrapped) - k, "  var v%d = f(%d, 'x');\n", i, i);
    k += (size_t)Dmod_SnPrintf(wrapped + k, sizeof(wrapped) - k, "})();\n");
    stream_t w = { wrapped, k, 0, 0, 0 };
    ast = dmvs_js_parse_stream(read_piece, &w, &e);
    DMOD_TEST_EXPECT_TRUE(ast != NULL && dmvs_js_info(ast, &info) == 0 && info.window < 256u);
    dmvs_js_free(ast);

    /* Reading fails: -EIO, where it stopped */
    stream_t f = { g_file, n, 0, 0, 500 };
    DMOD_TEST_EXPECT_TRUE(dmvs_js_parse_stream(read_piece, &f, &e) == NULL);
    DMOD_TEST_EXPECT_EQ(e.status, -EIO);
    DMOD_TEST_EXPECT_TRUE(e.offset >= 400u && e.offset <= 510u);
}

#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/*
 * The compiler: scripts compiled into a dmvsi document, then run - the
 * document's actions by a small interpreter here (as the view would run
 * them) - with a host of a few elements: document.getElementById(),
 * innerText (a text variable, a number shadowing it), addEventListener()
 * and onclick code. What the elements show is what is checked.
 */

#define MAX_VARS        512u
#define MAX_ELEMENTS    8u
#define DOCUMENT        100u

typedef struct
{
    char            id[16];
    char            text[32];           /* What it shows first */
    dmvsi_var_t     var;                /* Its text */
    dmvsi_var_t     shadow;             /* The number its text is (1/1000) */
    dmvsi_handler_t click;
} element_t;

typedef struct
{
    dmvsi_doc_t     doc;
    element_t       elements[MAX_ELEMENTS];
    uint32_t        count;
    uint32_t        reports;
    int32_t         num[MAX_VARS];
    char            text[MAX_VARS][128];
    int32_t         now;
    uint32_t        steps;              /* Actions run (a runaway loop stops) */
} page_t;

static page_t* g_page;

/* The page of a step (on the heap: the module's data stays small) */
static page_t* page(void)
{
    if (g_page == NULL)
        g_page = Dmod_Malloc(sizeof(page_t));
    return g_page;
}

/* ---- The host ---- */

static element_t* element(page_t* p, uint32_t object)
{
    return (object >= 1 && object <= p->count) ? &p->elements[object - 1U] : NULL;
}

static bool host_global(void* ctx, dmvs_js_compiler_t c, const char* name, dmvs_js_value_t* value)
{
    (void)ctx;
    (void)c;
    if (strcmp(name, "document") != 0)
        return false;
    memset(value, 0, sizeof(*value));
    value->kind = DMVS_JS_V_OBJECT;
    value->object = DOCUMENT;
    return true;
}

static int host_get(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* object, const char* name, dmvs_js_value_t* value)
{
    page_t* p = ctx;
    element_t* e = (object->kind == DMVS_JS_V_OBJECT) ? element(p, object->object) : NULL;
    (void)c;
    if (e == NULL || (strcmp(name, "innerText") != 0 && strcmp(name, "textContent") != 0))
        return -ENOTSUP;
    memset(value, 0, sizeof(*value));
    value->kind = DMVS_JS_V_RUNTIME;
    value->type = DMVS_JS_T_TEXT;
    value->var = e->var;
    value->number_var = e->shadow;
    return 0;
}

/*
 * innerText of an element a variable holds: kept back (flushed as the
 * compiler asks - the flush hook), then a CALL of a handler made at the end,
 * an IF per element the variable may hold (dmvs_js_object_domain())
 */
typedef struct
{
    dmvsi_var_t     holder;
    dmvsi_var_t     text;               /* What it is set to: a text variable */
    dmvsi_handler_t handler;
} held_t;

static held_t g_held[16];
static uint32_t g_held_count, g_kept, g_flushes;

static int set_held(page_t* p, dmvs_js_compiler_t c, const dmvs_js_value_t* object, const char* name, const dmvs_js_value_t* value)
{
    if (strcmp(name, "innerText") != 0 || g_held_count >= 16u)
        return -ENOTSUP;
    held_t* h = &g_held[g_held_count++];
    h->holder = object->var;
    h->text = dmvsi_add_text_var(p->doc, "held", 64, "");
    h->handler = dmvsi_new_handler(p->doc);
    dmvsi_action_t a;
    memset(&a, 0, sizeof(a));
    a.kind = DMVSI_ACT_SET;
    a.var = h->text;
    if (dmvs_js_text_operand(c, value, &a.operand, &a.text) != 0)
        return -ENOTSUP;
    dmvs_js_emit(c, &a);
    g_kept++;                           /* The CALL: at the flush */
    return 0;
}

static void host_flush(void* ctx, dmvs_js_compiler_t c)
{
    (void)ctx;
    g_flushes++;
    for (; g_kept > 0; g_kept--)
    {
        dmvsi_action_t a;
        memset(&a, 0, sizeof(a));
        a.kind = DMVSI_ACT_CALL;
        a.handler = g_held[g_held_count - g_kept].handler;
        dmvs_js_emit(c, &a);
    }
}

/* The handlers of what was set of held elements: an IF for each element */
static void make_held(page_t* p, dmvs_js_compiler_t c)
{
    for (uint32_t i = 0; i < g_held_count; i++)
    {
        uint32_t objects[MAX_ELEMENTS];
        uint32_t n = dmvs_js_object_domain(c, g_held[i].holder, objects, MAX_ELEMENTS);
        dmvsi_action_t a[3 * MAX_ELEMENTS];
        uint32_t k = 0;
        memset(a, 0, sizeof(a));
        for (uint32_t j = 0; j < n && j < MAX_ELEMENTS; j++)
        {
            a[k].kind = DMVSI_ACT_IF_EQ;
            a[k].var = g_held[i].holder;
            a[k++].value = (int32_t)objects[j];
            a[k].kind = DMVSI_ACT_SET;
            a[k].var = p->elements[objects[j] - 1U].var;
            a[k++].operand = g_held[i].text;
            a[k++].kind = DMVSI_ACT_END;
        }
        dmvsi_set_handler(p->doc, g_held[i].handler, a, k);
    }
    g_held_count = 0;
}

static char g_choices[256];                 /* What texts set were one of: "a|b|c@var " */

static void record_choices(dmvs_js_compiler_t c, const dmvs_js_value_t* value)
{
    const dmvs_js_value_t* picks = NULL;
    dmvsi_var_t index = 0;
    uint32_t n = dmvs_js_choices(c, value, &picks, &index);
    for (uint32_t i = 0; i < n; i++)
    {
        size_t k = strlen(g_choices);
        Dmod_SnPrintf(g_choices + k, sizeof(g_choices) - k, "%s%s", (i > 0) ? "|" : "", (picks[i].kind == DMVS_JS_V_STRING) ? picks[i].text : "?");
    }
    if (n > 0)
    {
        size_t k = strlen(g_choices);
        Dmod_SnPrintf(g_choices + k, sizeof(g_choices) - k, "%s ", (index != 0) ? "@" : "!");
    }
}

static int host_set(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* object, const char* name, const dmvs_js_value_t* value)
{
    page_t* p = ctx;
    if (object->kind == DMVS_JS_V_RUNTIME)
        return set_held(p, c, object, name, value);
    element_t* e = element(p, object->object);
    if (e == NULL || (strcmp(name, "innerText") != 0 && strcmp(name, "textContent") != 0 && strcmp(name, "innerHTML") != 0))
        return -ENOTSUP;
    record_choices(c, value);
    dmvsi_action_t a;
    memset(&a, 0, sizeof(a));
    a.kind = DMVSI_ACT_SET;
    a.var = e->var;
    if (dmvs_js_text_operand(c, value, &a.operand, &a.text) != 0)
        return -ENOTSUP;
    if (a.operand != e->var)
        dmvs_js_emit(c, &a);
    /* Its number too: parseFloat(innerText) */
    memset(&a, 0, sizeof(a));
    a.kind = DMVSI_ACT_SET;
    a.var = e->shadow;
    double n = 0.0;
    if (value->kind == DMVS_JS_V_RUNTIME && value->number_var != 0)
        a.operand = value->number_var;
    else if (value->kind == DMVS_JS_V_STRING && dmvs_js_parse_number(value->text, value->length, &n))
        a.value = (int32_t)(n * 1000.0 + ((n < 0) ? -0.5 : 0.5));
    else if (value->kind == DMVS_JS_V_NUMBER)
        a.value = (int32_t)(value->number * 1000.0 + ((value->number < 0) ? -0.5 : 0.5));
    if (a.operand != e->shadow)
        dmvs_js_emit(c, &a);
    return 0;
}

static int host_call(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* self, const char* method, const dmvs_js_value_t* args,
                     uint32_t count, dmvs_js_value_t* result)
{
    page_t* p = ctx;
    uint32_t object = (self->kind == DMVS_JS_V_OBJECT) ? self->object : 0;
    memset(result, 0, sizeof(*result));
    if (object == DOCUMENT && strcmp(method, "querySelectorAll") == 0)
    {
        /* Every element: an array of the host's objects */
        dmvs_js_value_t all[MAX_ELEMENTS];
        memset(all, 0, sizeof(all));
        for (uint32_t i = 0; i < p->count; i++)
        {
            all[i].kind = DMVS_JS_V_OBJECT;
            all[i].object = i + 1U;
        }
        return dmvs_js_array(c, all, p->count, result);
    }
    if (object == DOCUMENT && strcmp(method, "getElementById") == 0 && count == 1 && args[0].kind == DMVS_JS_V_STRING)
    {
        for (uint32_t i = 0; i < p->count; i++)
        {
            if (strcmp(p->elements[i].id, args[0].text) == 0)
            {
                result->kind = DMVS_JS_V_OBJECT;
                result->object = i + 1U;
                return 0;
            }
        }
        result->kind = DMVS_JS_V_NULL;
        return 0;
    }
    element_t* e = element(p, object);
    if (e != NULL && strcmp(method, "addEventListener") == 0 && count >= 2 && args[0].kind == DMVS_JS_V_STRING &&
        strcmp(args[0].text, "click") == 0)
    {
        e->click = dmvs_js_function_handler(c, &args[1], object);
        return (e->click != 0) ? 0 : -ENOTSUP;
    }
    return -ENOTSUP;
}

static void host_report(void* ctx, uint32_t line, uint32_t column, const char* message)
{
    page_t* p = ctx;
    p->reports++;
    Dmod_Printf("    (%u:%u) %s\n", line, column, message);
}

/* ---- The view: the document's actions run ---- */

static int32_t operand(page_t* p, const dmvsi_action_t* a)
{
    if (a->operand == DMVSI_VAR_TIME)
        return p->now;
    return (a->operand != 0) ? p->num[a->operand] : a->value;
}

static bool is_if(uint8_t kind)
{
    return kind == DMVSI_ACT_IF_EQ || kind == DMVSI_ACT_IF_NE || (kind >= DMVSI_ACT_IF_LT && kind <= DMVSI_ACT_IF_GE);
}

static void run(page_t* p, dmvsi_handler_t h, uint32_t depth);

/* Where the block opened at `at` has its ELSE (0: none) and END */
static void block_of(const dmvsi_action_t* a, uint32_t n, uint32_t at, uint32_t* else_at, uint32_t* end_at)
{
    uint32_t depth = 0;
    *else_at = 0;
    *end_at = n;
    for (uint32_t i = at + 1U; i < n; i++)
    {
        if (is_if(a[i].kind) || a[i].kind == DMVSI_ACT_LOOP)
            depth++;
        else if (a[i].kind == DMVSI_ACT_ELSE && depth == 0)
            *else_at = i;
        else if (a[i].kind == DMVSI_ACT_END)
        {
            if (depth == 0)
            {
                *end_at = i;
                return;
            }
            depth--;
        }
    }
}

/* The block a position is in (its opener), n: none */
static uint32_t opener_of(const dmvsi_action_t* a, uint32_t at, bool loop)
{
    uint32_t depth = 0;
    for (uint32_t i = at; i-- > 0;)
    {
        if (a[i].kind == DMVSI_ACT_END)
            depth++;
        else if (is_if(a[i].kind) || a[i].kind == DMVSI_ACT_LOOP)
        {
            if (depth == 0 && (!loop || a[i].kind == DMVSI_ACT_LOOP))
                return i;
            if (depth > 0)
                depth--;
        }
    }
    return UINT32_MAX;
}

static bool test(page_t* p, const dmvsi_action_t* a)
{
    int32_t x = p->num[a->var], y = operand(p, a);
    switch (a->kind)
    {
        case DMVSI_ACT_IF_EQ: return x == y;
        case DMVSI_ACT_IF_NE: return x != y;
        case DMVSI_ACT_IF_LT: return x < y;
        case DMVSI_ACT_IF_LE: return x <= y;
        case DMVSI_ACT_IF_GT: return x > y;
        default: return x >= y;
    }
}

static void run(page_t* p, dmvsi_handler_t h, uint32_t depth)
{
    const dmvsi_action_t* a = NULL;
    uint32_t n = dmvsi_handler_actions(p->doc, h, &a);
    if (depth > 8u)
    {
        Dmod_Printf("    calls too deep\n");
        return;
    }
    for (uint32_t pc = 0; pc < n && p->steps < 1000000u; p->steps++)
    {
        const dmvsi_action_t* x = &a[pc];
        uint32_t else_at, end_at, at;
        bool text = false;
        dmvsi_var_info_t info;
        if (x->var != 0 && x->var < MAX_VARS && dmvsi_var_info(p->doc, x->var, &info) == 0)
            text = info.kind == DMVSI_VAR_TEXT;
        if (is_if(x->kind))
        {
            if (test(p, x))
                pc++;
            else
            {
                block_of(a, n, pc, &else_at, &end_at);
                pc = (else_at != 0) ? else_at + 1U : end_at + 1U;
            }
            continue;
        }
        switch (x->kind)
        {
            case DMVSI_ACT_ELSE:
                at = opener_of(a, pc, false);
                block_of(a, n, at, &else_at, &end_at);
                pc = end_at + 1U;
                continue;
            case DMVSI_ACT_END:
                at = opener_of(a, pc, false);
                pc = (at != UINT32_MAX && a[at].kind == DMVSI_ACT_LOOP) ? at + 1U : pc + 1U;
                continue;
            case DMVSI_ACT_BREAK:
            case DMVSI_ACT_CONTINUE:
                at = opener_of(a, pc, true);
                block_of(a, n, at, &else_at, &end_at);
                pc = (x->kind == DMVSI_ACT_BREAK) ? end_at + 1U : at + 1U;
                continue;
            case DMVSI_ACT_RETURN:
                return;
            case DMVSI_ACT_CALL:
                run(p, x->handler, depth + 1U);
                break;
            case DMVSI_ACT_SET:
                if (text)
                    Dmod_SnPrintf(p->text[x->var], sizeof(p->text[0]), "%s", (x->operand != 0) ? p->text[x->operand] : x->text);
                else
                    p->num[x->var] = operand(p, x);
                break;
            case DMVSI_ACT_APPEND:
            {
                size_t l = strlen(p->text[x->var]);
                Dmod_SnPrintf(p->text[x->var] + l, sizeof(p->text[0]) - l, "%s", (x->operand != 0) ? p->text[x->operand] : x->text);
                break;
            }
            case DMVSI_ACT_FORMAT:
                Dmod_SnPrintf(p->text[x->var], sizeof(p->text[0]), x->text, (int)operand(p, x));
                break;
            case DMVSI_ACT_ANIMATE: p->num[x->var] = operand(p, x); break;
            case DMVSI_ACT_TOGGLE: p->num[x->var] = !p->num[x->var]; break;
            case DMVSI_ACT_ADD: p->num[x->var] += operand(p, x); break;
            case DMVSI_ACT_SUB: p->num[x->var] -= operand(p, x); break;
            case DMVSI_ACT_MUL: p->num[x->var] *= operand(p, x); break;
            case DMVSI_ACT_DIV: if (operand(p, x) != 0) p->num[x->var] /= operand(p, x); break;
            case DMVSI_ACT_MOD: if (operand(p, x) != 0) p->num[x->var] %= operand(p, x); break;
            case DMVSI_ACT_MIN: if (operand(p, x) < p->num[x->var]) p->num[x->var] = operand(p, x); break;
            case DMVSI_ACT_MAX: if (operand(p, x) > p->num[x->var]) p->num[x->var] = operand(p, x); break;
            default:
                break;
        }
        pc++;
    }
}

/* The time goes on, timers run as the view runs them */
static void wait(page_t* p, int32_t ms)
{
    for (int32_t t = 0; t < ms; t++)
    {
        p->now++;
        uint16_t period;
        dmvsi_handler_t h;
        for (uint32_t i = 0; dmvsi_timer_at(p->doc, i, &period, &h); i++)
        {
            if (p->now % period == 0)
                run(p, h, 0);
        }
    }
}

/* ---- Pages ---- */

static const char* shows(page_t* p, const char* id)
{
    for (uint32_t i = 0; i < p->count; i++)
        if (strcmp(p->elements[i].id, id) == 0)
            return p->text[p->elements[i].var];
    return "?";
}

static bool shows_is(page_t* p, const char* id, const char* expected)
{
    const char* t = shows(p, id);
    if (strcmp(t, expected) == 0)
        return true;
    Dmod_Printf("    #%s shows \"%s\", not \"%s\" (at %d ms)\n", id, t, expected, (int)p->now);
    return false;
}

static void click(page_t* p, const char* id)
{
    for (uint32_t i = 0; i < p->count; i++)
        if (strcmp(p->elements[i].id, id) == 0 && p->elements[i].click != 0)
            run(p, p->elements[i].click, 0);
}

/* A page of elements: "id=text|id=text..." (no tables of pointers: the module's data is not relocated) */
static dmvs_js_compiler_t compiler_of(page_t* p, const char* elements)
{
    memset(p, 0, sizeof(*p));
    p->doc = dmvsi_new();
    for (const char* at = elements; *at != '\0' && p->count < MAX_ELEMENTS;)
    {
        element_t* e = &p->elements[p->count++];
        size_t n = 0;
        while (*at != '=' && *at != '\0' && n + 1U < sizeof(e->id))
            e->id[n++] = *at++;
        at += (*at == '=') ? 1 : 0;
        n = 0;
        while (*at != '|' && *at != '\0' && n + 1U < sizeof(e->text))
            e->text[n++] = *at++;
        at += (*at == '|') ? 1 : 0;
        double number = 0.0;
        e->var = dmvsi_add_text_var(p->doc, e->id, 64, e->text);
        e->shadow = dmvsi_add_var(p->doc, "shadow",
                                  dmvs_js_parse_number(e->text, strlen(e->text), &number) ? (int32_t)(number * 1000.0) : 0);
    }
    dmvs_js_host_t host;
    host.ctx = p;
    host.global = host_global;
    host.get = host_get;
    host.set = host_set;
    host.call = host_call;
    host.report = host_report;
    host.flush = host_flush;
    g_held_count = 0;
    g_kept = 0;
    return dmvs_js_compiler_new(p->doc, &host);
}

/* onclick code compiled after the script (its `this`: the first element) */
static dmvs_js_ast_t g_handler_code;
static dmvsi_handler_t g_handler;

/* The page: its scripts compiled, its variables as they start, the init handler run */
static bool load(page_t* p, dmvs_js_compiler_t c, const char* script)
{
    dmvs_js_error_t e;
    dmvs_js_ast_t ast = dmvs_js_parse(script, strlen(script), &e);
    if (ast == NULL)
    {
        Dmod_Printf("    error at %u:%u: %s\n", e.line, e.column, e.message);
        return false;
    }
    int ret = dmvs_js_compile(c, ast);              /* The compiler's now */
    if (ret == 0 && g_handler_code != NULL)
        g_handler = dmvs_js_compile_handler(c, g_handler_code, 1);
    g_handler_code = NULL;
    if (ret == 0)
        ret = dmvs_js_finish(c);
    make_held(p, c);
    if (ret != 0)
    {
        Dmod_Printf("    compiled: %d\n", ret);
        return false;
    }
    dmvsi_var_info_t info;
    for (dmvsi_var_t v = 1; v < MAX_VARS && dmvsi_var_info(p->doc, v, &info) == 0; v++)
    {
        p->num[v] = info.initial;
        if (info.kind == DMVSI_VAR_TEXT)
            Dmod_SnPrintf(p->text[v], sizeof(p->text[0]), "%s", info.text);
    }
    if (dmvsi_init_handler(p->doc) != 0)
        run(p, dmvsi_init_handler(p->doc), 0);
    return true;
}

static void unload(page_t* p, dmvs_js_compiler_t c)
{
    dmvs_js_compiler_free(c);
    dmvsi_free(p->doc);
}

/* ---- Steps ---- */

DMOD_TEST_STEP(dmvs_js_computes_what_is_static)
{
    const char* ids = "sum=|list=|label=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(c != NULL);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const items = [3, 4, 5];\n"
        "let total = 0;\n"
        "items.forEach(x => total += x * 2);\n"
        "document.getElementById('sum').innerText = 'Sum: ' + total;\n"
        "const names = { a: 'Alpha', b: 'Beta' };\n"
        "document.getElementById('list').innerText = Object2(names);\n"
        "function Object2(o) { let s = ''; for (const k in o) s = s + o[k].toUpperCase().slice(0, 3); return s; }\n"
        "const pi = Math.round(Math.PI * 100) / 100;\n"
        "document.getElementById('label').textContent = `pi ${pi} ${(2.5).toFixed(2)} ${'7'.padStart(3, '0')}`;\n"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "sum", "Sum: 24"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "list", "ALPBET"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "label", "pi 3.14 2.50 007"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_runs_listeners_and_intervals)
{
    const char* ids = "speed=0 km/h|btn=Go";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const speedEl = document.getElementById('speed');\n"
        "let speed = 0, target = 0;\n"
        "let timer = null;\n"
        "function step() {\n"
        "  if (speed < target) speed += 5; else if (speed > target) speed -= 5;\n"
        "  speedEl.innerText = speed + ' km/h';\n"
        "  if (speed === target) { clearInterval(timer); timer = null; }\n"
        "}\n"
        "document.getElementById('btn').addEventListener('click', () => {\n"
        "  target = target === 0 ? 60 : 0;\n"
        "  if (!timer) timer = setInterval(step, 50);\n"
        "});\n"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "speed", "0 km/h"));
    click(p, "btn");
    wait(p, 300);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "speed", "30 km/h"));
    wait(p, 400);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "speed", "60 km/h"));
    wait(p, 1000);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "speed", "60 km/h"));     /* Stopped there */
    click(p, "btn");
    wait(p, 1000);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "speed", "0 km/h"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_reads_numbers_from_texts)
{
    const char* ids = "temp=21.5\xC2\xB0" "C|up=+|down=-";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const t = document.getElementById('temp');\n"
        "function bump(d) {\n"
        "  const v = parseFloat(t.innerText);\n"
        "  t.innerHTML = (v + d).toFixed(1) + '&deg;C';\n"
        "}\n"
        "document.getElementById('up').addEventListener('click', () => bump(0.5));\n"
        "document.getElementById('down').addEventListener('click', function () { bump(-0.5); });\n"));
    click(p, "up");
    click(p, "up");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "temp", "22.5\xC2\xB0" "C"));
    click(p, "down");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "temp", "22.0\xC2\xB0" "C"));
    for (int i = 0; i < 45; i++)
        click(p, "down");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "temp", "-0.5\xC2\xB0" "C"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_compiles_functions_and_returns)
{
    const char* ids = "clock=|clock2=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "function two(n) { return n < 10 ? '0' + n : '' + n; }\n"
        "let secs = 0;\n"
        "const clock = document.getElementById('clock');\n"
        "setInterval(() => {\n"
        "  secs++;\n"
        "  clock.innerText = two(Math.floor(secs / 60)) + ':' + two(secs % 60);\n"
        "  document.getElementById('clock2').innerText =\n"
        "    String(Math.floor(secs / 60)).padStart(2, '0') + ':' + String(secs % 60).padStart(2, '0');\n"
        "}, 1000);\n"));
    wait(p, 75000);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "clock", "01:15"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "clock2", "01:15"));
    wait(p, 3600000);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "clock", "61:15"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_runs_timeouts_and_loops)
{
    const char* ids = "blink=|bar=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const el = document.getElementById('blink');\n"
        "let on = false, n = 0;\n"
        "function tick() {\n"
        "  on = !on; n++;\n"
        "  el.innerText = on ? 'ON' : 'OFF';\n"
        "  if (n < 3) setTimeout(tick, 100);\n"
        "}\n"
        "tick();\n"
        "let level = 3;\n"
        "function bar() {\n"
        "  let s = '';\n"
        "  for (let i = 0; i < 5; i++) s += i < level ? '#' : '.';\n"
        "  let k = 0;\n"
        "  while (true) { k++; if (k >= level) break; }\n"
        "  switch (k) { case 1: s += ' one'; break; case 3: s += ' three'; break; default: s += ' many'; }\n"
        "  document.getElementById('bar').innerText = s;\n"
        "}\n"
        "bar();\n"
        "setTimeout(() => { level = 1; bar(); }, 1000);\n"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "blink", "ON"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "bar", "###.. three"));
    wait(p, 100);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "blink", "OFF"));
    wait(p, 100);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "blink", "ON"));
    wait(p, 500);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "blink", "ON"));          /* Three times only */
    wait(p, 400);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "bar", "#.... one"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_compiles_onclick_code)
{
    const char* ids = "counter=n=0";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    /* The page's onclick code is seen first: it sets `count`, a variable then */
    dmvs_js_error_t e;
    const char* onclick = "count++; this.innerText = 'n=' + count";
    dmvs_js_ast_t code = dmvs_js_parse(onclick, strlen(onclick), &e);
    DMOD_TEST_EXPECT_TRUE(code != NULL);
    DMOD_TEST_EXPECT_EQ(dmvs_js_scan(c, code), 0);
    g_handler_code = code;                          /* Compiled with the script, before the end */
    DMOD_TEST_EXPECT_TRUE(load(p, c, "let count = 0;\n"));
    dmvsi_handler_t h = g_handler;
    DMOD_TEST_EXPECT_TRUE(h != 0);
    run(p, h, 0);
    run(p, h, 0);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "counter", "n=2"));
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_reports_what_it_does_not_compile)
{
    const char* ids = "x=|clock=14:32";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    Dmod_Printf("    (reports expected:)\n");
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "class A {}\n"
        "const d = new Date();\n"
        "document.getElementById('x').innerText = 'still';\n"
        "const t = `${String(d.getHours()).padStart(2, '0')}:${d.getMinutes()}`;\n"
        "document.getElementById('clock').innerText = t;\n"
        "if (d.getHours() > 12) document.getElementById('x').innerText = 'pm';\n"));
    /* What is made of what is not converted is not either - not reported again, the page as it is */
    DMOD_TEST_EXPECT_EQ(p->reports, 2u);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "clock", "14:32"));
    DMOD_TEST_EXPECT_EQ(dmvs_js_reports(c), p->reports);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "x", "still"));                /* The rest is compiled */
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_follows_calls_and_timers_that_stop_themselves)
{
    const char* ids = "out=|go=|cnt=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const el = document.getElementById('out');\n"
        "let n = 0;\n"
        "function a() { n++; if (n < 3) b(); }\n"
        "function b() { a(); }\n"
        "document.getElementById('go').addEventListener('click', () => { n = 0; a(); el.innerText = 'n=' + n; });\n"
        "const out = document.getElementById('cnt');\n"
        "let k = 0;\n"
        "const iv = setInterval(() => { k++; out.innerText = k; if (k >= 4) clearInterval(iv); }, 100);\n"));
    /* a -> b -> a: copies of them, as deep as the compiler follows (the one deeper reported) */
    DMOD_TEST_EXPECT_EQ(p->reports, 1u);
    click(p, "go");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "out", "n=3"));
    wait(p, 1000);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "cnt", "4"));
    unload(p, c);

    /* A break under a runtime condition: a loop of the view (not unrolled); a for-of has none (reported) */
    c = compiler_of(p, ids);
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "let m = 0;\n"
        "for (let i = 0; i < 5; i++) { if (m > 1) break; m++; }\n"
        "let s = '';\n"
        "for (let i = 0; i < 4; i++) { if (i === 2) continue; s += i; }\n"
        "document.getElementById('out').innerText = m + ' ' + s;\n"));
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "out", "2 013"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    unload(p, c);
    c = compiler_of(p, ids);
    Dmod_Printf("    (reports expected:)\n");
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "let q = 0;\n"
        "for (const x of [1, 2, 3]) { if (q > 1) break; q += x; }\n"));
    DMOD_TEST_EXPECT_EQ(p->reports, 1u);
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_keeps_elements_in_variables)
{
    const char* ids = "a=|b=|c=|next=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    g_flushes = 0;
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const all = document.querySelectorAll('*');\n"
        "let current = null;\n"
        "let n = 0;\n"
        "function show(i) {\n"
        "  if (current !== null) current.innerText = '';\n"
        "  current = all[i];\n"
        "  current.innerText = 'here ' + n;\n"
        "}\n"
        "document.getElementById('next').addEventListener('click', () => {\n"
        "  n++;\n"
        "  if (current === all[0]) show(1); else if (current === all[1]) show(2); else show(0);\n"
        "});\n"
        "show(0);\n"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    DMOD_TEST_EXPECT_TRUE(g_flushes > 0u);
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "a", "here 0"));
    click(p, "next");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "a", "") && shows_is(p, "b", "here 1"));
    click(p, "next");
    click(p, "next");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "c", "") && shows_is(p, "a", "here 3"));
    unload(p, c);
}

DMOD_TEST_STEP(dmvs_js_compiles_listeners_when_the_scripts_have_loaded)
{
    const char* ids = "go=|out=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    /* The listener uses what the script makes after it is added: as it runs, after the script */
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "document.getElementById('go').addEventListener('click', () => show());\n"
        "const out = document.getElementById('out');\n"
        "const label = 'shown';\n"
        "function show() { out.innerText = label; }\n"));
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
    click(p, "go");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "out", "shown"));
    unload(p, c);
}

/* ---- An evaluator: the loading run as JavaScript runs it ---- */

static char g_built[512];
static uint32_t g_created;

static int build_call(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* self, const char* method,
                      const dmvs_js_value_t* args, uint32_t count, dmvs_js_value_t* result)
{
    (void)c;
    memset(result, 0, sizeof(*result));
    if (self->kind == DMVS_JS_V_OBJECT && self->object == DOCUMENT && strcmp(method, "createElement") == 0)
    {
        result->kind = DMVS_JS_V_OBJECT;
        result->object = 200u + ++g_created;
        return 0;
    }
    if (strcmp(method, "appendChild") == 0 && count == 1)
    {
        size_t n = strlen(g_built);
        Dmod_SnPrintf(g_built + n, sizeof(g_built) - n, "[%u]", (unsigned)(args[0].object - 200u));
        return 0;
    }
    return host_call(ctx, c, self, method, args, count, result);
}

static int build_set(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* object, const char* name, const dmvs_js_value_t* value)
{
    (void)ctx;
    (void)c;
    if (object->kind == DMVS_JS_V_OBJECT && object->object > 200u && value->kind == DMVS_JS_V_STRING)
    {
        size_t n = strlen(g_built);
        Dmod_SnPrintf(g_built + n, sizeof(g_built) - n, "%u.%s=%s ", (unsigned)(object->object - 200u), name, value->text);
        return 0;
    }
    return -ENOTSUP;
}

DMOD_TEST_STEP(dmvs_js_evaluates_what_the_scripts_do_when_they_load)
{
    page_t* p = page();
    memset(p, 0, sizeof(*p));
    g_built[0] = '\0';
    g_created = 0;
    dmvs_js_host_t host;
    memset(&host, 0, sizeof(host));
    host.ctx = p;
    host.global = host_global;
    host.call = build_call;
    host.set = build_set;
    host.report = host_report;
    dmvs_js_compiler_t c = dmvs_js_evaluator_new(&host);
    DMOD_TEST_EXPECT_TRUE(c != NULL);
    const char* script =
        "const songs = [{ t: 'A', s: 70 }, { t: 'B', s: 125 }, { t: 'C', s: 3 }];\n"
        "let current = 1, made = 0;\n"
        "const list = document.createElement('ul');\n"
        "function time(sec) { if (sec < 60) return '0:' + String(sec).padStart(2, '0'); return Math.floor(sec / 60) + ':' + (sec % 60); }\n"
        "function render() {\n"
        "  songs.forEach((song, i) => {\n"
        "    const div = document.createElement('div');\n"
        "    div.className = `row ${i === current ? 'on' : 'off'}`;\n"
        "    div.innerHTML = `${song.t} ${time(song.s)}`;\n"
        "    list.appendChild(div);\n"
        "    made++;\n"
        "  });\n"
        "}\n"
        "render();\n"
        "let n = 0;\n"
        "while (n < made) n += 2;\n"
        "if (n === 4) { const d = document.createElement('p'); d.textContent = 'four'; }\n"
        "setInterval(() => render(), 1000);\n";
    dmvs_js_error_t e;
    dmvs_js_ast_t ast = dmvs_js_parse(script, strlen(script), &e);
    DMOD_TEST_EXPECT_TRUE(ast != NULL);
    DMOD_TEST_EXPECT_EQ(dmvs_js_compile(c, ast), 0);
    dmvs_js_compiler_free(c);
    /* Every variable known as it changes, functions run at every call, the interval not */
    const char* want = "2.className=row off 2.innerHTML=A 1:10 [2]3.className=row on 3.innerHTML=B 2:5 [3]"
                       "4.className=row off 4.innerHTML=C 0:03 [4]5.textContent=four ";
    bool same = strcmp(g_built, want) == 0;
    if (!same)
        Dmod_Printf("    built %s\n    want  %s\n", g_built, want);
    DMOD_TEST_EXPECT_TRUE(same);
    DMOD_TEST_EXPECT_EQ(g_created, 5u);
    DMOD_TEST_EXPECT_EQ(p->reports, 0u);
}

DMOD_TEST_STEP(dmvs_js_tells_what_a_value_is_one_of)
{
    const char* ids = "cover=|row=|next=";
    page_t* p = page();
    dmvs_js_compiler_t c = compiler_of(p, ids);
    g_choices[0] = '\0';
    DMOD_TEST_EXPECT_TRUE(load(p, c,
        "const songs = [{ cover: 'a.jpg' }, { cover: 'b.jpg' }, { cover: 'c.jpg' }];\n"
        "let current = 0;\n"
        "function show() {\n"
        "  const song = songs[current];\n"
        "  document.getElementById('cover').innerText = song.cover;\n"
        "  document.getElementById('row').innerText = `row ${current === 1 ? 'on' : 'off'}`;\n"
        "}\n"
        "document.getElementById('next').addEventListener('click', () => { current = (current + 1) % 3; show(); });\n"));
    click(p, "next");
    DMOD_TEST_EXPECT_TRUE(shows_is(p, "cover", "b.jpg") && shows_is(p, "row", "row on"));
    /* The cover one of the songs', the row's text one of two - each with the variable telling which */
    const char* want = "a.jpg|b.jpg|c.jpg@ row off|row on@ ";
    bool same = strcmp(g_choices, want) == 0;
    if (!same)
        Dmod_Printf("    choices %s\n    want    %s\n", g_choices, want);
    DMOD_TEST_EXPECT_TRUE(same);
    unload(p, c);
}

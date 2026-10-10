#include "compiler.h"

/*
 * Compiling: scripts run by a partial evaluator (see compiler.h).
 *
 * - The pre-scan (all scripts so far): the names assigned anywhere - a let
 *   or a var of such a name is a runtime variable, the others are what
 *   they are set to - and the names that may hold fractions (a fraction, a
 *   division, parseFloat, a parameter given one, ...) - their variables are
 *   fixed point, the others integers.
 * - Statements whose conditions are static are computed: the branch taken
 *   is compiled, a loop of static bounds unrolled. Runtime ones become
 *   IF / LOOP blocks.
 * - A function is compiled once for each set of static arguments it is
 *   called with (and its `this`) into a handler, called with its runtime
 *   arguments set first; one that only computes a static value is that
 *   value.
 */

#define MAX_UNROLL      256u            /* Iterations of a loop unrolled at most */
#define MAX_FUNCTIONS   128u
#define MAX_RECURSION   2u              /* Copies of a function a recursive call makes */

static value_t expression(compiler_t* c, scope_t* s, const node_t* n);
static int statement(compiler_t* c, scope_t* s, const node_t* n);
static int statements(compiler_t* c, scope_t* s, const node_t* first);

/* What a loop being compiled is: a runtime LOOP, or unrolled (break / continue are flags) */


/* ---- Names: the pre-scan ---- */

static bool name_in(const char* const* list, uint32_t count, const char* name)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (strcmp(list[i], name) == 0)
            return true;
    }
    return false;
}

static void name_add(const char** list, uint32_t* count, const char* name)
{
    if (name != NULL && *count < MAX_NAMES && !name_in(list, *count, name))
        list[(*count)++] = name;
}

bool fractional_name(const compiler_t* c, const char* name)
{
    return name_in(c->fractional, c->fractional_count, name);
}

static bool assigned_name(const compiler_t* c, const char* name)
{
    return name_in(c->assigned, c->assigned_count, name);
}

static bool ident_is(const node_t* n, const char* name)
{
    return n != NULL && n->kind == DMVS_JS_IDENT && strcmp(n->text, name) == 0;
}

/* A member's property name (a.b), NULL for a[b] */
static const char* member_name(const node_t* n)
{
    return (n->kind == DMVS_JS_MEMBER && (n->flags & DMVS_JS_F_COMPUTED) == 0 && n->b != NULL) ? n->b->text : NULL;
}

/* The names a pattern / target binds */
static void targets(compiler_t* c, const node_t* t)
{
    if (t == NULL)
        return;
    if (t->kind == DMVS_JS_IDENT)
        name_add(c->assigned, &c->assigned_count, t->text);
    else if (t->kind == DMVS_JS_ARRAY || t->kind == DMVS_JS_OBJECT)
    {
        for (const node_t* k = t->a; k != NULL; k = k->next)
            targets(c, (k->kind == DMVS_JS_PROPERTY) ? k->b : k);
    }
    else if (t->kind == DMVS_JS_ASSIGN || t->kind == DMVS_JS_SPREAD)
        targets(c, t->a);
}

typedef struct
{
    const node_t*   functions[MAX_FUNCTIONS];   /* Declared functions, by name */
    uint32_t        function_count;
    const char*     returns_fraction[MAX_FUNCTIONS];
    uint32_t        returns_count;
    bool            changed;
} scan_t;

static const node_t* function_named(const scan_t* sc, const char* name)
{
    for (uint32_t i = 0; i < sc->function_count; i++)
    {
        if (sc->functions[i]->text != NULL && strcmp(sc->functions[i]->text, name) == 0)
            return sc->functions[i];
    }
    return NULL;
}

/* Whether an expression may be a fraction */
static bool fraction(const compiler_t* c, const scan_t* sc, const node_t* e)
{
    if (e == NULL)
        return false;
    switch (e->kind)
    {
        case DMVS_JS_NUMBER:
            return e->decimals > 0 || (e->flags & DMVS_JS_F_INEXACT) != 0;
        case DMVS_JS_IDENT:
            return fractional_name(c, e->text);
        case DMVS_JS_BINARY:
            if (e->op == DMVS_JS_OP_DIV || e->op == DMVS_JS_OP_POW)
                return true;
            return (e->op == DMVS_JS_OP_ADD || e->op == DMVS_JS_OP_SUB || e->op == DMVS_JS_OP_MUL || e->op == DMVS_JS_OP_MOD) &&
                   (fraction(c, sc, e->a) || fraction(c, sc, e->b));
        case DMVS_JS_UNARY:
            return (e->op == DMVS_JS_OP_SUB || e->op == DMVS_JS_OP_ADD) && fraction(c, sc, e->a);
        case DMVS_JS_CONDITIONAL:
            return fraction(c, sc, e->b) || fraction(c, sc, e->c);
        case DMVS_JS_ASSIGN:
            return fraction(c, sc, e->b);
        case DMVS_JS_LOGICAL:
            return fraction(c, sc, e->a) || fraction(c, sc, e->b);
        case DMVS_JS_SEQUENCE:
        {
            const node_t* last = e->a;
            while (last != NULL && last->next != NULL)
                last = last->next;
            return fraction(c, sc, last);
        }
        case DMVS_JS_CALL:
        {
            const node_t* f = e->a;
            if (ident_is(f, "parseFloat") || ident_is(f, "Number"))
                return true;
            if (f->kind == DMVS_JS_MEMBER && ident_is(f->a, "Math"))
            {
                const char* m = member_name(f);
                if (m == NULL || strcmp(m, "floor") == 0 || strcmp(m, "round") == 0 || strcmp(m, "ceil") == 0 ||
                    strcmp(m, "trunc") == 0 || strcmp(m, "sign") == 0)
                    return false;
                if (strcmp(m, "min") == 0 || strcmp(m, "max") == 0 || strcmp(m, "abs") == 0)
                {
                    for (const node_t* a = e->b; a != NULL; a = a->next)
                        if (fraction(c, sc, a))
                            return true;
                    return false;
                }
                return true;                        /* random, sqrt, sin, ... */
            }
            if (f->kind == DMVS_JS_IDENT)
                return name_in(sc->returns_fraction, sc->returns_count, f->text);
            return false;
        }
        default:
            return false;
    }
}

static void mark_fraction(compiler_t* c, scan_t* sc, const char* name)
{
    if (name != NULL && !fractional_name(c, name))
    {
        name_add(c->fractional, &c->fractional_count, name);
        sc->changed = true;
    }
}

/* One pass over a tree: assigned names; fractions where they are found */
static void scan(compiler_t* c, scan_t* sc, const node_t* n, const node_t* function, uint32_t depth)
{
    for (; n != NULL && depth < 200u; n = n->next)
    {
        switch (n->kind)
        {
            case DMVS_JS_ASSIGN:
                targets(c, n->a);
                if (n->a->kind == DMVS_JS_IDENT && (n->op == DMVS_JS_OP_DIV || fraction(c, sc, n->b)))
                    mark_fraction(c, sc, n->a->text);
                break;
            case DMVS_JS_UPDATE:
                targets(c, n->a);
                break;
            case DMVS_JS_FOR_IN:
            case DMVS_JS_FOR_OF:
                if (n->a != NULL && n->a->kind != DMVS_JS_VAR)
                    targets(c, n->a);
                else if (n->a != NULL && n->a->a != NULL)
                    targets(c, n->a->a->a);         /* for (let x of ...): x is set each time */
                break;
            case DMVS_JS_DECLARATOR:
                if (n->a->kind == DMVS_JS_IDENT && fraction(c, sc, n->b))
                    mark_fraction(c, sc, n->a->text);
                break;
            case DMVS_JS_RETURN:
                if (function != NULL && function->text != NULL && fraction(c, sc, n->a) &&
                    !name_in(sc->returns_fraction, sc->returns_count, function->text) && sc->returns_count < MAX_FUNCTIONS)
                {
                    sc->returns_fraction[sc->returns_count++] = function->text;
                    sc->changed = true;
                }
                break;
            case DMVS_JS_CALL:
                if (n->a->kind == DMVS_JS_IDENT)
                {
                    /* An argument that may be a fraction makes its parameter one */
                    const node_t* f = function_named(sc, n->a->text);
                    const node_t* p = (f != NULL) ? f->a : NULL;
                    for (const node_t* a = n->b; a != NULL && p != NULL; a = a->next, p = p->next)
                        if (p->kind == DMVS_JS_IDENT && fraction(c, sc, a))
                            mark_fraction(c, sc, p->text);
                }
                break;
            case DMVS_JS_FUNCTION:
                if ((n->flags & DMVS_JS_F_DECLARATION) != 0 && n->text != NULL && sc->function_count < MAX_FUNCTIONS &&
                    function_named(sc, n->text) == NULL)
                    sc->functions[sc->function_count++] = n;
                scan(c, sc, n->a, n, depth + 1U);
                if ((n->flags & DMVS_JS_F_EXPRESSION) != 0 && fraction(c, sc, n->b) && n->text != NULL)
                    mark_fraction(c, sc, n->text);
                scan(c, sc, n->b, n, depth + 1U);
                continue;
            default:
                break;
        }
        scan(c, sc, n->a, function, depth + 1U);
        scan(c, sc, n->b, function, depth + 1U);
        scan(c, sc, n->c, function, depth + 1U);
        scan(c, sc, n->d, function, depth + 1U);
    }
}

static void prescan(compiler_t* c)
{
    scan_t* sc = arena_alloc(&c->arena, sizeof(scan_t));
    if (sc == NULL)
    {
        c->failed = true;
        return;
    }
    for (int pass = 0; pass < 8; pass++)
    {
        sc->changed = false;
        for (const program_t* k = c->programs; k != NULL; k = k->next)
            scan(c, sc, k->root->a, NULL, 0);
        if (!sc->changed)
            break;
    }
}

/* ---- Scopes ---- */

static scope_t* new_scope(compiler_t* c, scope_t* parent, spec_t* spec)
{
    scope_t* s = arena_alloc(&c->arena, sizeof(scope_t));
    if (s == NULL)
    {
        c->failed = true;
        return parent;
    }
    s->parent = parent;
    s->spec = spec;
    return s;
}

static binding_t* lookup(scope_t* s, const char* name)
{
    for (; s != NULL; s = s->parent)
    {
        for (binding_t* b = s->bindings; b != NULL; b = b->next)
        {
            if (strcmp(b->name, name) == 0)
                return b;
        }
    }
    return NULL;
}

static binding_t* bind(compiler_t* c, scope_t* s, const char* name, const value_t* v, bool constant)
{
    for (binding_t* b = s->bindings; b != NULL; b = b->next)
    {
        if (strcmp(b->name, name) == 0)
        {
            b->value = *v;                          /* var x again, a function redeclared */
            b->constant = constant;
            return b;
        }
    }
    binding_t* b = arena_alloc(&c->arena, sizeof(binding_t));
    if (b == NULL)
    {
        c->failed = true;
        return NULL;
    }
    b->name = name;
    b->value = *v;
    b->constant = constant;
    b->next = s->bindings;
    s->bindings = b;
    return b;
}

/* A runtime value in a variable of its own (a temporary would be reused) */
static value_t own_var(compiler_t* c, const char* name, const value_t* v)
{
    uint8_t type = v->type, scale = (v->type == DMVS_JS_T_NUMBER) ? v->scale : 0u;
    if (type == DMVS_JS_T_NUMBER && fractional_name(c, name))
        scale = FIX;
    value_t var = v_runtime(type, scale, new_var(c, name, type, 0, ""));
    assign_to(c, &var, v);
    return var;
}

/* ---- Loops of the code being made ---- */


static loop_t* current_loop(compiler_t* c)
{
    return (c->loop_depth > 0) ? c->loops[c->loop_depth - 1U] : NULL;
}

static void push_loop(compiler_t* c, loop_t* l)
{
    l->blocks = c->code->blocks;
    if (c->loop_depth < MAX_LOOPS)
        c->loops[c->loop_depth++] = l;
    else
        report(c, "loops nested too deep - not converted");
}

static void pop_loop(compiler_t* c)
{
    if (c->loop_depth > 0)
        c->loop_depth--;
}


/* Whether the code ahead is skipped: an unrolled loop's break / continue reached */
static bool skipping(compiler_t* c)
{
    loop_t* l = current_loop(c);
    return l != NULL && !l->runtime && (l->broken || l->continued);
}

/* ---- Expressions ---- */

static double static_double(const value_t* v)
{
    double n = 0.0;
    if (static_number(v, &n))
        return n;
    if (v->kind == DMVS_JS_V_STRING)
    {
        if (v->length == 0)
            return 0.0;
        return parse_number(v->text, v->length, &n) ? n : (0.0 / 0.0);
    }
    return 0.0 / 0.0;                               /* NaN */
}

static bool is_text(const value_t* v)
{
    return v->kind == DMVS_JS_V_STRING || (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_TEXT);
}

static bool numeric(const value_t* v)
{
    return v->kind == DMVS_JS_V_RUNTIME && v->type != DMVS_JS_T_TEXT;
}

/* The scale a static number needs: 0 for an integer */
static uint8_t static_scale(const value_t* v)
{
    double n;
    return (static_number(v, &n) && n != (double)(int64_t)n) ? (uint8_t)FIX : 0u;
}

static uint8_t scale_of(const value_t* v)
{
    return is_static(v) ? static_scale(v) : number_scale(v);
}

/* a op b, numbers: runtime when either is */
static value_t arithmetic(compiler_t* c, uint8_t op, const value_t* a, const value_t* b)
{
    if (is_static(a) && is_static(b))
    {
        double x = static_double(a), y = static_double(b);
        switch (op)
        {
            case DMVS_JS_OP_ADD: return v_number(x + y);
            case DMVS_JS_OP_SUB: return v_number(x - y);
            case DMVS_JS_OP_MUL: return v_number(x * y);
            case DMVS_JS_OP_DIV: return v_number(x / y);
            case DMVS_JS_OP_MOD:
            {
                if (y == 0.0)
                    return v_number(0.0 / 0.0);
                double q = x / y;
                q = (q < 0) ? -(double)(int64_t)(-q) : (double)(int64_t)q;
                return v_number(x - q * y);
            }
            case DMVS_JS_OP_POW:
            {
                double r = 1.0;
                int64_t e = (int64_t)y;
                if ((double)e != y || e < 0 || e > 64)
                {
                    report(c, "a power that is not a small whole number - not converted");
                    return v_number(0.0);
                }
                while (e-- > 0)
                    r *= x;
                return v_number(r);
            }
            default:
                return v_number(0.0);
        }
    }
    if (!(numeric(a) || is_static(a)) || !(numeric(b) || is_static(b)))
    {
        report(c, "arithmetic on what is not a number - not converted");
        return v_number(0.0);
    }
    uint8_t sa = scale_of(a), sb = scale_of(b);
    dmvsi_var_t var;
    int32_t imm;
    value_t r;
    switch (op)
    {
        case DMVS_JS_OP_ADD:
        case DMVS_JS_OP_SUB:
        case DMVS_JS_OP_MOD:
        {
            uint8_t s = (sa > sb) ? sa : sb;
            r = number_copy(c, a, s);
            if (!number_operand(c, b, s, &var, &imm))
                return r;
            emit_op(c, (op == DMVS_JS_OP_ADD) ? DMVSI_ACT_ADD : (op == DMVS_JS_OP_SUB) ? DMVSI_ACT_SUB : DMVSI_ACT_MOD,
                    r.var, var, imm, NULL);
            return r;
        }
        case DMVS_JS_OP_MUL:
        {
            uint8_t s = (sa == FIX || sb == FIX) ? (uint8_t)FIX : 0u;
            r = number_copy(c, a, s);
            if (!number_operand(c, b, sb, &var, &imm))
                return r;
            emit_op(c, DMVSI_ACT_MUL, r.var, var, imm, NULL);
            if (sb == FIX)
                emit_op(c, DMVSI_ACT_DIV, r.var, 0, 1000, NULL);       /* b's thousandths */
            return r;
        }
        case DMVS_JS_OP_DIV:
        {
            r = number_copy(c, a, FIX);
            if (!number_operand(c, b, sb, &var, &imm))
                return r;
            if (sb == FIX)
                emit_op(c, DMVSI_ACT_MUL, r.var, 0, 1000, NULL);
            emit_op(c, DMVSI_ACT_DIV, r.var, var, imm, NULL);
            return r;
        }
        default:
            report(c, "an operator the view does not have for runtime numbers - not converted");
            return v_number(0.0);
    }
}

/* A runtime comparison as an IF: var op operand (the operand a variable or a number) */
typedef struct
{
    uint8_t         kind;               /* DMVSI_ACT_IF_*; 0: static (`value` is the result) */
    dmvsi_var_t     var;
    dmvsi_var_t     operand;
    int32_t         imm;
    bool            value;
} test_t;

static uint8_t if_kind(uint8_t op, bool swapped)
{
    switch (op)
    {
        case DMVS_JS_OP_EQ: case DMVS_JS_OP_SEQ: return DMVSI_ACT_IF_EQ;
        case DMVS_JS_OP_NE: case DMVS_JS_OP_SNE: return DMVSI_ACT_IF_NE;
        case DMVS_JS_OP_LT: return swapped ? DMVSI_ACT_IF_GT : DMVSI_ACT_IF_LT;
        case DMVS_JS_OP_LE: return swapped ? DMVSI_ACT_IF_GE : DMVSI_ACT_IF_LE;
        case DMVS_JS_OP_GT: return swapped ? DMVSI_ACT_IF_LT : DMVSI_ACT_IF_GT;
        case DMVS_JS_OP_GE: return swapped ? DMVSI_ACT_IF_LE : DMVSI_ACT_IF_GE;
        default: return 0;
    }
}

static test_t compare(compiler_t* c, uint8_t op, const value_t* a, const value_t* b)
{
    test_t t;
    memset(&t, 0, sizeof(t));
    if (is_static(a) && is_static(b))
    {
        bool eq;
        if (a->kind == DMVS_JS_V_STRING && b->kind == DMVS_JS_V_STRING &&
            op != DMVS_JS_OP_EQ && op != DMVS_JS_OP_SEQ && op != DMVS_JS_OP_NE && op != DMVS_JS_OP_SNE)
        {
            size_t n = (a->length < b->length) ? a->length : b->length;
            int d = memcmp(a->text, b->text, n);
            if (d == 0)
                d = (a->length < b->length) ? -1 : (a->length > b->length) ? 1 : 0;
            t.value = (op == DMVS_JS_OP_LT) ? d < 0 : (op == DMVS_JS_OP_LE) ? d <= 0 : (op == DMVS_JS_OP_GT) ? d > 0 : d >= 0;
            return t;
        }
        bool strict = op == DMVS_JS_OP_SEQ || op == DMVS_JS_OP_SNE;
        if (op == DMVS_JS_OP_EQ || op == DMVS_JS_OP_NE || strict)
        {
            if (strict)
                eq = a->kind == b->kind && same_static(a, b);
            else if ((a->kind == DMVS_JS_V_NULL || a->kind == DMVS_JS_V_UNDEFINED) ||
                     (b->kind == DMVS_JS_V_NULL || b->kind == DMVS_JS_V_UNDEFINED))
                eq = (a->kind == DMVS_JS_V_NULL || a->kind == DMVS_JS_V_UNDEFINED) &&
                     (b->kind == DMVS_JS_V_NULL || b->kind == DMVS_JS_V_UNDEFINED);
            else if (a->kind == b->kind)
                eq = same_static(a, b);
            else
                eq = static_double(a) == static_double(b);
            t.value = (op == DMVS_JS_OP_EQ || op == DMVS_JS_OP_SEQ) ? eq : !eq;
            return t;
        }
        double x = static_double(a), y = static_double(b);
        t.value = (op == DMVS_JS_OP_LT) ? x < y : (op == DMVS_JS_OP_LE) ? x <= y : (op == DMVS_JS_OP_GT) ? x > y : x >= y;
        return t;
    }
    if (is_text(a) || is_text(b))
    {
        report(c, "a comparison of runtime texts - not converted");
        return t;
    }
    if (!(numeric(a) || is_static(a)) || !(numeric(b) || is_static(b)))
    {
        report(c, "a comparison of what is not a number - not converted");
        return t;
    }
    bool swapped = is_static(a);
    const value_t* x = swapped ? b : a;
    const value_t* y = swapped ? a : b;
    uint8_t s = (scale_of(x) > scale_of(y)) ? scale_of(x) : scale_of(y);
    int32_t imm;
    if (!number_operand(c, x, s, &t.var, &imm) || !number_operand(c, y, s, &t.operand, &t.imm))
        return t;
    t.kind = if_kind(op, swapped);
    return t;
}

/* A test as a boolean value */
static value_t test_value(compiler_t* c, const test_t* t)
{
    if (t->kind == 0)
        return v_bool(t->value);
    dmvsi_var_t r = temp(c, DMVS_JS_T_BOOL);
    emit_op(c, DMVSI_ACT_SET, r, 0, 0, NULL);
    emit_op(c, t->kind, t->var, t->operand, t->imm, NULL);
    emit_op(c, DMVSI_ACT_SET, r, 0, 1, NULL);
    emit_end(c);
    return v_runtime(DMVS_JS_T_BOOL, 0, r);
}

/* A condition (an IF's, a loop's) as a test - a comparison as it is, anything else by its truth */
static test_t condition(compiler_t* c, scope_t* s, const node_t* n)
{
    test_t t;
    memset(&t, 0, sizeof(t));
    if (n->kind == DMVS_JS_BINARY && if_kind(n->op, false) != 0)
    {
        value_t a = expression(c, s, n->a);
        value_t b = expression(c, s, n->b);
        return compare(c, n->op, &a, &b);
    }
    value_t v = expression(c, s, n);
    if (is_static(&v))
    {
        t.value = truthy(&v);
        return t;
    }
    if (v.kind == DMVS_JS_V_RUNTIME && v.type != DMVS_JS_T_TEXT)
    {
        t.kind = DMVSI_ACT_IF_NE;                   /* A number, a boolean, an element: not 0 */
        t.var = v.var;
        return t;
    }
    value_t b = to_bool(c, &v);
    if (is_static(&b))
    {
        t.value = truthy(&b);
        return t;
    }
    t.kind = DMVSI_ACT_IF_NE;
    t.var = b.var;
    return t;
}

/* A value picked from a static array by a runtime index: each element's value (or its property) */
static value_t select(compiler_t* c, const object_t* sel, const char* property)
{
    const object_t* a = sel->array;
    value_t picks[MAX_SELECT];
    uint32_t n = (a->count < MAX_SELECT) ? a->count : MAX_SELECT;
    bool texts = false, numbers = false;
    uint8_t scale = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        picks[i] = a->values[i];
        if (property != NULL)
        {
            const object_t* o = as_object(&a->values[i], O_OBJECT);
            picks[i] = v_undefined();
            for (uint32_t k = 0; o != NULL && k < o->count; k++)
                if (strcmp(o->keys[k], property) == 0)
                    picks[i] = o->values[k];
        }
        if (!is_static(&picks[i]) || picks[i].kind == DMVS_JS_V_OBJECT || picks[i].kind == DMVS_JS_V_INTERNAL)
        {
            report(c, "picking what is not a number or a text by a runtime index - not converted");
            return v_undefined();
        }
        texts = texts || picks[i].kind == DMVS_JS_V_STRING;
        numbers = numbers || picks[i].kind != DMVS_JS_V_STRING;
        if (static_scale(&picks[i]) == FIX)
            scale = FIX;
    }
    if (texts && numbers)
    {
        report(c, "picking numbers and texts by a runtime index - not converted");
        return v_undefined();
    }
    value_t r = texts ? v_runtime(DMVS_JS_T_TEXT, 0, temp(c, DMVS_JS_T_TEXT))
                      : v_runtime(DMVS_JS_T_NUMBER, scale, temp(c, DMVS_JS_T_NUMBER));
    for (uint32_t i = 0; i < n; i++)
    {
        emit_op(c, DMVSI_ACT_IF_EQ, sel->index, 0, (int32_t)i, NULL);
        assign_to(c, &r, &picks[i]);
        emit_end(c);
    }
    return r;
}

/* Whether object.name is the host's: its object, or a variable that holds one (an integer, not a number's method) */
static bool host_object(const value_t* v, const char* name)
{
    if (v->kind == DMVS_JS_V_OBJECT)
        return true;
    return v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_NUMBER && v->scale == 0 && name != NULL &&
           strcmp(name, "toFixed") != 0 && strcmp(name, "toString") != 0;
}

/* object.name - static, the host's, a selection's */
static value_t member(compiler_t* c, const value_t* object, const char* name)
{
    value_t out = v_undefined();
    if (host_object(object, name))
    {
        if (c->host.get == NULL || c->host.get(c->host.ctx, (dmvs_js_compiler_t)c, object, name, &out) != 0)
        {
            char m[96];
            Dmod_SnPrintf(m, sizeof(m), "the element's %s - not converted", name);
            report(c, m);
            return v_undefined();
        }
        return out;
    }
    const object_t* sel = as_object(object, O_SELECT);
    if (sel != NULL)
        return select(c, sel, name);
    const object_t* o = as_object(object, O_OBJECT);
    if (o != NULL)
    {
        for (uint32_t i = 0; i < o->count; i++)
            if (strcmp(o->keys[i], name) == 0)
                return o->values[i];
    }
    const object_t* a = as_object(object, O_ARRAY);
    if (a != NULL && strcmp(name, "length") == 0)
        return v_number((double)a->count);
    if (object->kind == DMVS_JS_V_STRING && strcmp(name, "length") == 0)
        return v_number((double)object->length);
    if (builtin_member(c, object, name, &out))
        return out;
    if (o == NULL)
    {
        char m[96];
        Dmod_SnPrintf(m, sizeof(m), "the property %s of this value - not converted", name);
        report(c, m);
    }
    return out;
}

/* object[index] */
static value_t index_of(compiler_t* c, const value_t* object, const value_t* index)
{
    const object_t* a = as_object(object, O_ARRAY);
    if (is_static(index))
    {
        if (index->kind == DMVS_JS_V_STRING)
            return member(c, object, index->text);
        double n = static_double(index);
        if (a != NULL)
            return (n >= 0 && n < a->count && n == (double)(uint32_t)n) ? a->values[(uint32_t)n] : v_undefined();
        if (object->kind == DMVS_JS_V_STRING && n >= 0 && n < (double)object->length)
            return v_string(c, object->text + (size_t)n, 1);
        bool ok;
        value_t key = to_static_string(c, index, &ok);
        return member(c, object, key.text);
    }
    if (a != NULL && numeric(index))
    {
        /* An element picked when the view runs: its index kept (it may change before it is used) */
        object_t* sel = new_object(c, O_SELECT);
        if (sel == NULL)
            return v_undefined();
        sel->array = a;
        sel->index = new_var(c, "index", DMVS_JS_T_NUMBER, 0, NULL);
        value_t iv = v_runtime(DMVS_JS_T_NUMBER, 0, sel->index);
        assign_to(c, &iv, index);
        return v_internal(sel);
    }
    report(c, "an index known only when the view runs - not converted");
    return v_undefined();
}

static value_t array_literal(compiler_t* c, scope_t* s, const node_t* n)
{
    uint32_t count = 0;
    for (const node_t* k = n->a; k != NULL; k = k->next)
        count++;
    object_t* a = new_object(c, O_ARRAY);
    if (a == NULL)
        return v_undefined();
    a->values = arena_alloc(&c->arena, (count + 1U) * sizeof(value_t));
    if (a->values == NULL)
    {
        c->failed = true;
        return v_undefined();
    }
    for (const node_t* k = n->a; k != NULL; k = k->next)
    {
        c->at = k;
        if (k->kind == DMVS_JS_SPREAD)
        {
            report(c, "spread in an array - not converted");
            continue;
        }
        value_t v = (k->kind == DMVS_JS_HOLE) ? v_undefined() : expression(c, s, k);
        if (v.kind == DMVS_JS_V_RUNTIME)
            v = own_var(c, "item", &v);
        a->values[a->count++] = v;
    }
    return v_internal(a);
}

static value_t function_value(compiler_t* c, scope_t* s, const node_t* n);

static value_t object_literal(compiler_t* c, scope_t* s, const node_t* n)
{
    uint32_t count = 0;
    for (const node_t* k = n->a; k != NULL; k = k->next)
        count++;
    object_t* o = new_object(c, O_OBJECT);
    if (o == NULL)
        return v_undefined();
    o->values = arena_alloc(&c->arena, (count + 1U) * sizeof(value_t));
    o->keys = arena_alloc(&c->arena, (count + 1U) * sizeof(const char*));
    if (o->values == NULL || o->keys == NULL)
    {
        c->failed = true;
        return v_undefined();
    }
    for (const node_t* p = n->a; p != NULL; p = p->next)
    {
        c->at = p;
        if (p->kind != DMVS_JS_PROPERTY || (p->flags & (DMVS_JS_F_COMPUTED | DMVS_JS_F_GETTER | DMVS_JS_F_SETTER)) != 0)
        {
            report(c, "a property the compiler does not take (spread, computed, getter) - not converted");
            continue;
        }
        const char* key = p->a->text;
        char number_key[32];
        if (p->a->kind == DMVS_JS_NUMBER)
        {
            bool ok;
            value_t k = v_number((double)p->a->number);
            for (uint8_t d = 0; d < p->a->decimals; d++)
                k.number /= 10.0;
            value_t ks = to_static_string(c, &k, &ok);
            Dmod_SnPrintf(number_key, sizeof(number_key), "%s", ks.text);
            key = arena_strndup(&c->arena, number_key, strlen(number_key));
        }
        value_t v = expression(c, s, p->b);
        if (v.kind == DMVS_JS_V_RUNTIME)
            v = own_var(c, key, &v);
        o->keys[o->count] = key;
        o->values[o->count++] = v;
    }
    return v_internal(o);
}

static value_t function_value(compiler_t* c, scope_t* s, const node_t* n)
{
    object_t* f = new_object(c, O_FUNCTION);
    if (f == NULL)
        return v_undefined();
    f->node = n;
    f->env = s;
    if ((n->flags & DMVS_JS_F_ARROW) != 0)
    {
        binding_t* self = lookup(s, "this");
        f->self = (self != NULL) ? self->value : v_undefined();
    }
    else
        f->self = v_undefined();
    return v_internal(f);
}

/* The parts of a template literal (its chunks and expressions) joined */
static value_t template_value(compiler_t* c, scope_t* s, const node_t* n)
{
    if (n->b != NULL)
    {
        report(c, "a tagged template - not converted");
        return v_string(c, "", 0);
    }
    value_t parts[32];
    uint32_t count = 0;
    for (const node_t* k = n->a; k != NULL && count < 32u; k = k->next)
        parts[count++] = (k->kind == DMVS_JS_STRING && k == n->a) ? v_string(c, k->text, k->length) : expression(c, s, k);
    return concat(c, parts, count);
}

/* Set a target (a name, the host's object.name) to a value; compound: the operator's result */
static value_t assign(compiler_t* c, scope_t* s, const node_t* target, uint8_t op, const value_t* value)
{
    value_t v = *value;
    if (target->kind == DMVS_JS_IDENT)
    {
        binding_t* b = lookup(s, target->text);
        if (b == NULL)
        {
            report(c, "an assignment to a name never declared - not converted");
            return v;
        }
        if (op != DMVS_JS_OP_ASSIGN)
        {
            value_t r = (op == DMVS_JS_OP_ADD && (is_text(&b->value) || is_text(&v))) ? concat(c, (value_t[]){ b->value, v }, 2)
                                                                                   : arithmetic(c, op, &b->value, &v);
            v = r;
        }
        if (b->value.kind == DMVS_JS_V_RUNTIME)
        {
            assign_to(c, &b->value, &v);
            return b->value;
        }
        if (b->constant)
        {
            report(c, "an assignment to a constant - not converted");
            return v;
        }
        if (c->code == &c->init && c->code->blocks == 0 && is_static(&v))
        {
            b->value = v;                           /* Still known: while the script loads, outside conditions */
            return v;
        }
        report(c, "an assignment the compiler cannot follow - not converted");
        return v;
    }
    if (target->kind == DMVS_JS_MEMBER)
    {
        value_t object = expression(c, s, target->a);
        const char* name = member_name(target);
        value_t key;
        if (name == NULL)
        {
            key = expression(c, s, target->b);
            bool ok;
            value_t ks = is_static(&key) ? to_static_string(c, &key, &ok) : v_undefined();
            name = (ks.kind == DMVS_JS_V_STRING) ? ks.text : NULL;
        }
        if (name == NULL)
        {
            report(c, "an assignment to an index known only when the view runs - not converted");
            return v;
        }
        if (op != DMVS_JS_OP_ASSIGN)
        {
            value_t old = member(c, &object, name);
            v = (op == DMVS_JS_OP_ADD && (is_text(&old) || is_text(&v))) ? concat(c, (value_t[]){ old, v }, 2)
                                                                       : arithmetic(c, op, &old, &v);
        }
        if (host_object(&object, name))
        {
            if (c->host.set == NULL || c->host.set(c->host.ctx, (dmvs_js_compiler_t)c, &object, name, &v) != 0)
            {
                char m[96];
                Dmod_SnPrintf(m, sizeof(m), "setting the element's %s - not converted", name);
                report(c, m);
            }
            return v;
        }
        object_t* o = (object_t*)as_object(&object, O_OBJECT);
        if (o != NULL && c->code == &c->init && c->code->blocks == 0 && is_static(&v))
        {
            for (uint32_t i = 0; i < o->count; i++)
            {
                if (strcmp(o->keys[i], name) == 0)
                {
                    o->values[i] = v;
                    return v;
                }
            }
        }
        report(c, "setting a property the view does not keep - not converted");
        return v;
    }
    report(c, "an assignment to a pattern - not converted");
    return v;
}

static value_t call_expression(compiler_t* c, scope_t* s, const node_t* n)
{
    value_t args[MAX_ARGS];
    uint32_t count = 0;
    const node_t* callee = n->a;
    value_t self = v_undefined(), fn;
    const char* method = NULL;

    if (callee->kind == DMVS_JS_MEMBER)
    {
        self = expression(c, s, callee->a);
        method = member_name(callee);
        if (method == NULL)
        {
            value_t key = expression(c, s, callee->b);
            bool ok;
            value_t ks = is_static(&key) ? to_static_string(c, &key, &ok) : v_undefined();
            method = (ks.kind == DMVS_JS_V_STRING) ? ks.text : NULL;
            if (method == NULL)
            {
                report(c, "a method known only when the view runs - not converted");
                return v_undefined();
            }
        }
    }
    for (const node_t* a = n->b; a != NULL; a = a->next)
    {
        c->at = a;
        if (a->kind == DMVS_JS_SPREAD || count >= MAX_ARGS)
        {
            report(c, "spread arguments, or too many - not converted");
            return v_undefined();
        }
        args[count++] = expression(c, s, a);
    }
    c->at = n;

    if (method != NULL && host_object(&self, method))
    {
        /* The host's method */
        value_t out = v_undefined();
        if (c->host.call == NULL || c->host.call(c->host.ctx, (dmvs_js_compiler_t)c, &self, method, args, count, &out) != 0)
        {
            char m[96];
            Dmod_SnPrintf(m, sizeof(m), "the element's %s() - not converted", method);
            report(c, m);
        }
        c->at = n;
        return out;
    }
    fn = (method != NULL) ? member(c, &self, method) : expression(c, s, callee);
    return call_value(c, &fn, &self, args, count);
}

static value_t expression(compiler_t* c, scope_t* s, const node_t* n)
{
    if (n == NULL)
        return v_undefined();
    c->at = n;
    switch (n->kind)
    {
        case DMVS_JS_NUMBER:
        {
            double v = (double)n->number;
            for (uint8_t d = 0; d < n->decimals; d++)
                v /= 10.0;
            return v_number(v);
        }
        case DMVS_JS_STRING:
            return v_string(c, n->text, n->length);
        case DMVS_JS_TEMPLATE:
            return template_value(c, s, n);
        case DMVS_JS_BOOL:
            return v_bool(n->op != 0);
        case DMVS_JS_NULL:
        {
            value_t v = v_undefined();
            v.kind = DMVS_JS_V_NULL;
            return v;
        }
        case DMVS_JS_THIS:
        {
            binding_t* b = lookup(s, "this");
            return (b != NULL) ? b->value : v_undefined();
        }
        case DMVS_JS_IDENT:
        {
            binding_t* b = lookup(s, n->text);
            if (b != NULL)
                return b->value;
            value_t v = v_undefined();
            if (strcmp(n->text, "undefined") == 0)
                return v;
            if (strcmp(n->text, "NaN") == 0)
                return v_number(0.0 / 0.0);
            if (builtin_global(c, n->text, &v))
                return v;
            if (c->host.global != NULL && c->host.global(c->host.ctx, (dmvs_js_compiler_t)c, n->text, &v))
                return v;
            char m[96];
            Dmod_SnPrintf(m, sizeof(m), "the name %s - not converted", n->text);
            report(c, m);
            return v_undefined();
        }
        case DMVS_JS_ARRAY:
            return array_literal(c, s, n);
        case DMVS_JS_OBJECT:
            return object_literal(c, s, n);
        case DMVS_JS_FUNCTION:
            return function_value(c, s, n);
        case DMVS_JS_MEMBER:
        {
            value_t object = expression(c, s, n->a);
            c->at = n;
            if ((n->flags & DMVS_JS_F_OPTIONAL) != 0 && is_static(&object) &&
                (object.kind == DMVS_JS_V_NULL || object.kind == DMVS_JS_V_UNDEFINED))
                return v_undefined();
            const char* name = member_name(n);
            if (name != NULL)
                return member(c, &object, name);
            value_t index = expression(c, s, n->b);
            c->at = n;
            return index_of(c, &object, &index);
        }
        case DMVS_JS_CALL:
            return call_expression(c, s, n);
        case DMVS_JS_NEW:
            report(c, "new - not converted");
            return v_undefined();
        case DMVS_JS_UNARY:
        {
            value_t a = expression(c, s, n->a);
            c->at = n;
            switch (n->op)
            {
                case DMVS_JS_OP_NOT:
                {
                    if (is_static(&a))
                        return v_bool(!truthy(&a));
                    value_t b = to_bool(c, &a);
                    if (is_static(&b))
                        return v_bool(!truthy(&b));
                    dmvsi_var_t r = temp(c, DMVS_JS_T_BOOL);
                    emit_op(c, DMVSI_ACT_SET, r, 0, 1, NULL);
                    emit_op(c, DMVSI_ACT_IF_NE, b.var, 0, 0, NULL);
                    emit_op(c, DMVSI_ACT_SET, r, 0, 0, NULL);
                    emit_end(c);
                    return v_runtime(DMVS_JS_T_BOOL, 0, r);
                }
                case DMVS_JS_OP_SUB:
                {
                    value_t m1 = v_number(-1.0);
                    if (is_static(&a))
                        return v_number(-static_double(&a));
                    return arithmetic(c, DMVS_JS_OP_MUL, &a, &m1);
                }
                case DMVS_JS_OP_ADD:
                    return is_static(&a) ? v_number(static_double(&a)) : a;
                case DMVS_JS_OP_TYPEOF:
                {
                    const char* t = (a.kind == DMVS_JS_V_UNDEFINED) ? "undefined" : (a.kind == DMVS_JS_V_BOOL) ? "boolean" :
                                    (a.kind == DMVS_JS_V_NUMBER) ? "number" : (a.kind == DMVS_JS_V_STRING) ? "string" :
                                    (as_object(&a, O_FUNCTION) != NULL || as_object(&a, O_BUILTIN) != NULL) ? "function" :
                                    (a.kind == DMVS_JS_V_RUNTIME) ? ((a.type == DMVS_JS_T_TEXT) ? "string" :
                                                                     (a.type == DMVS_JS_T_BOOL) ? "boolean" : "number") : "object";
                    return v_string(c, t, strlen(t));
                }
                case DMVS_JS_OP_VOID:
                    return v_undefined();
                default:
                    report(c, "an operator the compiler does not take - not converted");
                    return v_undefined();
            }
        }
        case DMVS_JS_UPDATE:
        {
            value_t one = v_number(1.0);
            value_t old = expression(c, s, n->a);
            value_t before = (!is_static(&old) && (n->flags & DMVS_JS_F_PREFIX) == 0) ? number_copy(c, &old, number_scale(&old)) : old;
            value_t r = assign(c, s, n->a, (n->op == DMVS_JS_OP_INC) ? DMVS_JS_OP_ADD : DMVS_JS_OP_SUB, &one);
            return ((n->flags & DMVS_JS_F_PREFIX) != 0) ? r : before;
        }
        case DMVS_JS_BINARY:
        {
            value_t a = expression(c, s, n->a);
            value_t b = expression(c, s, n->b);
            c->at = n;
            if (if_kind(n->op, false) != 0)
            {
                test_t t = compare(c, n->op, &a, &b);
                return test_value(c, &t);
            }
            if (n->op == DMVS_JS_OP_ADD && (is_text(&a) || is_text(&b) || as_object(&a, O_ARRAY) || as_object(&b, O_ARRAY)))
                return concat(c, (value_t[]){ a, b }, 2);
            if (n->op == DMVS_JS_OP_IN || n->op == DMVS_JS_OP_INSTANCEOF || n->op >= DMVS_JS_OP_SHL)
            {
                report(c, "an operator the compiler does not take - not converted");
                return v_undefined();
            }
            return arithmetic(c, n->op, &a, &b);
        }
        case DMVS_JS_LOGICAL:
        {
            value_t a = expression(c, s, n->a);
            c->at = n;
            if (is_static(&a))
            {
                bool nullish = a.kind == DMVS_JS_V_NULL || a.kind == DMVS_JS_V_UNDEFINED;
                bool take_b = (n->op == DMVS_JS_OP_AND) ? truthy(&a) : (n->op == DMVS_JS_OP_OR) ? !truthy(&a) : nullish;
                return take_b ? expression(c, s, n->b) : a;
            }
            if (n->op == DMVS_JS_OP_COALESCE)
                return a;                           /* A runtime value is never null */
            /* Runtime: true or false, as a condition takes it */
            value_t ab = to_bool(c, &a);
            dmvsi_var_t r = temp(c, DMVS_JS_T_BOOL);
            emit_op(c, DMVSI_ACT_SET, r, ab.var, 0, NULL);
            emit_op(c, (n->op == DMVS_JS_OP_AND) ? DMVSI_ACT_IF_NE : DMVSI_ACT_IF_EQ, r, 0, 0, NULL);
            c->code->blocks++;
            value_t b = expression(c, s, n->b);
            value_t bb = to_bool(c, &b);
            value_t rv = v_runtime(DMVS_JS_T_BOOL, 0, r);
            assign_to(c, &rv, &bb);
            c->code->blocks--;
            emit_end(c);
            return rv;
        }
        case DMVS_JS_CONDITIONAL:
        {
            test_t t = condition(c, s, n->a);
            c->at = n;
            if (t.kind == 0)
                return expression(c, s, t.value ? n->b : n->c);
            emit_op(c, t.kind, t.var, t.operand, t.imm, NULL);
            c->code->blocks++;
            value_t a = expression(c, s, n->b);
            value_t r;
            if (is_text(&a))
                r = v_runtime(DMVS_JS_T_TEXT, 0, temp(c, DMVS_JS_T_TEXT));
            else
                r = v_runtime((a.kind == DMVS_JS_V_BOOL || (a.kind == DMVS_JS_V_RUNTIME && a.type == DMVS_JS_T_BOOL)) ?
                              DMVS_JS_T_BOOL : DMVS_JS_T_NUMBER, FIX, temp(c, DMVS_JS_T_NUMBER));
            assign_to(c, &r, &a);
            emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
            value_t b = expression(c, s, n->c);
            assign_to(c, &r, &b);
            c->code->blocks--;
            emit_end(c);
            return r;
        }
        case DMVS_JS_ASSIGN:
        {
            value_t v = expression(c, s, n->b);
            c->at = n;
            return assign(c, s, n->a, n->op, &v);
        }
        case DMVS_JS_SEQUENCE:
        {
            value_t v = v_undefined();
            for (const node_t* k = n->a; k != NULL; k = k->next)
                v = expression(c, s, k);
            return v;
        }
        default:
            report(c, "an expression the compiler does not take - not converted");
            return v_undefined();
    }
}

/* ---- Functions ---- */

static bool same_args(const spec_t* sp, const value_t* self, const value_t* args, uint32_t count)
{
    if (!same_static(&sp->self, self) || sp->self.kind != self->kind)
        return false;
    for (uint32_t i = 0; i < MAX_ARGS; i++)
    {
        const value_t* a = (i < count) ? &args[i] : NULL;
        const value_t* p = (i < sp->argc) ? &sp->args[i] : NULL;
        bool a_runtime = a != NULL && !is_static(a);
        bool p_runtime = p != NULL && p->kind == DMVS_JS_V_RUNTIME && p->var != 0 && (p->number_var == 1);
        if (a_runtime != p_runtime)
            return false;
        if (a_runtime)
        {
            if (a->kind == DMVS_JS_V_RUNTIME && p->type != a->type)
                return false;
            continue;
        }
        value_t undef = v_undefined();
        if (!same_static((a != NULL) ? a : &undef, (p != NULL) ? p : &undef) ||
            ((a != NULL) ? a->kind : DMVS_JS_V_UNDEFINED) != ((p != NULL) ? p->kind : DMVS_JS_V_UNDEFINED))
            return false;
    }
    return true;
}

/* Its statements, or its expression (an arrow's) as what it returns */
static void function_body(compiler_t* c, scope_t* s, spec_t* sp)
{
    const node_t* fn = sp->fn->node;
    if ((fn->flags & DMVS_JS_F_EXPRESSION) != 0)
    {
        value_t v = expression(c, s, fn->b);
        c->at = fn->b;
        if (v.kind != DMVS_JS_V_UNDEFINED)
        {
            /* As `return v` */
            if (is_static(&v))
            {
                sp->returns = true;
                sp->ret = v;
                sp->ret_static_only = true;
            }
            else
            {
                sp->returns = true;
                sp->ret_runtime = true;
                sp->ret = own_var(c, "ret", &v);
            }
        }
        release_temps(c);
        return;
    }
    (void)statements(c, s, fn->b->a);
}

/* A function's handler for these arguments: made, or made now (fresh: not the one being compiled - a recursive call's) */
static spec_t* specialize(compiler_t* c, const object_t* f, const value_t* self, const value_t* args, uint32_t count,
                          bool fresh)
{
    for (spec_t* sp = c->specs; sp != NULL; sp = sp->next)
    {
        if (sp->fn == f && same_args(sp, self, args, count) && !(fresh && sp->compiling))
            return sp;
    }
    spec_t* sp = arena_alloc(&c->arena, sizeof(spec_t));
    if (sp == NULL)
    {
        c->failed = true;
        return NULL;
    }
    sp->fn = f;
    sp->self = *self;
    sp->name = (f->node->text != NULL) ? f->node->text : "fn";
    sp->next = c->specs;
    c->specs = sp;

    /* Its code, its scope: parameters static or runtime variables */
    code_t* code = arena_alloc(&c->arena, sizeof(code_t));
    if (code == NULL)
    {
        c->failed = true;
        return NULL;
    }
    static uint32_t serial;
    char prefix[48];
    Dmod_SnPrintf(prefix, sizeof(prefix), "%s%u", sp->name, (unsigned)++serial);
    code->prefix = arena_strndup(&c->arena, prefix, strlen(prefix));
    code->spec = sp;
    flush_host(c);                                  /* The host's changes kept back are the caller's */
    code->outer = c->code;
    c->code = code;
    sp->compiling = true;

    scope_t* s = new_scope(c, f->env, sp);
    if ((f->node->flags & DMVS_JS_F_ARROW) == 0)
        bind(c, s, "this", self, true);
    uint32_t i = 0;
    for (const node_t* p = f->node->a; p != NULL; p = p->next, i++)
    {
        const node_t* name = (p->kind == DMVS_JS_ASSIGN) ? p->a : p;
        value_t a = (i < count) ? args[i] : v_undefined();
        if (a.kind == DMVS_JS_V_UNDEFINED && p->kind == DMVS_JS_ASSIGN)
            a = expression(c, s, p->b);             /* Its default */
        if (name->kind != DMVS_JS_IDENT)
        {
            c->at = p;
            report(c, "a parameter that is a pattern or a rest - not converted");
            continue;
        }
        if (i < MAX_ARGS)
        {
            if (!is_static(&a) && a.kind == DMVS_JS_V_RUNTIME)
            {
                uint8_t scale = (a.type == DMVS_JS_T_NUMBER && (fractional_name(c, name->text) || a.scale == FIX)) ? (uint8_t)FIX : 0u;
                value_t pv = v_runtime(a.type, scale, new_var(c, name->text, a.type, 0, ""));
                pv.number_var = 1;                  /* (a mark: set at each call) */
                sp->args[i] = pv;
                pv.number_var = 0;
                bind(c, s, name->text, &pv, false);
            }
            else
            {
                if (!is_static(&a))
                {
                    c->at = p;
                    report(c, "an element picked at runtime as an argument - not converted");
                }
                sp->args[i] = a;
                bind(c, s, name->text, &a, false);
            }
        }
        sp->argc = i + 1U;
    }
    for (; i < count && i < MAX_ARGS; i++)
    {
        sp->args[i] = args[i];
        sp->argc = i + 1U;
    }

    sp->ret_static_only = true;
    uint32_t loop_depth = c->loop_depth;
    c->loop_depth = 0;                               /* A break in it is not of the loops around the call */
    function_body(c, s, sp);
    flush_host(c);
    c->loop_depth = loop_depth;

    c->code = code->outer;
    sp->compiling = false;
    sp->done = true;

    /* Only a static value computed: no handler (unless one was given out while it was compiled) */
    bool only_returns = sp->ret_static_only && code->count == sp->ret_sets;
    if (only_returns)
        sp->ret_runtime = false;
    if (only_returns && sp->handler == 0)
        return sp;
    int ret = 0;
    if (sp->handler != 0)
        ret = dmvsi_set_handler(c->doc, sp->handler, code->actions, only_returns ? 0 : code->count);
    else if (code->count > 0)
        ret = ((sp->handler = dmvsi_add_handler(c->doc, code->actions, code->count)) == 0) ? -EINVAL : 0;
    if (ret != 0)
    {
        c->at = f->node;
        report(c, "a function the document does not take (its code) - not converted");
    }
    return sp;
}

value_t call_value(compiler_t* c, const value_t* callee, const value_t* self, const value_t* args, uint32_t count)
{
    const object_t* f = as_object(callee, O_FUNCTION);
    if (f == NULL)
    {
        const object_t* b = as_object(callee, O_BUILTIN);
        if (b != NULL)
            return builtin_call(c, b, args, count);
        report(c, "a call of what is not a function - not converted");
        return v_undefined();
    }
    value_t me = ((f->node->flags & DMVS_JS_F_ARROW) != 0) ? f->self : *self;
    /*
     * A recursive call (a -> b -> a, as a page's functions call each other):
     * the view's calls are not, so another copy of it - up to MAX_RECURSION
     * deep, where the call is left out.
     */
    uint32_t depth = 0;
    for (spec_t* sp = c->specs; sp != NULL; sp = sp->next)
    {
        if (sp->fn == f && sp->compiling && same_args(sp, &me, args, count))
            depth++;
    }
    if (depth > MAX_RECURSION)
    {
        report(c, "a recursive call deeper than the compiler follows - not converted");
        return v_undefined();
    }
    const node_t* at = c->at;
    spec_t* sp = specialize(c, f, &me, args, count, depth > 0);
    c->at = at;
    if (sp == NULL)
        return v_undefined();

    /* Its runtime arguments, then the call */
    for (uint32_t i = 0; i < sp->argc && i < count; i++)
    {
        if (sp->args[i].kind == DMVS_JS_V_RUNTIME && sp->args[i].number_var == 1)
        {
            value_t p = sp->args[i];
            p.number_var = 0;
            assign_to(c, &p, &args[i]);
        }
    }
    if (sp->handler != 0)
    {
        dmvsi_action_t a;
        memset(&a, 0, sizeof(a));
        a.kind = DMVSI_ACT_CALL;
        a.handler = sp->handler;
        emit(c, &a);
    }
    if (!sp->returns)
        return v_undefined();
    if (!sp->ret_runtime)
        return sp->ret;
    /* What it returned, before another call overwrites it */
    if (sp->ret.type == DMVS_JS_T_TEXT)
    {
        dmvsi_var_t t = temp(c, DMVS_JS_T_TEXT);
        emit_op(c, DMVSI_ACT_SET, t, sp->ret.var, 0, NULL);
        value_t r = v_runtime(DMVS_JS_T_TEXT, 0, t);
        r.number_var = sp->ret.number_var;
        return r;
    }
    return number_copy(c, &sp->ret, sp->ret.scale);
}

dmvsi_handler_t function_handler(compiler_t* c, const value_t* fn, const value_t* self)
{
    const object_t* f = as_object(fn, O_FUNCTION);
    if (f == NULL)
    {
        report(c, "a listener that is not a function - not converted");
        return 0;
    }
    value_t me = ((f->node->flags & DMVS_JS_F_ARROW) != 0) ? f->self : *self;
    const node_t* at = c->at;
    spec_t* sp = specialize(c, f, &me, NULL, 0, false);
    c->at = at;
    if (sp == NULL)
        return 0;
    if (sp->handler == 0 && sp->compiling)
        sp->handler = dmvsi_new_handler(c->doc);                    /* Itself (setTimeout(tick)): made when it is done */
    else if (sp->handler == 0)
        sp->handler = dmvsi_add_handler(c->doc, NULL, 0);           /* Nothing to do, but a handler */
    return sp->handler;
}

/* ---- Statements ---- */

/* The returned value of the function being compiled */
static void return_value(compiler_t* c, scope_t* s, const node_t* n)
{
    spec_t* sp = (c->code != NULL) ? c->code->spec : NULL;
    if (sp == NULL)
    {
        report(c, "return outside a function - not converted");
        return;
    }
    value_t v = expression(c, s, n->a);
    c->at = n;
    if (n->a != NULL)
    {
        if (!sp->returns)
        {
            sp->returns = true;
            if (is_static(&v) && v.kind != DMVS_JS_V_UNDEFINED)
                sp->ret = v;
        }
        else if (!(is_static(&v) && sp->ret_static_only && same_static(&v, &sp->ret)))
            sp->ret_static_only = false;
        if (!is_static(&v))
            sp->ret_static_only = false;
        if (!sp->ret_runtime && (!sp->ret_static_only || !is_static(&v)))
        {
            /* It needs a variable: one of the type of what it returns */
            value_t base = is_static(&sp->ret) ? sp->ret : v;
            uint8_t type = is_text(&base) || is_text(&v) ? DMVS_JS_T_TEXT :
                           (base.kind == DMVS_JS_V_BOOL || (base.kind == DMVS_JS_V_RUNTIME && base.type == DMVS_JS_T_BOOL)) ?
                           DMVS_JS_T_BOOL : DMVS_JS_T_NUMBER;
            uint8_t scale = (type == DMVS_JS_T_NUMBER && (scale_of(&base) == FIX || scale_of(&v) == FIX ||
                             fractional_name(c, sp->name))) ? (uint8_t)FIX : 0u;
            sp->ret = v_runtime(type, scale, new_var(c, "ret", type, 0, ""));
            sp->ret_runtime = true;
        }
        if (sp->ret_runtime)
        {
            uint32_t before = c->code->count;
            assign_to(c, &sp->ret, &v);
            if (is_static(&v))
                sp->ret_sets += c->code->count - before;
            if (v.kind == DMVS_JS_V_RUNTIME && v.type == DMVS_JS_T_TEXT && v.number_var != 0)
                sp->ret.number_var = v.number_var;
        }
    }
    emit_op(c, DMVSI_ACT_RETURN, 0, 0, 0, NULL);
    if (sp->ret_static_only)
        sp->ret_sets++;                             /* The RETURN is part of only returning */
}

/* Whether an expression is setTimeout(...) / setInterval(...) */
static bool starts_timer(compiler_t* c, scope_t* s, const node_t* e)
{
    (void)c;
    return e != NULL && e->kind == DMVS_JS_CALL && (ident_is(e->a, "setTimeout") || ident_is(e->a, "setInterval")) &&
           lookup(s, e->a->text) == NULL;
}

static int declarations(compiler_t* c, scope_t* s, const node_t* n)
{
    for (const node_t* d = n->a; d != NULL; d = d->next)
    {
        c->at = d;
        if (d->a->kind != DMVS_JS_IDENT)
        {
            report(c, "a declaration of a pattern - not converted");
            continue;
        }
        const char* name = d->a->text;
        bool constant = n->op == DMVS_JS_VAR_CONST || !assigned_name(c, name);
        if (constant && starts_timer(c, s, d->b))
        {
            /* const t = setInterval(() => { ... clearInterval(t) }): its number known before the function is compiled */
            value_t id = v_number((double)(c->site_count + 1U));
            bind(c, s, name, &id, true);
        }
        value_t v = (d->b != NULL) ? expression(c, s, d->b) : v_undefined();
        c->at = d;
        if (constant)
        {
            if (v.kind == DMVS_JS_V_RUNTIME)
                v = own_var(c, name, &v);
            else if (as_object(&v, O_SELECT) != NULL)
            {
                /* An element picked by a runtime index: its index kept as it is now */
                object_t* copy = new_object(c, O_SELECT);
                if (copy != NULL)
                {
                    *copy = *as_object(&v, O_SELECT);
                    copy->index = new_var(c, name, DMVS_JS_T_NUMBER, 0, NULL);
                    value_t iv = v_runtime(DMVS_JS_T_NUMBER, 0, copy->index);
                    value_t from = v_runtime(DMVS_JS_T_NUMBER, 0, as_object(&v, O_SELECT)->index);
                    assign_to(c, &iv, &from);
                    v = v_internal(copy);
                }
            }
            bind(c, s, name, &v, n->op == DMVS_JS_VAR_CONST);
            continue;
        }

        /* A runtime variable: of the type of what it starts as */
        uint8_t type;
        if (is_text(&v))
            type = DMVS_JS_T_TEXT;
        else if (v.kind == DMVS_JS_V_BOOL || (v.kind == DMVS_JS_V_RUNTIME && v.type == DMVS_JS_T_BOOL))
            type = DMVS_JS_T_BOOL;
        else if (v.kind == DMVS_JS_V_INTERNAL)
        {
            report(c, "a variable that holds an element or an object and changes - not converted");
            bind(c, s, name, &v, false);
            continue;
        }
        else
            type = DMVS_JS_T_NUMBER;
        uint8_t scale = (type == DMVS_JS_T_NUMBER && (fractional_name(c, name) || scale_of(&v) == FIX)) ? (uint8_t)FIX : 0u;
        bool at_load = c->code == &c->init && c->code->blocks == 0 && current_loop(c) == NULL;
        int32_t initial = 0;
        const char* text = "";
        bool initialized = false;
        if (at_load && is_static(&v))
        {
            /* Known when the view is shown: the variable's initial value */
            if (type == DMVS_JS_T_TEXT)
            {
                bool ok;
                text = to_static_string(c, &v, &ok).text;
                initialized = true;
            }
            else
            {
                dmvsi_var_t var;
                if (number_operand(c, &v, scale, &var, &initial))
                    initialized = true;
            }
        }
        value_t var = v_runtime(type, scale, new_var(c, name, type, initial, text));
        if (initialized && type == DMVS_JS_T_NUMBER && scale == 0)
            track(c, var.var, &v);                  /* let current = home: what it may hold */
        if (!initialized && v.kind != DMVS_JS_V_UNDEFINED)
            assign_to(c, &var, &v);
        else if (!initialized && c->code != &c->init)
        {
            value_t zero = (type == DMVS_JS_T_TEXT) ? v_string(c, "", 0) : v_number(0.0);
            assign_to(c, &var, &zero);              /* A function's local: as it starts each time */
        }
        bind(c, s, name, &var, false);
    }
    return 0;
}

/* Function declarations of a list of statements: there before anything runs */
static void hoist(compiler_t* c, scope_t* s, const node_t* first)
{
    for (const node_t* n = first; n != NULL; n = n->next)
    {
        if (n->kind == DMVS_JS_FUNCTION && (n->flags & DMVS_JS_F_DECLARATION) != 0 && n->text != NULL)
        {
            value_t f = function_value(c, s, n);
            bind(c, s, n->text, &f, false);
        }
    }
}

static int block(compiler_t* c, scope_t* s, const node_t* n)
{
    scope_t* inner = new_scope(c, s, s->spec);
    return statements(c, inner, (n->kind == DMVS_JS_BLOCK) ? n->a : NULL);
}

/* A statement as a block of its own (a branch: `if (a) b();`) */
static int branch(compiler_t* c, scope_t* s, const node_t* n)
{
    if (n == NULL)
        return 0;
    if (n->kind == DMVS_JS_BLOCK)
        return block(c, s, n);
    scope_t* inner = new_scope(c, s, s->spec);
    return statement(c, inner, n);
}

static int if_statement(compiler_t* c, scope_t* s, const node_t* n)
{
    test_t t = condition(c, s, n->a);
    c->at = n;
    if (t.kind == 0)
    {
        release_temps(c);
        return branch(c, s, t.value ? n->b : n->c);
    }
    emit_op(c, t.kind, t.var, t.operand, t.imm, NULL);
    release_temps(c);
    c->code->blocks++;
    branch(c, s, n->b);
    if (n->c != NULL)
    {
        emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
        branch(c, s, n->c);
    }
    c->code->blocks--;
    return emit_end(c);
}

/* for (let i = a; i < b; i++) with static bounds and a body that does not set i: unrolled */
static bool unrollable(compiler_t* c, scope_t* s, const node_t* n, const char** name, double* from, uint32_t* steps,
                       double* step)
{
    const node_t* init = n->a;
    const node_t* test = n->b;
    const node_t* update = n->c;
    if (init == NULL || init->kind != DMVS_JS_VAR || init->a == NULL || init->a->next != NULL ||
        init->a->a->kind != DMVS_JS_IDENT || test == NULL || update == NULL)
        return false;
    *name = init->a->a->text;
    /* i++, i--, i += k, i -= k */
    if (update->kind == DMVS_JS_UPDATE && ident_is(update->a, *name))
        *step = (update->op == DMVS_JS_OP_INC) ? 1.0 : -1.0;
    else if (update->kind == DMVS_JS_ASSIGN && ident_is(update->a, *name) && update->b->kind == DMVS_JS_NUMBER &&
             (update->op == DMVS_JS_OP_ADD || update->op == DMVS_JS_OP_SUB))
    {
        *step = (double)update->b->number;
        for (uint8_t d = 0; d < update->b->decimals; d++)
            *step /= 10.0;
        if (update->op == DMVS_JS_OP_SUB)
            *step = -*step;
    }
    else
        return false;
    if (test->kind != DMVS_JS_BINARY || !ident_is(test->a, *name) || if_kind(test->op, false) == 0)
        return false;
    uint32_t mark = c->code->count;
    value_t start = expression(c, s, init->a->b);
    value_t limit = expression(c, s, test->b);
    if (!is_static(&start) || !is_static(&limit) || c->code->count != mark || *step == 0.0)
        return false;
    *from = static_double(&start);
    double end = static_double(&limit);
    uint32_t k = 0;
    for (double i = *from; k <= MAX_UNROLL; i += *step, k++)
    {
        bool go = (test->op == DMVS_JS_OP_LT) ? i < end : (test->op == DMVS_JS_OP_LE) ? i <= end :
                  (test->op == DMVS_JS_OP_GT) ? i > end : (test->op == DMVS_JS_OP_GE) ? i >= end :
                  (test->op == DMVS_JS_OP_NE || test->op == DMVS_JS_OP_SNE) ? i != end : i == end;
        if (!go)
            break;
    }
    *steps = k;
    return k <= MAX_UNROLL;
}

/* Whether a statement sets a name (an unrolled loop's variable must stay as it is) */
static bool sets_name(const node_t* n, const char* name, uint32_t depth)
{
    for (; n != NULL && depth < 100u; n = n->next)
    {
        if ((n->kind == DMVS_JS_ASSIGN || n->kind == DMVS_JS_UPDATE) && ident_is(n->a, name))
            return true;
        if (sets_name(n->a, name, depth + 1U) || sets_name(n->b, name, depth + 1U) ||
            sets_name(n->c, name, depth + 1U) || sets_name(n->d, name, depth + 1U))
            return true;
    }
    return false;
}

/* Whether a loop's body has a break / continue of its own other than as one of its statements (under an if, ...) */
static bool breaks_inside(const node_t* n, bool top, uint32_t depth)
{
    for (; n != NULL && depth < 100u; n = n->next)
    {
        switch (n->kind)
        {
            case DMVS_JS_BREAK:
            case DMVS_JS_CONTINUE:
                if (!top && n->text == NULL)
                    return true;
                continue;
            case DMVS_JS_FUNCTION:
            case DMVS_JS_FOR:
            case DMVS_JS_FOR_IN:
            case DMVS_JS_FOR_OF:
            case DMVS_JS_WHILE:
            case DMVS_JS_DO_WHILE:
            case DMVS_JS_SWITCH:
                continue;                           /* Their breaks are theirs */
            case DMVS_JS_BLOCK:
                if (breaks_inside(n->a, top, depth + 1U))
                    return true;
                continue;
            default:
                if (breaks_inside(n->a, false, depth + 1U) || breaks_inside(n->b, false, depth + 1U) ||
                    breaks_inside(n->c, false, depth + 1U) || breaks_inside(n->d, false, depth + 1U))
                    return true;
                continue;
        }
    }
    return false;
}

static int loop_body(compiler_t* c, scope_t* s, const node_t* body, loop_t* l)
{
    push_loop(c, l);
    branch(c, s, body);
    pop_loop(c);
    return 0;
}

static int for_statement(compiler_t* c, scope_t* s, const node_t* n)
{
    scope_t* inner = new_scope(c, s, s->spec);
    const char* name = NULL;
    double from = 0, step = 0;
    uint32_t steps = 0;
    if (!breaks_inside(n->d, true, 0) && unrollable(c, inner, n, &name, &from, &steps, &step) && !sets_name(n->d, name, 0))
    {
        loop_t l;
        memset(&l, 0, sizeof(l));
        for (uint32_t k = 0; k < steps && !l.broken; k++)
        {
            scope_t* iteration = new_scope(c, inner, s->spec);
            value_t i = v_number(from + step * k);
            bind(c, iteration, name, &i, true);
            l.continued = false;
            loop_body(c, iteration, n->d, &l);
        }
        return 0;
    }
    if (n->a != NULL)
    {
        if (n->a->kind == DMVS_JS_VAR)
            declarations(c, inner, n->a);
        else
            (void)expression(c, inner, n->a);
        release_temps(c);
    }
    loop_t l;
    memset(&l, 0, sizeof(l));
    l.runtime = true;
    l.update = n->c;
    emit_op(c, DMVSI_ACT_LOOP, 0, 0, 0, NULL);
    c->code->blocks++;
    c->code->loops++;
    if (n->b != NULL)
    {
        test_t t = condition(c, inner, n->b);
        c->at = n;
        if (t.kind == 0 && !t.value)
            emit_op(c, DMVSI_ACT_BREAK, 0, 0, 0, NULL);
        else if (t.kind != 0)
        {
            emit_op(c, t.kind, t.var, t.operand, t.imm, NULL);
            emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
            emit_op(c, DMVSI_ACT_BREAK, 0, 0, 0, NULL);
            emit_end(c);
        }
        release_temps(c);
    }
    loop_body(c, inner, n->d, &l);
    if (n->c != NULL)
    {
        (void)expression(c, inner, n->c);
        release_temps(c);
    }
    c->code->loops--;
    c->code->blocks--;
    return emit_end(c);
}

static int while_statement(compiler_t* c, scope_t* s, const node_t* n, bool test_first)
{
    loop_t l;
    memset(&l, 0, sizeof(l));
    l.runtime = true;
    const node_t* test = test_first ? n->a : n->b;
    const node_t* body = test_first ? n->b : n->a;
    emit_op(c, DMVSI_ACT_LOOP, 0, 0, 0, NULL);
    c->code->blocks++;
    c->code->loops++;
    if (!test_first)
        loop_body(c, s, body, &l);
    test_t t = condition(c, s, test);
    c->at = n;
    if (t.kind == 0 && !t.value)
        emit_op(c, DMVSI_ACT_BREAK, 0, 0, 0, NULL);
    else if (t.kind != 0)
    {
        emit_op(c, t.kind, t.var, t.operand, t.imm, NULL);
        emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
        emit_op(c, DMVSI_ACT_BREAK, 0, 0, 0, NULL);
        emit_end(c);
    }
    release_temps(c);
    if (test_first)
        loop_body(c, s, body, &l);
    c->code->loops--;
    c->code->blocks--;
    return emit_end(c);
}

static int for_of(compiler_t* c, scope_t* s, const node_t* n)
{
    value_t list = expression(c, s, n->b);
    c->at = n;
    const object_t* a = as_object(&list, O_ARRAY);
    const object_t* o = as_object(&list, O_OBJECT);
    const node_t* target = (n->a->kind == DMVS_JS_VAR) ? n->a->a->a : n->a;
    if (target->kind != DMVS_JS_IDENT || (n->kind == DMVS_JS_FOR_OF && a == NULL) || (n->kind == DMVS_JS_FOR_IN && o == NULL && a == NULL))
    {
        report(c, "a for over what is known only when the view runs - not converted");
        return 0;
    }
    loop_t l;
    memset(&l, 0, sizeof(l));
    uint32_t count = (a != NULL) ? a->count : o->count;
    for (uint32_t k = 0; k < count && !l.broken; k++)
    {
        scope_t* iteration = new_scope(c, s, s->spec);
        char key[16];
        value_t v;
        if (n->kind == DMVS_JS_FOR_OF)
            v = a->values[k];
        else if (o != NULL)
            v = v_string(c, o->keys[k], strlen(o->keys[k]));
        else
        {
            Dmod_SnPrintf(key, sizeof(key), "%u", (unsigned)k);
            v = v_string(c, key, strlen(key));
        }
        bind(c, iteration, target->text, &v, true);
        l.continued = false;
        loop_body(c, iteration, (n->kind == DMVS_JS_FOR_OF || n->kind == DMVS_JS_FOR_IN) ? n->c : n->d, &l);
    }
    return 0;
}

static int switch_statement(compiler_t* c, scope_t* s, const node_t* n)
{
    value_t d = expression(c, s, n->a);
    c->at = n;
    scope_t* inner = new_scope(c, s, s->spec);
    if (is_static(&d))
    {
        /* The case it is (and what falls through), up to a break */
        const node_t* start = NULL;
        for (const node_t* k = n->b; k != NULL && start == NULL; k = k->next)
        {
            if (k->a == NULL)
                continue;
            value_t v = expression(c, inner, k->a);
            if (is_static(&v) && v.kind == d.kind && same_static(&v, &d))
                start = k;
        }
        for (const node_t* k = n->b; k != NULL && start == NULL; k = k->next)
            if (k->a == NULL)
                start = k;
        loop_t l;
        memset(&l, 0, sizeof(l));
        push_loop(c, &l);
        for (const node_t* k = start; k != NULL && !l.broken; k = k->next)
            statements(c, inner, k->b);
        pop_loop(c);
        return 0;
    }
    /* Runtime: a chain of IFs - each case ending with break */
    uint32_t opened = 0;
    const node_t* fallback = NULL;
    for (const node_t* k = n->b; k != NULL; k = k->next)
    {
        if (k->a == NULL)
        {
            fallback = k;
            continue;
        }
        value_t v = expression(c, inner, k->a);
        test_t t = compare(c, DMVS_JS_OP_SEQ, &d, &v);
        if (t.kind == 0)
            continue;
        if (opened > 0)
            emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
        emit_op(c, t.kind, t.var, t.operand, t.imm, NULL);
        opened++;
        c->code->blocks++;
        loop_t l;
        memset(&l, 0, sizeof(l));
        push_loop(c, &l);
        statements(c, inner, k->b);
        pop_loop(c);
        if (!l.broken)
            report(c, "a case that falls through - not converted");
    }
    if (fallback != NULL)
    {
        if (opened > 0)
            emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
        loop_t l;
        memset(&l, 0, sizeof(l));
        push_loop(c, &l);
        statements(c, inner, fallback->b);
        pop_loop(c);
    }
    for (uint32_t i = 0; i < opened; i++)
    {
        c->code->blocks--;
        emit_end(c);
    }
    return 0;
}

static int statement(compiler_t* c, scope_t* s, const node_t* n)
{
    if (skipping(c) || c->failed)
        return 0;
    c->at = n;
    int ret = 0;
    switch (n->kind)
    {
        case DMVS_JS_EMPTY:
            break;
        case DMVS_JS_VAR:
            ret = declarations(c, s, n);
            break;
        case DMVS_JS_FUNCTION:
            break;                                  /* Hoisted */
        case DMVS_JS_EXPRESSION:
            (void)expression(c, s, n->a);
            break;
        case DMVS_JS_BLOCK:
            ret = block(c, s, n);
            break;
        case DMVS_JS_IF:
            ret = if_statement(c, s, n);
            break;
        case DMVS_JS_RETURN:
            return_value(c, s, n);
            break;
        case DMVS_JS_FOR:
            ret = for_statement(c, s, n);
            break;
        case DMVS_JS_WHILE:
            ret = while_statement(c, s, n, true);
            break;
        case DMVS_JS_DO_WHILE:
            ret = while_statement(c, s, n, false);
            break;
        case DMVS_JS_FOR_OF:
        case DMVS_JS_FOR_IN:
            ret = for_of(c, s, n);
            break;
        case DMVS_JS_SWITCH:
            ret = switch_statement(c, s, n);
            break;
        case DMVS_JS_BREAK:
        case DMVS_JS_CONTINUE:
        {
            loop_t* l = current_loop(c);
            if (n->text != NULL || l == NULL)
            {
                report(c, "a break / continue to a label, or outside a loop - not converted");
                break;
            }
            if (l->runtime)
            {
                if (n->kind == DMVS_JS_CONTINUE && l->update != NULL)
                    (void)expression(c, s, l->update);
                emit_op(c, (n->kind == DMVS_JS_BREAK) ? DMVSI_ACT_BREAK : DMVSI_ACT_CONTINUE, 0, 0, 0, NULL);
            }
            else if (c->code->blocks > l->blocks)
            {
                if (!l->reported)
                    report(c, "a break / continue under a runtime condition, in a for-of unrolled - not converted");
                l->reported = true;
            }
            else if (n->kind == DMVS_JS_BREAK)
                l->broken = true;
            else
                l->continued = true;
            break;
        }
        case DMVS_JS_TRY:
            report(c, "try - only its block is compiled");
            ret = block(c, s, n->a);
            break;
        default:
            report(c, "a statement the compiler does not take (class, throw, labels) - not converted");
            break;
    }
    release_temps(c);
    return ret;
}

static int statements(compiler_t* c, scope_t* s, const node_t* first)
{
    hoist(c, s, first);
    for (const node_t* n = first; n != NULL && !c->failed; n = n->next)
        (void)statement(c, s, n);
    return c->failed ? -ENOMEM : 0;
}

/* ---- The API ---- */

dmod_dmvs_js_api_declaration(1.0, dmvs_js_compiler_t, _compiler_new, ( dmvsi_doc_t doc, const dmvs_js_host_t* host ))
{
    if (doc == NULL)
        return NULL;
    compiler_t* c = Dmod_Malloc(sizeof(*c));
    if (c == NULL)
        return NULL;
    memset(c, 0, sizeof(*c));
    c->doc = doc;
    if (host != NULL)
        c->host = *host;
    c->global = new_scope(c, NULL, NULL);
    c->code = &c->init;
    if (c->failed)
    {
        arena_release(&c->arena);
        Dmod_Free(c);
        return NULL;
    }
    return (dmvs_js_compiler_t)c;
}

/* A tree into the compiler's keeping, the pre-scan run again with it (NULL: not taken) */
static const node_t* take(compiler_t* c, dmvs_js_ast_t ast)
{
    const node_t* root = dmvs_js_root(ast);
    if (root == NULL)
        return NULL;
    program_t** end = &c->programs;
    for (; *end != NULL; end = &(*end)->next)
    {
        if ((*end)->ast == ast)
            return root;                            /* Seen (scanned) before */
    }
    program_t* k = arena_alloc(&c->arena, sizeof(program_t));
    if (k == NULL)
    {
        c->failed = true;
        return NULL;
    }
    k->ast = ast;
    k->root = root;
    *end = k;
    prescan(c);
    return root;
}

dmod_dmvs_js_api_declaration(1.0, int, _scan, ( dmvs_js_compiler_t compiler, dmvs_js_ast_t code ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL || code == NULL)
        return -EINVAL;
    return (take(c, code) != NULL) ? 0 : -ENOMEM;
}

dmod_dmvs_js_api_declaration(1.0, int, _compile, ( dmvs_js_compiler_t compiler, dmvs_js_ast_t script ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL || script == NULL)
        return -EINVAL;
    const node_t* program = take(c, script);
    if (program == NULL)
        return -ENOMEM;
    c->code = &c->init;
    c->loop_depth = 0;
    (void)statements(c, c->global, program->a);
    return c->failed ? -ENOMEM : 0;
}

dmod_dmvs_js_api_declaration(1.0, dmvsi_handler_t, _compile_handler, ( dmvs_js_compiler_t compiler, dmvs_js_ast_t code, uint32_t this_object ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL || code == NULL)
        return 0;
    const node_t* program = take(c, code);
    if (program == NULL)
        return 0;
    /* As the body of a function: `this` its object */
    object_t* f = new_object(c, O_FUNCTION);
    node_t* fn = arena_alloc(&c->arena, sizeof(node_t));
    node_t* body = arena_alloc(&c->arena, sizeof(node_t));
    if (f == NULL || fn == NULL || body == NULL)
    {
        c->failed = true;
        return 0;
    }
    body->kind = DMVS_JS_BLOCK;
    body->a = program->a;
    fn->kind = DMVS_JS_FUNCTION;
    fn->b = body;
    fn->text = "onclick";
    fn->line = program->line;
    fn->column = program->column;
    f->node = fn;
    f->env = c->global;
    value_t self = v_undefined();
    if (this_object != 0)
    {
        self.kind = DMVS_JS_V_OBJECT;
        self.object = this_object;
    }
    value_t fv = v_internal(f);
    code_t* outer = c->code;
    dmvsi_handler_t h = function_handler(c, &fv, &self);
    c->code = outer;
    return h;
}

dmod_dmvs_js_api_declaration(1.0, int, _finish, ( dmvs_js_compiler_t compiler ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL)
        return -EINVAL;
    c->code = &c->init;
    flush_host(c);
    int ret = finish_timers(c);
    if (ret == 0 && c->init.count > 0)
    {
        dmvsi_handler_t h = dmvsi_add_handler(c->doc, c->init.actions, c->init.count);
        if (h == 0 || dmvsi_set_init(c->doc, h) != 0)
        {
            report(c, "the init handler - the document does not take it");
            ret = -EINVAL;
        }
    }
    return (c->failed) ? -ENOMEM : ret;
}

dmod_dmvs_js_api_declaration(1.0, void, _compiler_free, ( dmvs_js_compiler_t compiler ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL)
        return;
    for (const program_t* k = c->programs; k != NULL; k = k->next)
        dmvs_js_free(k->ast);
    arena_release(&c->arena);
    Dmod_Free(c);
}

dmod_dmvs_js_api_declaration(1.0, uint32_t, _reports, ( dmvs_js_compiler_t compiler ))
{
    return (compiler != NULL) ? ((compiler_t*)compiler)->reports : 0u;
}

dmod_dmvs_js_api_declaration(1.0, int, _emit, ( dmvs_js_compiler_t compiler, const dmvsi_action_t* action ))
{
    return (compiler != NULL && action != NULL) ? emit((compiler_t*)compiler, action) : -EINVAL;
}

dmod_dmvs_js_api_declaration(1.0, int, _number_operand, ( dmvs_js_compiler_t compiler, const dmvs_js_value_t* v, uint8_t scale, dmvsi_var_t* var, int32_t* value ))
{
    if (compiler == NULL || v == NULL || var == NULL || value == NULL)
        return -EINVAL;
    return number_operand((compiler_t*)compiler, v, scale, var, value) ? 0 : -EINVAL;
}

dmod_dmvs_js_api_declaration(1.0, int, _text_operand, ( dmvs_js_compiler_t compiler, const dmvs_js_value_t* v, dmvsi_var_t* var, const char** text ))
{
    if (compiler == NULL || v == NULL || var == NULL || text == NULL)
        return -EINVAL;
    return text_operand((compiler_t*)compiler, v, var, text) ? 0 : -EINVAL;
}

dmod_dmvs_js_api_declaration(1.0, bool, _truthy, ( const dmvs_js_value_t* v ))
{
    return v != NULL && is_static(v) && truthy(v);
}

dmod_dmvs_js_api_declaration(1.0, dmvsi_handler_t, _function_handler, ( dmvs_js_compiler_t compiler, const dmvs_js_value_t* function, uint32_t this_object ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL || function == NULL)
        return 0;
    value_t self = v_undefined();
    if (this_object != 0)
    {
        self.kind = DMVS_JS_V_OBJECT;
        self.object = this_object;
    }
    return function_handler(c, function, &self);
}

dmod_dmvs_js_api_declaration(1.0, bool, _parse_number, ( const char* text, size_t length, double* number ))
{
    return text != NULL && number != NULL && parse_number(text, length, number);
}

dmod_dmvs_js_api_declaration(1.0, void, _report, ( dmvs_js_compiler_t compiler, const char* message ))
{
    if (compiler != NULL && message != NULL)
        report((compiler_t*)compiler, message);
}

dmod_dmvs_js_api_declaration(1.0, uint32_t, _object_domain, ( dmvs_js_compiler_t compiler, dmvsi_var_t var, uint32_t* objects, uint32_t max ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL || var == 0)
        return 0;
    /* The variables it is set from, and theirs: their objects */
    dmvsi_var_t seen[64];
    uint32_t seen_count = 0, done = 0, count = 0;
    seen[seen_count++] = var;
    while (done < seen_count)
    {
        dmvsi_var_t v = seen[done++];
        for (const holds_t* h = c->holds; h != NULL; h = h->next)
        {
            if (h->var != v)
                continue;
            if (h->from != 0)
            {
                bool known = false;
                for (uint32_t i = 0; i < seen_count && !known; i++)
                    known = seen[i] == h->from;
                if (!known && seen_count < sizeof(seen) / sizeof(seen[0]))
                    seen[seen_count++] = h->from;
                continue;
            }
            bool known = false;
            for (uint32_t i = 0; i < count && i < max && !known; i++)
                known = objects[i] == h->object;
            if (known)
                continue;
            if (count < max && objects != NULL)
                objects[count] = h->object;
            count++;
        }
    }
    return count;
}

dmod_dmvs_js_api_declaration(1.0, int, _array, ( dmvs_js_compiler_t compiler, const dmvs_js_value_t* values, uint32_t count, dmvs_js_value_t* array ))
{
    compiler_t* c = (compiler_t*)compiler;
    if (c == NULL || array == NULL || (values == NULL && count > 0))
        return -EINVAL;
    object_t* a = new_object(c, O_ARRAY);
    if (a == NULL)
        return -ENOMEM;
    a->values = arena_alloc(&c->arena, (count + 1U) * sizeof(value_t));
    if (a->values == NULL)
    {
        c->failed = true;
        return -ENOMEM;
    }
    if (count > 0)
        memcpy(a->values, values, count * sizeof(value_t));
    a->count = count;
    *array = v_internal(a);
    return 0;
}

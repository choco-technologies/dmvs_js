#include "compiler.h"

/*
 * What the language has: Math, parseFloat / parseInt, Number / String,
 * the methods of numbers, strings and arrays, setTimeout / setInterval,
 * console. Static values are computed; runtime ones become actions.
 */

static value_t builtin(compiler_t* c, uint32_t id, const value_t* self, const char* method)
{
    object_t* o = new_object(c, O_BUILTIN);
    if (o == NULL)
        return v_undefined();
    o->builtin = id;
    o->self = (self != NULL) ? *self : v_undefined();
    o->method = method;
    return v_internal(o);
}

static bool name_is(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

bool builtin_global(compiler_t* c, const char* name, value_t* out)
{
    static const struct { char name[16]; uint32_t id; } globals[] = {
        { "Math", B_MATH }, { "parseFloat", B_PARSE_FLOAT }, { "parseInt", B_PARSE_INT },
        { "Number", B_NUMBER }, { "String", B_STRING }, { "setTimeout", B_SET_TIMEOUT },
        { "setInterval", B_SET_INTERVAL }, { "clearTimeout", B_CLEAR_TIMER }, { "clearInterval", B_CLEAR_TIMER },
        { "console", B_CONSOLE },
    };
    for (size_t i = 0; i < sizeof(globals) / sizeof(globals[0]); i++)
    {
        if (name_is(globals[i].name, name))
        {
            *out = builtin(c, globals[i].id, NULL, NULL);
            return true;
        }
    }
    if (name_is(name, "Infinity"))
    {
        *out = v_number(1e300 * 1e300);
        return true;
    }
    return false;
}

static bool is_number(const value_t* v)
{
    return v->kind == DMVS_JS_V_NUMBER || (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_NUMBER);
}

static bool is_text_value(const value_t* v)
{
    return v->kind == DMVS_JS_V_STRING || (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_TEXT);
}

bool builtin_member(compiler_t* c, const value_t* object, const char* name, value_t* out)
{
    if (is_unknown(object))
    {
        *out = v_unknown();
        return true;
    }
    const object_t* b = as_object(object, O_BUILTIN);
    if (b != NULL && b->builtin == B_MATH)
    {
        static const struct { char name[8]; uint32_t id; } math[] = {
            { "floor", B_MATH_FLOOR }, { "round", B_MATH_ROUND }, { "ceil", B_MATH_CEIL }, { "trunc", B_MATH_TRUNC },
            { "min", B_MATH_MIN }, { "max", B_MATH_MAX }, { "abs", B_MATH_ABS },
        };
        if (name_is(name, "PI"))
        {
            *out = v_number(3.14159265358979323846);
            return true;
        }
        for (size_t i = 0; i < sizeof(math) / sizeof(math[0]); i++)
        {
            if (name_is(math[i].name, name))
            {
                *out = builtin(c, math[i].id, NULL, NULL);
                return true;
            }
        }
        return false;
    }
    if (b != NULL && b->builtin == B_CONSOLE)
    {
        *out = builtin(c, B_NOOP, NULL, NULL);      /* log, warn, error, ...: nothing in a view */
        return true;
    }
    if (name_is(name, "toString"))
    {
        *out = builtin(c, B_TO_STRING, object, NULL);
        return true;
    }
    if (is_number(object) && name_is(name, "toFixed"))
    {
        *out = builtin(c, B_TO_FIXED, object, NULL);
        return true;
    }
    if (is_text_value(object) && name_is(name, "padStart"))
    {
        *out = builtin(c, B_PAD_START, object, NULL);
        return true;
    }
    if (object->kind == DMVS_JS_V_STRING)
    {
        static const char methods[][12] = {
            "toUpperCase", "toLowerCase", "trim", "includes", "startsWith", "endsWith", "indexOf", "slice",
            "substring", "split", "padEnd", "repeat", "charAt",
        };
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        {
            if (name_is(methods[i], name))
            {
                *out = builtin(c, B_STRING_METHOD, object, methods[i]);
                return true;
            }
        }
        return false;
    }
    if (as_object(object, O_ARRAY) != NULL)
    {
        if (name_is(name, "forEach"))
        {
            *out = builtin(c, B_FOR_EACH, object, NULL);
            return true;
        }
        static const char methods[][10] = { "join", "includes", "indexOf", "map", "filter", "find", "some", "every" };
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        {
            if (name_is(methods[i], name))
            {
                *out = builtin(c, B_ARRAY_METHOD, object, methods[i]);
                return true;
            }
        }
    }
    return false;
}

/* ---- Numbers ---- */

static double arg_number(const value_t* args, uint32_t count, uint32_t i, double otherwise)
{
    double n;
    return (i < count && static_number(&args[i], &n)) ? n : otherwise;
}

static double floor_of(double x)
{
    double t = (x < 0) ? -(double)(int64_t)(-x) : (double)(int64_t)x;
    return (t > x) ? t - 1.0 : t;
}

/* Math.floor / round / ceil / trunc of a runtime number: an integer */
static value_t rounding(compiler_t* c, uint32_t id, const value_t* v)
{
    if (v->kind != DMVS_JS_V_RUNTIME || v->type == DMVS_JS_T_TEXT)
    {
        report(c, "rounding what is not a number - not converted");
        return v_unknown();
    }
    if (number_scale(v) == 0)
        return *v;
    value_t r = number_copy(c, v, FIX);
    if (id == B_MATH_ROUND)
        emit_op(c, DMVSI_ACT_ADD, r.var, 0, 500, NULL);
    dmvsi_var_t m = temp(c, DMVS_JS_T_NUMBER);
    emit_op(c, DMVSI_ACT_SET, m, r.var, 0, NULL);
    emit_op(c, DMVSI_ACT_MOD, m, 0, 1000, NULL);
    emit_op(c, DMVSI_ACT_DIV, r.var, 0, 1000, NULL);   /* Towards zero */
    if (id == B_MATH_FLOOR || id == B_MATH_ROUND)
    {
        emit_op(c, DMVSI_ACT_IF_LT, m, 0, 0, NULL);
        emit_op(c, DMVSI_ACT_SUB, r.var, 0, 1, NULL);
        emit_end(c);
    }
    else if (id == B_MATH_CEIL)
    {
        emit_op(c, DMVSI_ACT_IF_GT, m, 0, 0, NULL);
        emit_op(c, DMVSI_ACT_ADD, r.var, 0, 1, NULL);
        emit_end(c);
    }
    r.scale = 0;
    return r;
}

static value_t min_max(compiler_t* c, bool max, const value_t* args, uint32_t count)
{
    bool all_static = true;
    uint8_t scale = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        double n;
        all_static = all_static && is_static(&args[i]);
        if (static_number(&args[i], &n) ? n != (double)(int64_t)n : number_scale(&args[i]) == FIX)
            scale = FIX;
    }
    if (all_static)
    {
        double r = max ? -(1e300 * 1e300) : (1e300 * 1e300);
        for (uint32_t i = 0; i < count; i++)
        {
            double n = arg_number(args, count, i, 0.0 / 0.0);
            if (n != n)
                return v_number(n);
            r = (max ? n > r : n < r) ? n : r;
        }
        return v_number(r);
    }
    if (count == 0)
        return v_number(0.0);
    value_t r = number_copy(c, &args[0], scale);
    for (uint32_t i = 1; i < count; i++)
    {
        dmvsi_var_t var;
        int32_t imm;
        if (number_operand(c, &args[i], scale, &var, &imm))
            emit_op(c, max ? DMVSI_ACT_MAX : DMVSI_ACT_MIN, r.var, var, imm, NULL);
    }
    return r;
}

/* parseFloat / Number: a number from a text - a runtime text knows it when it was made of one */
static value_t to_number(compiler_t* c, const value_t* v, bool integer)
{
    double n;
    if (v->kind == DMVS_JS_V_STRING)
    {
        if (!parse_number(v->text, v->length, &n))
            return v_number(0.0 / 0.0);
        return v_number(integer ? ((n < 0) ? -(double)(int64_t)(-n) : (double)(int64_t)n) : n);
    }
    if (static_number(v, &n))
        return v_number(integer ? ((n < 0) ? -(double)(int64_t)(-n) : (double)(int64_t)n) : n);
    if (v->kind == DMVS_JS_V_RUNTIME && v->type != DMVS_JS_T_TEXT)
        return integer ? rounding(c, B_MATH_TRUNC, v) : *v;
    if (v->kind == DMVS_JS_V_RUNTIME && v->number_var != 0)
    {
        value_t f = v_runtime(DMVS_JS_T_NUMBER, FIX, v->number_var);
        f = number_copy(c, &f, FIX);
        return integer ? rounding(c, B_MATH_TRUNC, &f) : f;
    }
    report(c, "a number read from a text known only when the view runs - not converted");
    return v_unknown();
}

/* n.toFixed(d) of a static n */
static value_t static_fixed(compiler_t* c, double n, uint32_t decimals)
{
    char out[48];
    size_t k = 0;
    if (n != n)
        return v_string(c, "NaN", 3);
    bool negative = n < 0;
    double scale = 1.0;
    for (uint32_t i = 0; i < decimals; i++)
        scale *= 10.0;
    double a = (negative ? -n : n) * scale + 0.5;
    if (a > 9e18)
        return v_string(c, "Infinity", 8);
    uint64_t r = (uint64_t)a;
    char digits[24];
    size_t d = 0;
    do
    {
        digits[d++] = (char)('0' + (int)(r % 10u));
        r /= 10u;
    } while (r > 0 || d <= decimals);
    if (negative && !(d == decimals + 1U && digits[0] == '0' && (uint64_t)a == 0))
        out[k++] = '-';
    while (d > 0)
    {
        out[k++] = digits[--d];
        if (d == decimals && decimals > 0)
            out[k++] = '.';
    }
    return v_string(c, out, k);
}

static value_t to_fixed(compiler_t* c, const value_t* self, const value_t* args, uint32_t count)
{
    double d = arg_number(args, count, 0, 0.0);
    if ((count > 0 && !is_static(&args[0])) || d < 0 || d > 20)
    {
        report(c, "toFixed() of digits known only when the view runs - not converted");
        return v_string(c, "", 0);
    }
    double n;
    if (static_number(self, &n))
        return static_fixed(c, n, (uint32_t)d);
    value_t out = v_runtime(DMVS_JS_T_TEXT, 0, temp(c, DMVS_JS_T_TEXT));
    if (fixed_text(c, out.var, self, (uint32_t)d, false) == 0)
        out.number_var = number_copy(c, self, FIX).var;
    return out;
}

/* s.padStart(n, fill): a runtime text of a number - the number formatted at that width */
static value_t pad_start(compiler_t* c, const value_t* self, const value_t* args, uint32_t count)
{
    double width = arg_number(args, count, 0, 0.0);
    const char* fill = " ";
    if (count > 1)
    {
        if (args[1].kind != DMVS_JS_V_STRING)
        {
            report(c, "padStart() with a fill known only when the view runs - not converted");
            return *self;
        }
        fill = args[1].text;
    }
    if (self->kind == DMVS_JS_V_STRING)
    {
        size_t fill_length = strlen(fill);
        if (width <= (double)self->length || fill_length == 0 || width > 1024)
            return *self;
        size_t total = (size_t)width, pad = total - self->length;
        char* out = arena_alloc(&c->arena, total + 1U);
        if (out == NULL)
        {
            c->failed = true;
            return *self;
        }
        for (size_t i = 0; i < pad; i++)
            out[i] = fill[i % fill_length];
        memcpy(out + pad, self->text, self->length);
        return v_string(c, out, total);
    }
    bool zero = strcmp(fill, "0") == 0, space = strcmp(fill, " ") == 0;
    if (self->number_var == 0 || !(zero || space) || width < 1 || width > 9)
    {
        report(c, "padStart() of a runtime text that is not a number - not converted");
        return *self;
    }
    value_t n = v_runtime(DMVS_JS_T_NUMBER, FIX, self->number_var);
    value_t i = rounding(c, B_MATH_TRUNC, &n);
    char format[8];
    Dmod_SnPrintf(format, sizeof(format), zero ? "%%0%ud" : "%%%ud", (unsigned)width);
    value_t out = v_runtime(DMVS_JS_T_TEXT, 0, temp(c, DMVS_JS_T_TEXT));
    emit_op(c, DMVSI_ACT_FORMAT, out.var, i.var, 0, format);
    out.number_var = self->number_var;
    return out;
}

/* ---- Static strings ---- */

static bool static_text(const value_t* args, uint32_t count, uint32_t i, const char** text, size_t* length)
{
    if (i >= count || args[i].kind != DMVS_JS_V_STRING)
        return false;
    *text = args[i].text;
    *length = args[i].length;
    return true;
}

static int64_t find(const char* s, size_t n, const char* p, size_t m, size_t from)
{
    for (size_t i = from; i + m <= n; i++)
    {
        if (memcmp(s + i, p, m) == 0)
            return (int64_t)i;
    }
    return -1;
}

/* A position of slice / substring: negative from the end (slice), clamped */
static size_t position(double p, size_t length, bool from_end)
{
    if (p != p)
        p = 0;
    if (p < 0)
        p = from_end ? (double)length + p : 0;
    if (p < 0)
        p = 0;
    return (p > (double)length) ? length : (size_t)p;
}

static value_t string_method(compiler_t* c, const value_t* self, const char* m, const value_t* args, uint32_t count)
{
    const char* s = self->text;
    size_t n = self->length;
    const char* p = NULL;
    size_t pn = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        if (!is_static(&args[i]))
        {
            report(c, "a method of a text with arguments known only when the view runs - not converted");
            return v_unknown();
        }
    }
    if (name_is(m, "toUpperCase") || name_is(m, "toLowerCase"))
    {
        char* out = arena_alloc(&c->arena, n + 1U);
        if (out == NULL)
        {
            c->failed = true;
            return *self;
        }
        bool upper = m[2] == 'U';
        for (size_t i = 0; i < n; i++)
        {
            char ch = s[i];
            if (upper && ch >= 'a' && ch <= 'z')
                ch = (char)(ch - 'a' + 'A');
            else if (!upper && ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
            out[i] = ch;
        }
        return v_string(c, out, n);
    }
    if (name_is(m, "trim"))
    {
        size_t a = 0, b = n;
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r'))
            a++;
        while (b > a && (s[b - 1U] == ' ' || s[b - 1U] == '\t' || s[b - 1U] == '\n' || s[b - 1U] == '\r'))
            b--;
        return v_string(c, s + a, b - a);
    }
    if (name_is(m, "includes") || name_is(m, "indexOf") || name_is(m, "startsWith") || name_is(m, "endsWith"))
    {
        if (!static_text(args, count, 0, &p, &pn))
            return v_bool(false);
        if (name_is(m, "startsWith"))
            return v_bool(pn <= n && memcmp(s, p, pn) == 0);
        if (name_is(m, "endsWith"))
            return v_bool(pn <= n && memcmp(s + n - pn, p, pn) == 0);
        int64_t at = find(s, n, p, pn, position(arg_number(args, count, 1, 0), n, false));
        return name_is(m, "includes") ? v_bool(at >= 0) : v_number((double)at);
    }
    if (name_is(m, "slice") || name_is(m, "substring"))
    {
        bool slice = m[1] == 'l';
        size_t a = position(arg_number(args, count, 0, 0), n, slice);
        size_t b = position(arg_number(args, count, 1, (double)n), n, slice);
        if (!slice && a > b)
        {
            size_t t = a;
            a = b;
            b = t;
        }
        return v_string(c, s + a, (b > a) ? b - a : 0);
    }
    if (name_is(m, "charAt"))
    {
        size_t a = position(arg_number(args, count, 0, 0), n, false);
        return v_string(c, s + a, (a < n) ? 1 : 0);
    }
    if (name_is(m, "repeat") || name_is(m, "padEnd"))
    {
        double times = arg_number(args, count, 0, 0);
        size_t total = name_is(m, "repeat") ? (size_t)times * n : (size_t)times;
        if (times < 0 || total > 4096)
            return *self;
        if (total <= n && !name_is(m, "repeat"))
            return *self;
        const char* fill = s;
        size_t fn = n, start = 0;
        if (name_is(m, "padEnd"))
        {
            fill = " ";
            fn = 1;
            if (static_text(args, count, 1, &p, &pn) && pn > 0)
            {
                fill = p;
                fn = pn;
            }
            start = n;
        }
        char* out = arena_alloc(&c->arena, total + 1U);
        if (out == NULL)
        {
            c->failed = true;
            return *self;
        }
        memcpy(out, s, start);
        for (size_t i = start; i < total; i++)
            out[i] = fill[(i - start) % fn];
        return v_string(c, out, total);
    }
    if (name_is(m, "split"))
    {
        object_t* a = new_object(c, O_ARRAY);
        if (a == NULL)
            return v_undefined();
        bool chars = static_text(args, count, 0, &p, &pn) && pn == 0;
        if (!chars && !static_text(args, count, 0, &p, &pn))
        {
            a->values = arena_alloc(&c->arena, sizeof(value_t));
            if (a->values != NULL)
                a->values[a->count++] = *self;
            return v_internal(a);
        }
        uint32_t parts = 1;
        for (size_t i = 0; !chars && i + pn <= n; i++)
            if (memcmp(s + i, p, pn) == 0)
            {
                parts++;
                i += pn - 1U;
            }
        if (chars)
            parts = (uint32_t)n;
        a->values = arena_alloc(&c->arena, (parts + 1U) * sizeof(value_t));
        if (a->values == NULL)
        {
            c->failed = true;
            return v_undefined();
        }
        size_t from = 0;
        for (size_t i = 0; i < n || (!chars && from <= n); )
        {
            if (chars)
            {
                a->values[a->count++] = v_string(c, s + i, 1);
                i++;
                continue;
            }
            int64_t at = find(s, n, p, pn, from);
            size_t end = (at < 0) ? n : (size_t)at;
            a->values[a->count++] = v_string(c, s + from, end - from);
            if (at < 0)
                break;
            from = end + pn;
            i = from;
        }
        return v_internal(a);
    }
    return v_undefined();
}

/* ---- Arrays ---- */

static value_t array_method(compiler_t* c, const object_t* a, const char* m, const value_t* args, uint32_t count)
{
    if (name_is(m, "join"))
    {
        const char* sep = ",";
        size_t sn = 1;
        if (count > 0 && args[0].kind == DMVS_JS_V_STRING)
        {
            sep = args[0].text;
            sn = args[0].length;
        }
        value_t* parts = arena_alloc(&c->arena, (2U * a->count + 1U) * sizeof(value_t));
        if (parts == NULL)
        {
            c->failed = true;
            return v_string(c, "", 0);
        }
        uint32_t k = 0;
        for (uint32_t i = 0; i < a->count; i++)
        {
            if (i > 0)
                parts[k++] = v_string(c, sep, sn);
            parts[k++] = a->values[i];
        }
        return concat(c, parts, k);
    }
    if (name_is(m, "includes") || name_is(m, "indexOf"))
    {
        if (count == 0 || !is_static(&args[0]))
        {
            report(c, "looking for a value known only when the view runs - not converted");
            return v_bool(false);
        }
        int64_t at = -1;
        for (uint32_t i = 0; i < a->count && at < 0; i++)
        {
            if (a->values[i].kind == args[0].kind && same_static(&a->values[i], &args[0]))
                at = i;
        }
        return name_is(m, "includes") ? v_bool(at >= 0) : v_number((double)at);
    }
    if (count == 0)
        return v_undefined();
    /* With a callback: called for every element, its result static to be used */
    object_t* out = new_object(c, O_ARRAY);
    if (out == NULL)
        return v_undefined();
    out->values = arena_alloc(&c->arena, (a->count + 1U) * sizeof(value_t));
    if (out->values == NULL)
    {
        c->failed = true;
        return v_undefined();
    }
    bool map = name_is(m, "map");
    for (uint32_t i = 0; i < a->count; i++)
    {
        value_t cb_args[3] = { a->values[i], v_number((double)i), v_internal(a) };
        value_t undefined = v_undefined();
        value_t r = call_value(c, &args[0], &undefined, cb_args, 3);
        if (map)
        {
            if (r.kind == DMVS_JS_V_RUNTIME)
            {
                /* In a variable of its own: a temporary is reused */
                value_t var = v_runtime(r.type, r.scale, new_var(c, "item", r.type, 0, ""));
                assign_to(c, &var, &r);
                r = var;
            }
            out->values[out->count++] = r;
            continue;
        }
        if (!is_static(&r))
        {
            report(c, "filter / find / some / every with a test known only when the view runs - not converted");
            return v_unknown();
        }
        bool yes = truthy(&r);
        if (name_is(m, "filter") && yes)
            out->values[out->count++] = a->values[i];
        else if (name_is(m, "find") && yes)
            return a->values[i];
        else if (name_is(m, "some") && yes)
            return v_bool(true);
        else if (name_is(m, "every") && !yes)
            return v_bool(false);
    }
    if (name_is(m, "find"))
        return v_undefined();
    if (name_is(m, "some") || name_is(m, "every"))
        return v_bool(name_is(m, "every"));
    return v_internal(out);
}

/* ---- Timers ---- */

static value_t start_timer(compiler_t* c, bool repeat, const value_t* args, uint32_t count)
{
    if (c->evaluate)
        return v_number((double)++c->timers);       /* Evaluating the loading: what it starts does not run */
    double ms = arg_number(args, count, 1, 0.0);
    if (count == 0 || (count > 1 && !is_static(&args[1])))
    {
        report(c, "a timer of a time known only when the view runs - not converted");
        return v_unknown();
    }
    if (count > 2)
        report(c, "arguments of a timer's function - not given to it");
    if (ms < 0)
        ms = 0;
    if (ms > 2000000000.0)
        ms = 2000000000.0;
    /* Its site first (its number is what the function may stop), then the function */
    if (c->site_count >= MAX_SITES)
    {
        report(c, "too many timers - not converted");
        return v_unknown();
    }
    uint32_t k = c->site_count++;
    site_t* site = &c->sites[k];
    char name[24];
    site->node = c->at;
    site->spec = (c->code != NULL) ? c->code->spec : NULL;
    site->ms = (uint32_t)ms;
    site->repeat = repeat;
    code_t* code = c->code;
    const node_t* at = c->at;
    c->code = NULL;                                 /* Names of the document's own, not the function's */
    Dmod_SnPrintf(name, sizeof(name), "timer%u_on", (unsigned)(k + 1U));
    site->active = new_var(c, name, DMVS_JS_T_BOOL, 0, NULL);
    Dmod_SnPrintf(name, sizeof(name), "timer%u_due", (unsigned)(k + 1U));
    site->due = new_var(c, name, DMVS_JS_T_NUMBER, 0, NULL);
    Dmod_SnPrintf(name, sizeof(name), "timer%u_left", (unsigned)(k + 1U));
    site->left = new_var(c, name, DMVS_JS_T_NUMBER, 0, NULL);
    c->code = code;
    value_t undefined = v_undefined();
    site->handler = function_handler(c, &args[0], &undefined);
    c->code = code;
    c->at = at;
    if (site->handler == 0)
    {
        site->handler = dmvsi_add_handler(c->doc, NULL, 0);
        return v_number(0.0);
    }
    emit_op(c, DMVSI_ACT_SET, site->due, DMVSI_VAR_TIME, 0, NULL);
    emit_op(c, DMVSI_ACT_ADD, site->due, 0, (int32_t)site->ms, NULL);
    emit_op(c, DMVSI_ACT_SET, site->active, 0, 1, NULL);
    return v_number((double)(k + 1U));
}

static value_t clear_timer(compiler_t* c, const value_t* args, uint32_t count)
{
    if (c->evaluate)
        return v_undefined();
    if (count == 0)
        return v_undefined();
    double id;
    if (static_number(&args[0], &id))
    {
        if (id >= 1 && id <= c->site_count)
            emit_op(c, DMVSI_ACT_SET, c->sites[(uint32_t)id - 1U].active, 0, 0, NULL);
        return v_undefined();
    }
    if (args[0].kind == DMVS_JS_V_UNDEFINED)
        return v_undefined();
    if (args[0].kind != DMVS_JS_V_RUNTIME || args[0].type != DMVS_JS_T_NUMBER)
    {
        report(c, "clearing what is not a timer - not converted");
        return v_unknown();
    }
    /* Any timer it may be - also the ones compiled after it: a handler made at the end */
    dmvsi_var_t var;
    int32_t imm;
    if (!number_operand(c, &args[0], 0, &var, &imm))
        return v_undefined();
    if (c->clear == 0)
    {
        code_t* outer = c->code;
        c->code = NULL;
        c->clear_id = new_var(c, "timer_clear", DMVS_JS_T_NUMBER, 0, NULL);
        c->code = outer;
        c->clear = dmvsi_new_handler(c->doc);
        if (c->clear == 0)
            c->failed = true;
    }
    emit_op(c, DMVSI_ACT_SET, c->clear_id, var, imm, NULL);
    dmvsi_action_t call;
    memset(&call, 0, sizeof(call));
    call.kind = DMVSI_ACT_CALL;
    call.handler = c->clear;
    emit(c, &call);
    return v_undefined();
}

/*
 * The timers polling the sites: each runs every `period` ms - near what it
 * waits for - and calls the callback when it is due.
 */
int finish_timers(compiler_t* c)
{
    if (c->clear != 0)
    {
        dmvsi_action_t* clear = arena_alloc(&c->arena, (3U * c->site_count + 1U) * sizeof(dmvsi_action_t));
        if (clear == NULL)
            return -ENOMEM;
        uint32_t n = 0;
        for (uint32_t k = 0; k < c->site_count; k++)
        {
            clear[n].kind = DMVSI_ACT_IF_EQ;
            clear[n].var = c->clear_id;
            clear[n++].value = (int32_t)(k + 1U);
            clear[n].kind = DMVSI_ACT_SET;
            clear[n++].var = c->sites[k].active;
            clear[n++].kind = DMVSI_ACT_END;
        }
        if (dmvsi_set_handler(c->doc, c->clear, clear, n) != 0)
            return -EINVAL;
    }
    for (uint32_t k = 0; k < c->site_count; k++)
    {
        const site_t* site = &c->sites[k];
        dmvsi_action_t poll[10];
        uint32_t n = 0;
        memset(poll, 0, sizeof(poll));
        poll[n].kind = DMVSI_ACT_IF_NE;
        poll[n++].var = site->active;
        poll[n].kind = DMVSI_ACT_SET;
        poll[n].var = site->left;
        poll[n++].operand = site->due;
        poll[n].kind = DMVSI_ACT_SUB;
        poll[n].var = site->left;
        poll[n++].operand = DMVSI_VAR_TIME;
        poll[n].kind = DMVSI_ACT_IF_LE;
        poll[n++].var = site->left;
        if (site->repeat)
        {
            poll[n].kind = DMVSI_ACT_ADD;
            poll[n].var = site->due;
            poll[n++].value = (int32_t)((site->ms > 0) ? site->ms : 1u);
        }
        else
        {
            poll[n].kind = DMVSI_ACT_SET;
            poll[n++].var = site->active;
        }
        poll[n].kind = DMVSI_ACT_CALL;
        poll[n++].handler = site->handler;
        poll[n++].kind = DMVSI_ACT_END;
        poll[n++].kind = DMVSI_ACT_END;
        dmvsi_handler_t h = dmvsi_add_handler(c->doc, poll, n);
        uint32_t period = site->ms;
        if (period > 50u)
            period = 50u;
        if (period < 10u)
            period = 10u;
        if (h == 0 || dmvsi_add_timer(c->doc, period, h) != 0)
        {
            c->at = site->node;
            report(c, "a timer the document does not take - not converted");
            return -EINVAL;
        }
    }
    return 0;
}

/* ---- Calls ---- */

value_t builtin_call(compiler_t* c, const object_t* fn, const value_t* args, uint32_t count)
{
    value_t none = v_undefined();
    for (uint32_t i = 0; i < count; i++)
    {
        if (is_unknown(&args[i]) && fn->builtin != B_NOOP)
            return v_unknown();
    }
    if (is_unknown(&fn->self))
        return v_unknown();
    const value_t* a0 = (count > 0) ? &args[0] : &none;
    switch (fn->builtin)
    {
        case B_MATH_FLOOR:
        case B_MATH_ROUND:
        case B_MATH_CEIL:
        case B_MATH_TRUNC:
        {
            double n;
            if (static_number(a0, &n) || a0->kind == DMVS_JS_V_UNDEFINED)
            {
                if (a0->kind == DMVS_JS_V_UNDEFINED)
                    return v_number(0.0 / 0.0);
                double r = (fn->builtin == B_MATH_FLOOR) ? floor_of(n) : (fn->builtin == B_MATH_ROUND) ? floor_of(n + 0.5) :
                           (fn->builtin == B_MATH_CEIL) ? -floor_of(-n) : (n < 0) ? -(double)(int64_t)(-n) : (double)(int64_t)n;
                return v_number(r);
            }
            return rounding(c, fn->builtin, a0);
        }
        case B_MATH_MIN:
        case B_MATH_MAX:
            return min_max(c, fn->builtin == B_MATH_MAX, args, count);
        case B_MATH_ABS:
        {
            double n;
            if (static_number(a0, &n))
                return v_number((n < 0) ? -n : n);
            if (a0->kind != DMVS_JS_V_RUNTIME || a0->type == DMVS_JS_T_TEXT)
                return v_number(0.0 / 0.0);
            value_t r = number_copy(c, a0, number_scale(a0));
            emit_op(c, DMVSI_ACT_IF_LT, r.var, 0, 0, NULL);
            emit_op(c, DMVSI_ACT_MUL, r.var, 0, -1, NULL);
            emit_end(c);
            return r;
        }
        case B_PARSE_FLOAT:
        case B_NUMBER:
            if (count == 0)
                return v_number((fn->builtin == B_NUMBER) ? 0.0 : 0.0 / 0.0);
            return to_number(c, a0, false);
        case B_PARSE_INT:
            return to_number(c, a0, true);
        case B_STRING:
        case B_TO_STRING:
        {
            const value_t* v = (fn->builtin == B_STRING) ? a0 : &fn->self;
            if (fn->builtin == B_STRING && count == 0)
                return v_string(c, "", 0);
            return to_text(c, v);
        }
        case B_TO_FIXED:
            return to_fixed(c, &fn->self, args, count);
        case B_PAD_START:
            return pad_start(c, &fn->self, args, count);
        case B_STRING_METHOD:
            return string_method(c, &fn->self, fn->method, args, count);
        case B_ARRAY_METHOD:
            return array_method(c, as_object(&fn->self, O_ARRAY), fn->method, args, count);
        case B_FOR_EACH:
        {
            const object_t* a = as_object(&fn->self, O_ARRAY);
            for (uint32_t i = 0; a != NULL && count > 0 && i < a->count; i++)
            {
                value_t cb_args[3] = { a->values[i], v_number((double)i), fn->self };
                (void)call_value(c, &args[0], &none, cb_args, 3);
            }
            return v_undefined();
        }
        case B_SET_TIMEOUT:
        case B_SET_INTERVAL:
            return start_timer(c, fn->builtin == B_SET_INTERVAL, args, count);
        case B_CLEAR_TIMER:
            return clear_timer(c, args, count);
        case B_NOOP:
            return v_undefined();
        default:
            report(c, "a call of what is not a function - not converted");
            return v_unknown();
    }
}

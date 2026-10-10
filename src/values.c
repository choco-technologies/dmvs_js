#include "compiler.h"

/*
 * Values and the actions that work on them: static values are computed
 * here; runtime ones are variables, temporaries of the handler being made
 * when an expression needs them (released after every statement).
 */

/* ---- Reports ---- */

void report(compiler_t* c, const char* message)
{
    c->reports++;
    if (c->host.report != NULL)
        c->host.report(c->host.ctx, (c->at != NULL) ? c->at->line : 0, (c->at != NULL) ? c->at->column : 0, message);
}

/* ---- Values ---- */

value_t v_undefined(void)
{
    value_t v;
    memset(&v, 0, sizeof(v));
    v.kind = DMVS_JS_V_UNDEFINED;
    return v;
}

value_t v_unknown(void)
{
    value_t v = v_undefined();
    v.kind = DMVS_JS_V_UNKNOWN;
    return v;
}

bool is_unknown(const value_t* v)
{
    return v->kind == DMVS_JS_V_UNKNOWN;
}

value_t v_number(double n)
{
    value_t v = v_undefined();
    v.kind = DMVS_JS_V_NUMBER;
    v.number = n;
    return v;
}

value_t v_bool(bool b)
{
    value_t v = v_undefined();
    v.kind = DMVS_JS_V_BOOL;
    v.number = b ? 1.0 : 0.0;
    return v;
}

value_t v_string(compiler_t* c, const char* s, size_t n)
{
    value_t v = v_undefined();
    v.kind = DMVS_JS_V_STRING;
    v.text = arena_strndup(&c->arena, s, n);
    v.length = n;
    if (v.text == NULL)
    {
        c->failed = true;
        v.text = "";
        v.length = 0;
    }
    return v;
}

/* A character reference's text (&deg;): its code point, 0 when it is none the compiler knows */
static uint32_t entity(const char* name, size_t n)
{
    static const struct { char name[8]; uint16_t cp; } names[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
        { "deg", 0xB0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "middot", 0xB7 }, { "bull", 0x2022 },
        { "hellip", 0x2026 }, { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "times", 0xD7 }, { "euro", 0x20AC },
        { "plusmn", 0xB1 }, { "micro", 0xB5 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "larr", 0x2190 },
        { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "trade", 0x2122 },
    };
    if (n >= 2 && name[0] == '#')
    {
        uint32_t cp = 0;
        bool hex = name[1] == 'x' || name[1] == 'X';
        for (size_t i = hex ? 2 : 1; i < n; i++)
        {
            char ch = name[i];
            uint32_t d = (ch >= '0' && ch <= '9') ? (uint32_t)(ch - '0') : (hex && ch >= 'a' && ch <= 'f') ? (uint32_t)(ch - 'a' + 10) :
                         (hex && ch >= 'A' && ch <= 'F') ? (uint32_t)(ch - 'A' + 10) : 99u;
            if (d >= (hex ? 16u : 10u) || cp > 0x10FFFFu)
                return 0;
            cp = cp * (hex ? 16u : 10u) + d;
        }
        return (cp <= 0x10FFFFu) ? cp : 0;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strlen(names[i].name) == n && memcmp(names[i].name, name, n) == 0)
            return names[i].cp;
    }
    return 0;
}

value_t html_string(compiler_t* c, const char* s, size_t n)
{
    char* out = arena_alloc(&c->arena, n + 1U);         /* UTF-8 of a reference is shorter than it */
    if (out == NULL)
    {
        c->failed = true;
        return v_string(c, "", 0);
    }
    size_t k = 0;
    for (size_t i = 0; i < n; )
    {
        uint32_t cp = 0;
        size_t end = i + 1U;
        if (s[i] == '&')
        {
            while (end < n && end - i < 10U && s[end] != ';' && s[end] != '&')
                end++;
            cp = (end < n && s[end] == ';') ? entity(s + i + 1U, end - i - 1U) : 0;
        }
        if (cp == 0)
        {
            out[k++] = s[i++];
            continue;
        }
        if (cp < 0x80u)
            out[k++] = (char)cp;
        else if (cp < 0x800u)
        {
            out[k++] = (char)(0xC0u | (cp >> 6));
            out[k++] = (char)(0x80u | (cp & 0x3Fu));
        }
        else if (cp < 0x10000u)
        {
            out[k++] = (char)(0xE0u | (cp >> 12));
            out[k++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[k++] = (char)(0x80u | (cp & 0x3Fu));
        }
        else
        {
            out[k++] = (char)(0xF0u | (cp >> 18));
            out[k++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[k++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[k++] = (char)(0x80u | (cp & 0x3Fu));
        }
        i = end + 1U;
    }
    return v_string(c, out, k);
}

value_t v_runtime(uint8_t type, uint8_t scale, dmvsi_var_t var)
{
    value_t v = v_undefined();
    v.kind = DMVS_JS_V_RUNTIME;
    v.type = type;
    v.scale = (type == DMVS_JS_T_NUMBER) ? scale : 0u;
    v.var = var;
    return v;
}

value_t v_internal(const object_t* o)
{
    value_t v = v_undefined();
    v.kind = DMVS_JS_V_INTERNAL;
    v.internal = o;
    return v;
}

object_t* new_object(compiler_t* c, uint8_t kind)
{
    object_t* o = arena_alloc(&c->arena, sizeof(object_t));
    if (o == NULL)
    {
        c->failed = true;
        return NULL;
    }
    o->kind = kind;
    return o;
}

const object_t* as_object(const value_t* v, uint8_t kind)
{
    if (v->kind != DMVS_JS_V_INTERNAL || v->internal == NULL)
        return NULL;
    const object_t* o = v->internal;
    return (kind == 0 || o->kind == kind) ? o : NULL;
}

bool is_static(const value_t* v)
{
    return v->kind != DMVS_JS_V_RUNTIME && as_object(v, O_SELECT) == NULL;
}

bool static_number(const value_t* v, double* n)
{
    if (v->kind == DMVS_JS_V_NUMBER || v->kind == DMVS_JS_V_BOOL)
    {
        *n = v->number;
        return true;
    }
    if (v->kind == DMVS_JS_V_NULL)
    {
        *n = 0.0;
        return true;
    }
    return false;
}

bool truthy(const value_t* v)
{
    switch (v->kind)
    {
        case DMVS_JS_V_BOOL:
        case DMVS_JS_V_NUMBER:
            return v->number != 0.0 && v->number == v->number;     /* Not 0, not NaN */
        case DMVS_JS_V_STRING:
            return v->length > 0;
        case DMVS_JS_V_OBJECT:
        case DMVS_JS_V_INTERNAL:
            return true;
        default:
            return false;
    }
}

bool same_static(const value_t* a, const value_t* b)
{
    if (a->kind != b->kind)
    {
        double x, y;                                /* 1 == true: as numbers (loose, the compiler's own use) */
        return static_number(a, &x) && static_number(b, &y) && a->kind != DMVS_JS_V_NULL && b->kind != DMVS_JS_V_NULL && x == y;
    }
    switch (a->kind)
    {
        case DMVS_JS_V_BOOL:
        case DMVS_JS_V_NUMBER:
            return a->number == b->number;
        case DMVS_JS_V_STRING:
            return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
        case DMVS_JS_V_OBJECT:
            return a->object == b->object;
        case DMVS_JS_V_INTERNAL:
            return a->internal == b->internal;
        case DMVS_JS_V_RUNTIME:
            return a->var == b->var;
        default:
            return true;
    }
}

/* ---- Numbers as text, as JavaScript writes them ---- */

static size_t put_digits(char* out, uint64_t v)
{
    char d[24];
    size_t n = 0, k = 0;
    do
    {
        d[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n > 0)
        out[k++] = d[--n];
    return k;
}

/* A static number's text: integers as they are, fractions to 9 decimals at most (trailing zeros dropped) */
static size_t number_string(double n, char* out)
{
    size_t k = 0;
    if (n != n)
    {
        strcpy(out, "NaN");
        return 3;
    }
    if (n < 0)
    {
        out[k++] = '-';
        n = -n;
    }
    if (n > 1e18)
    {
        strcpy(out + k, "Infinity");
        return k + 8;
    }
    uint64_t ip = (uint64_t)n;
    double frac = n - (double)ip;
    uint64_t f = (uint64_t)(frac * 1e9 + 0.5);
    if (f >= 1000000000u)
    {
        ip++;
        f -= 1000000000u;
    }
    if (k == 1 && ip == 0 && f == 0)
        k = 0;                                      /* -0 */
    k += put_digits(out + k, ip);
    if (f != 0)
    {
        char digits[9];
        for (int i = 8; i >= 0; i--)
        {
            digits[i] = (char)('0' + (int)(f % 10u));
            f /= 10u;
        }
        int last = 8;
        while (last > 0 && digits[last] == '0')
            last--;
        out[k++] = '.';
        for (int i = 0; i <= last; i++)
            out[k++] = digits[i];
    }
    out[k] = '\0';
    return k;
}

value_t to_static_string(compiler_t* c, const value_t* v, bool* ok)
{
    char buf[40];
    *ok = true;
    switch (v->kind)
    {
        case DMVS_JS_V_STRING:
            return *v;
        case DMVS_JS_V_NUMBER:
            return v_string(c, buf, number_string(v->number, buf));
        case DMVS_JS_V_BOOL:
            return (v->number != 0.0) ? v_string(c, "true", 4) : v_string(c, "false", 5);
        case DMVS_JS_V_NULL:
            return v_string(c, "null", 4);
        case DMVS_JS_V_UNDEFINED:
            return v_string(c, "undefined", 9);
        default:
            break;
    }
    const object_t* a = as_object(v, O_ARRAY);
    if (a != NULL)
    {
        /* Its elements joined with ',' */
        size_t total = 0;
        value_t* parts = arena_alloc(&c->arena, (a->count + 1U) * sizeof(value_t));
        if (parts == NULL)
        {
            c->failed = true;
            return v_string(c, "", 0);
        }
        for (uint32_t i = 0; i < a->count && *ok; i++)
        {
            bool undefined = a->values[i].kind == DMVS_JS_V_UNDEFINED || a->values[i].kind == DMVS_JS_V_NULL;
            parts[i] = undefined ? v_string(c, "", 0) : to_static_string(c, &a->values[i], ok);
            total += parts[i].length + 1U;
        }
        char* out = arena_alloc(&c->arena, total + 1U);
        if (out == NULL || !*ok)
            return v_string(c, "", 0);
        size_t n = 0;
        for (uint32_t i = 0; i < a->count; i++)
        {
            if (i > 0)
                out[n++] = ',';
            memcpy(out + n, parts[i].text, parts[i].length);
            n += parts[i].length;
        }
        value_t s = v_undefined();
        s.kind = DMVS_JS_V_STRING;
        s.text = out;
        s.length = n;
        return s;
    }
    if (as_object(v, O_OBJECT) != NULL)
        return v_string(c, "[object Object]", 15);
    *ok = false;                                    /* Runtime, the host's: not known */
    return v_string(c, "", 0);
}

bool parse_number(const char* s, size_t n, double* out)
{
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    bool negative = false;
    if (i < n && (s[i] == '-' || s[i] == '+'))
        negative = s[i++] == '-';
    double v = 0.0, scale = 1.0;
    bool digits = false, fraction = false;
    for (; i < n; i++)
    {
        if (s[i] == '.' && !fraction)
        {
            fraction = true;
            continue;
        }
        if (s[i] < '0' || s[i] > '9')
            break;
        digits = true;
        if (fraction)
        {
            scale /= 10.0;
            v += (s[i] - '0') * scale;
        }
        else
            v = v * 10.0 + (s[i] - '0');
    }
    if (!digits)
        return false;
    if (i + 1U < n && (s[i] == 'e' || s[i] == 'E'))
    {
        size_t j = i + 1U;
        bool neg = false;
        if (s[j] == '-' || s[j] == '+')
            neg = s[j++] == '-';
        int e = 0;
        bool any = false;
        for (; j < n && s[j] >= '0' && s[j] <= '9' && e < 400; j++, any = true)
            e = e * 10 + (s[j] - '0');
        for (int k = 0; any && k < e; k++)
            v = neg ? v / 10.0 : v * 10.0;
    }
    *out = negative ? -v : v;
    return true;
}

/* ---- Code ---- */

void flush_host(compiler_t* c)
{
    if (c->host.flush == NULL || c->flushing || c->code == NULL)
        return;
    c->flushing = true;
    c->host.flush(c->host.ctx, (dmvs_js_compiler_t)c);
    c->flushing = false;
}

/* What makes the code go another way: the host's changes kept back are emitted before it */
static bool flow_kind(uint8_t kind)
{
    switch (kind)
    {
        case DMVSI_ACT_IF_EQ: case DMVSI_ACT_IF_NE: case DMVSI_ACT_IF_LT: case DMVSI_ACT_IF_LE:
        case DMVSI_ACT_IF_GT: case DMVSI_ACT_IF_GE: case DMVSI_ACT_ELSE: case DMVSI_ACT_END:
        case DMVSI_ACT_LOOP: case DMVSI_ACT_BREAK: case DMVSI_ACT_CONTINUE: case DMVSI_ACT_CALL:
        case DMVSI_ACT_RETURN:
            return true;
        default:
            return false;
    }
}

void track(compiler_t* c, dmvsi_var_t var, const value_t* v)
{
    bool object = v->kind == DMVS_JS_V_OBJECT;
    bool from = v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_NUMBER && v->var != var;
    if (var == 0 || !(object || from))
        return;
    for (const holds_t* h = c->holds; h != NULL; h = h->next)
    {
        if (h->var == var && (object ? (h->from == 0 && h->object == v->object) : h->from == v->var))
            return;
    }
    holds_t* h = arena_alloc(&c->arena, sizeof(holds_t));
    if (h == NULL)
    {
        c->failed = true;
        return;
    }
    h->var = var;
    h->from = object ? 0 : v->var;
    h->object = object ? v->object : 0;
    h->next = c->holds;
    c->holds = h;
}

int emit(compiler_t* c, const dmvsi_action_t* a)
{
    if (flow_kind(a->kind))
        flush_host(c);
    code_t* k = c->code;
    if (k->count == k->capacity)
    {
        uint32_t cap = (k->capacity == 0) ? 32u : k->capacity * 2u;
        dmvsi_action_t* bigger = arena_alloc(&c->arena, cap * sizeof(dmvsi_action_t));
        if (bigger == NULL)
        {
            c->failed = true;
            return -ENOMEM;
        }
        if (k->count != 0)
            memcpy(bigger, k->actions, k->count * sizeof(dmvsi_action_t));
        k->actions = bigger;                        /* The old one stays in the arena */
        k->capacity = cap;
    }
    dmvsi_action_t* to = &k->actions[k->count++];
    *to = *a;
    if (a->text != NULL)
    {
        to->text = arena_strndup(&c->arena, a->text, strlen(a->text));
        if (to->text == NULL)
        {
            c->failed = true;
            return -ENOMEM;
        }
    }
    return 0;
}

int emit_op(compiler_t* c, uint8_t kind, dmvsi_var_t var, dmvsi_var_t operand, int32_t value, const char* text)
{
    dmvsi_action_t a;
    memset(&a, 0, sizeof(a));
    a.kind = kind;
    a.var = var;
    a.operand = operand;
    a.value = value;
    a.text = text;
    return emit(c, &a);
}

int emit_end(compiler_t* c)
{
    return emit_op(c, DMVSI_ACT_END, 0, 0, 0, NULL);
}

dmvsi_var_t new_var(compiler_t* c, const char* name, uint8_t type, int32_t initial, const char* text)
{
    char full[64];
    const char* prefix = (c->code != NULL && c->code->prefix != NULL) ? c->code->prefix : NULL;
    if (prefix != NULL)
        Dmod_SnPrintf(full, sizeof(full), "%s_%s", prefix, name);
    else
        Dmod_SnPrintf(full, sizeof(full), "%s", name);
    dmvsi_var_t v = (type == DMVS_JS_T_TEXT) ? dmvsi_add_text_var(c->doc, full, TEXT_SIZE, (text != NULL) ? text : "")
                                             : dmvsi_add_var(c->doc, full, initial);
    if (v == 0)
        c->failed = true;
    return v;
}

dmvsi_var_t temp(compiler_t* c, uint8_t type)
{
    code_t* k = c->code;
    bool text = type == DMVS_JS_T_TEXT;
    dmvsi_var_t* pool = text ? k->text_temps : k->num_temps;
    uint32_t* used = text ? &k->text_used : &k->num_used;
    if (*used >= MAX_TEMPS)
    {
        report(c, "an expression too large (temporaries)");
        return pool[MAX_TEMPS - 1U];
    }
    if (pool[*used] == 0)
    {
        char name[16];
        Dmod_SnPrintf(name, sizeof(name), text ? "s%u" : "t%u", (unsigned)(*used + 1U));
        pool[*used] = new_var(c, name, type, 0, "");
    }
    return pool[(*used)++];
}

void release_temps(compiler_t* c)
{
    c->code->num_used = 0;
    c->code->text_used = 0;
}

uint8_t number_scale(const value_t* v)
{
    return (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_NUMBER) ? v->scale : 0u;
}

static int32_t pow10i(uint8_t n)
{
    int32_t p = 1;
    while (n-- > 0)
        p *= 10;
    return p;
}

bool number_operand(compiler_t* c, const value_t* v, uint8_t scale, dmvsi_var_t* var, int32_t* imm)
{
    double n;
    *var = 0;
    *imm = 0;
    if (v->kind == DMVS_JS_V_OBJECT)
    {
        *imm = (int32_t)v->object;                  /* The host's object: its handle */
        return true;
    }
    if (static_number(v, &n))
    {
        double scaled = n * pow10i(scale);
        if (!(scaled >= -2147483648.0 && scaled <= 2147483647.0))
        {
            report(c, "a number out of the range of the view (32 bits)");
            return false;
        }
        *imm = (int32_t)((scaled < 0) ? scaled - 0.5 : scaled + 0.5);
        return true;
    }
    if (v->kind != DMVS_JS_V_RUNTIME || v->type == DMVS_JS_T_TEXT)
        return false;
    uint8_t from = number_scale(v);
    if (from == scale)
    {
        *var = v->var;
        return true;
    }
    dmvsi_var_t t = temp(c, DMVS_JS_T_NUMBER);
    emit_op(c, DMVSI_ACT_SET, t, v->var, 0, NULL);
    if (scale > from)
        emit_op(c, DMVSI_ACT_MUL, t, 0, pow10i((uint8_t)(scale - from)), NULL);
    else
        emit_op(c, DMVSI_ACT_DIV, t, 0, pow10i((uint8_t)(from - scale)), NULL);
    *var = t;
    return true;
}

value_t number_copy(compiler_t* c, const value_t* v, uint8_t scale)
{
    dmvsi_var_t var;
    int32_t imm;
    if (!number_operand(c, v, scale, &var, &imm))
        return v_undefined();
    dmvsi_var_t t = temp(c, DMVS_JS_T_NUMBER);
    emit_op(c, DMVSI_ACT_SET, t, var, imm, NULL);
    return v_runtime(DMVS_JS_T_NUMBER, scale, t);
}

value_t to_bool(compiler_t* c, const value_t* v)
{
    if (is_static(v))
        return v_bool(truthy(v));
    if (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_BOOL)
        return *v;
    if (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_NUMBER)
    {
        dmvsi_var_t t = temp(c, DMVS_JS_T_BOOL);
        emit_op(c, DMVSI_ACT_SET, t, 0, 0, NULL);
        emit_op(c, DMVSI_ACT_IF_NE, v->var, 0, 0, NULL);
        emit_op(c, DMVSI_ACT_SET, t, 0, 1, NULL);
        emit_end(c);
        return v_runtime(DMVS_JS_T_BOOL, 0, t);
    }
    report(c, "whether a runtime text or a selected element is true - not converted");
    return v_bool(false);
}

bool text_operand(compiler_t* c, const value_t* v, dmvsi_var_t* var, const char** text)
{
    *var = 0;
    *text = NULL;
    if (is_unknown(v))
        return false;
    if (is_static(v))
    {
        bool ok = true;
        value_t s = to_static_string(c, v, &ok);
        *text = s.text;
        return ok;
    }
    value_t t = to_text(c, v);
    if (t.kind != DMVS_JS_V_RUNTIME)
    {
        *text = (t.kind == DMVS_JS_V_STRING) ? t.text : "";
        return t.kind == DMVS_JS_V_STRING;
    }
    *var = t.var;
    return true;
}

/* The digits of n at `decimals` decimals (n a scale-DMVS_JS_SCALE operand), fraction trailing zeros dropped */
int number_text(compiler_t* c, dmvsi_var_t text, const value_t* number, bool append)
{
    dmvsi_var_t nv;
    int32_t imm;
    if (!number_operand(c, number, number_scale(number), &nv, &imm))
        return -EINVAL;
    dmvsi_var_t piece = temp(c, DMVS_JS_T_TEXT);
    if (number_scale(number) == 0)
    {
        emit_op(c, DMVSI_ACT_FORMAT, append ? piece : text, nv, imm, "%d");
        if (append)
            emit_op(c, DMVSI_ACT_APPEND, text, piece, 0, NULL);
        return 0;
    }
    /* |n| into its integer part and thousandths */
    dmvsi_var_t a = temp(c, DMVS_JS_T_NUMBER), ip = temp(c, DMVS_JS_T_NUMBER), fr = temp(c, DMVS_JS_T_NUMBER);
    dmvsi_var_t q = temp(c, DMVS_JS_T_NUMBER);
    emit_op(c, DMVSI_ACT_SET, a, nv, imm, NULL);
    emit_op(c, DMVSI_ACT_IF_LT, a, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_MUL, a, 0, -1, NULL);
    emit_end(c);
    emit_op(c, DMVSI_ACT_SET, ip, a, 0, NULL);
    emit_op(c, DMVSI_ACT_DIV, ip, 0, 1000, NULL);
    emit_op(c, DMVSI_ACT_SET, fr, a, 0, NULL);
    emit_op(c, DMVSI_ACT_MOD, fr, 0, 1000, NULL);
    if (!append)
        emit_op(c, DMVSI_ACT_SET, text, 0, 0, "");
    emit_op(c, DMVSI_ACT_IF_LT, a, 0, 1, NULL);     /* Not "-0" */
    emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_SET, q, nv, imm, NULL);
    emit_op(c, DMVSI_ACT_IF_LT, q, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_APPEND, text, 0, 0, "-");
    emit_end(c);
    emit_end(c);
    emit_op(c, DMVSI_ACT_FORMAT, piece, ip, 0, "%d");
    emit_op(c, DMVSI_ACT_APPEND, text, piece, 0, NULL);
    emit_op(c, DMVSI_ACT_IF_NE, fr, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_APPEND, text, 0, 0, ".");
    emit_op(c, DMVSI_ACT_SET, q, fr, 0, NULL);
    emit_op(c, DMVSI_ACT_MOD, q, 0, 100, NULL);
    emit_op(c, DMVSI_ACT_IF_EQ, q, 0, 0, NULL);     /* .5 */
    emit_op(c, DMVSI_ACT_SET, q, fr, 0, NULL);
    emit_op(c, DMVSI_ACT_DIV, q, 0, 100, NULL);
    emit_op(c, DMVSI_ACT_FORMAT, piece, q, 0, "%d");
    emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_SET, q, fr, 0, NULL);
    emit_op(c, DMVSI_ACT_MOD, q, 0, 10, NULL);
    emit_op(c, DMVSI_ACT_IF_EQ, q, 0, 0, NULL);     /* .25 */
    emit_op(c, DMVSI_ACT_SET, q, fr, 0, NULL);
    emit_op(c, DMVSI_ACT_DIV, q, 0, 10, NULL);
    emit_op(c, DMVSI_ACT_FORMAT, piece, q, 0, "%02d");
    emit_op(c, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_FORMAT, piece, fr, 0, "%03d");     /* .125 */
    emit_end(c);
    emit_end(c);
    emit_op(c, DMVSI_ACT_APPEND, text, piece, 0, NULL);
    emit_end(c);
    return 0;
}

/* toFixed(decimals): rounded half away from zero (as JavaScript does, near enough) */
int fixed_text(compiler_t* c, dmvsi_var_t text, const value_t* number, uint32_t decimals, bool append)
{
    dmvsi_var_t nv;
    int32_t imm;
    uint8_t scale = number_scale(number);
    if (!number_operand(c, number, scale, &nv, &imm))
        return -EINVAL;
    dmvsi_var_t piece = temp(c, DMVS_JS_T_TEXT);
    dmvsi_var_t r = temp(c, DMVS_JS_T_NUMBER), ip = temp(c, DMVS_JS_T_NUMBER), fr = temp(c, DMVS_JS_T_NUMBER);
    uint32_t kept = (decimals < scale) ? decimals : scale;     /* Digits the variable has */
    int32_t unit = pow10i((uint8_t)kept);
    emit_op(c, DMVSI_ACT_SET, r, nv, imm, NULL);
    emit_op(c, DMVSI_ACT_IF_LT, r, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_MUL, r, 0, -1, NULL);
    emit_end(c);
    if (kept < scale)
    {
        int32_t drop = pow10i((uint8_t)(scale - kept));
        emit_op(c, DMVSI_ACT_ADD, r, 0, drop / 2, NULL);
        emit_op(c, DMVSI_ACT_DIV, r, 0, drop, NULL);
    }
    emit_op(c, DMVSI_ACT_SET, ip, r, 0, NULL);
    emit_op(c, DMVSI_ACT_DIV, ip, 0, unit, NULL);
    emit_op(c, DMVSI_ACT_SET, fr, r, 0, NULL);
    emit_op(c, DMVSI_ACT_MOD, fr, 0, unit, NULL);
    if (!append)
        emit_op(c, DMVSI_ACT_SET, text, 0, 0, "");
    emit_op(c, DMVSI_ACT_IF_NE, r, 0, 0, NULL);     /* "-0.0" is "0.0" */
    emit_op(c, DMVSI_ACT_SET, ip, nv, imm, NULL);   /* (ip: its sign, for a moment) */
    emit_op(c, DMVSI_ACT_IF_LT, ip, 0, 0, NULL);
    emit_op(c, DMVSI_ACT_APPEND, text, 0, 0, "-");
    emit_end(c);
    emit_op(c, DMVSI_ACT_SET, ip, r, 0, NULL);
    emit_op(c, DMVSI_ACT_DIV, ip, 0, unit, NULL);
    emit_end(c);
    emit_op(c, DMVSI_ACT_FORMAT, piece, ip, 0, "%d");
    emit_op(c, DMVSI_ACT_APPEND, text, piece, 0, NULL);
    if (decimals > 0)
    {
        emit_op(c, DMVSI_ACT_APPEND, text, 0, 0, ".");
        if (kept > 0)
        {
            char format[8];
            Dmod_SnPrintf(format, sizeof(format), "%%0%ud", (unsigned)kept);
            emit_op(c, DMVSI_ACT_FORMAT, piece, fr, 0, format);
            emit_op(c, DMVSI_ACT_APPEND, text, piece, 0, NULL);
        }
        for (uint32_t i = kept; i < decimals && i < 20u; i++)
            emit_op(c, DMVSI_ACT_APPEND, text, 0, 0, "0");
    }
    return 0;
}

value_t to_text(compiler_t* c, const value_t* v)
{
    if (is_static(v))
    {
        bool ok = true;
        value_t s = to_static_string(c, v, &ok);
        if (!ok)
            report(c, "a value without a text the view knows");
        return s;
    }
    if (v->kind == DMVS_JS_V_RUNTIME && v->type == DMVS_JS_T_TEXT)
        return *v;
    if (v->kind != DMVS_JS_V_RUNTIME)
    {
        report(c, "an element picked at runtime as text - not converted");
        return v_string(c, "", 0);
    }
    dmvsi_var_t t = temp(c, DMVS_JS_T_TEXT);
    value_t out = v_runtime(DMVS_JS_T_TEXT, 0, t);
    if (v->type == DMVS_JS_T_BOOL)
    {
        emit_op(c, DMVSI_ACT_SET, t, 0, 0, "false");
        emit_op(c, DMVSI_ACT_IF_NE, v->var, 0, 0, NULL);
        emit_op(c, DMVSI_ACT_SET, t, 0, 0, "true");
        emit_end(c);
        return out;
    }
    number_text(c, t, v, false);
    out.number_var = number_copy(c, v, FIX).var;
    return out;
}

value_t concat(compiler_t* c, const value_t* parts, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (is_unknown(&parts[i]))
            return v_unknown();
    }
    bool all_static = true;
    for (uint32_t i = 0; i < count; i++)
        all_static = all_static && is_static(&parts[i]);
    if (all_static)
    {
        size_t total = 0;
        value_t* s = arena_alloc(&c->arena, (count + 1U) * sizeof(value_t));
        if (s == NULL)
        {
            c->failed = true;
            return v_string(c, "", 0);
        }
        for (uint32_t i = 0; i < count; i++)
        {
            bool ok = true;
            s[i] = to_static_string(c, &parts[i], &ok);
            total += s[i].length;
        }
        char* out = arena_alloc(&c->arena, total + 1U);
        if (out == NULL)
        {
            c->failed = true;
            return v_string(c, "", 0);
        }
        size_t n = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            memcpy(out + n, s[i].text, s[i].length);
            n += s[i].length;
        }
        value_t v = v_undefined();
        v.kind = DMVS_JS_V_STRING;
        v.text = out;
        v.length = n;
        return v;
    }

    dmvsi_var_t t = temp(c, DMVS_JS_T_TEXT);
    value_t out = v_runtime(DMVS_JS_T_TEXT, 0, t);
    bool first = true;
    for (uint32_t i = 0; i < count; i++)
    {
        const value_t* p = &parts[i];
        if (is_static(p))
        {
            bool ok = true;
            value_t s = to_static_string(c, p, &ok);
            if (s.length == 0 && !first)
                continue;
            emit_op(c, first ? DMVSI_ACT_SET : DMVSI_ACT_APPEND, t, 0, 0, s.text);
        }
        else if (p->kind == DMVS_JS_V_RUNTIME && p->type == DMVS_JS_T_TEXT)
        {
            if (p->var == t)
                report(c, "a text appended to itself");
            emit_op(c, first ? DMVSI_ACT_SET : DMVSI_ACT_APPEND, t, p->var, 0, NULL);
            if (first && count > 1U && p->number_var != 0)
                out.number_var = p->number_var;             /* "22.5" + "°": still its number */
        }
        else if (p->kind == DMVS_JS_V_RUNTIME && p->type == DMVS_JS_T_NUMBER)
        {
            number_text(c, t, p, !first);
            if (first)
                out.number_var = number_copy(c, p, FIX).var;
        }
        else
        {
            value_t s = to_text(c, p);
            dmvsi_var_t var;
            const char* text;
            if (text_operand(c, &s, &var, &text))
                emit_op(c, first ? DMVSI_ACT_SET : DMVSI_ACT_APPEND, t, var, 0, text);
        }
        if (i > 0 && out.number_var != 0 && !is_static(p))
            out.number_var = 0;                             /* More than the number and static text */
        first = false;
    }
    return out;
}

int assign_to(compiler_t* c, const value_t* target, const value_t* v)
{
    if (is_unknown(v))
        return 0;                                   /* Reported: the variable stays as it is */
    if (target->type == DMVS_JS_T_TEXT)
    {
        dmvsi_var_t var;
        const char* text;
        if (!text_operand(c, v, &var, &text))
            return -EINVAL;
        if (var == target->var)
            return 0;
        return emit_op(c, DMVSI_ACT_SET, target->var, var, 0, (var == 0) ? text : NULL);
    }
    value_t b = (target->type == DMVS_JS_T_BOOL) ? to_bool(c, v) : *v;
    dmvsi_var_t var;
    int32_t imm;
    if (target->type == DMVS_JS_T_NUMBER && target->scale == 0)
        track(c, target->var, v);
    if (!number_operand(c, &b, target->scale, &var, &imm))
    {
        report(c, "a value of another type than the variable's - not converted");
        return -EINVAL;
    }
    if (var == target->var)
        return 0;
    return emit_op(c, DMVSI_ACT_SET, target->var, var, imm, NULL);
}

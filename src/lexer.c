#include "private.h"

/*
 * The lexer: the source into tokens, one at a time. A '/' is a division
 * or the start of a regular expression by the token before it; a template
 * literal comes in chunks - up to each ${ and, after the parser has read
 * the expression and its '}', on from there (lex_template_continue()).
 * Strings and template chunks are decoded (escapes) into the arena;
 * numbers are kept exactly as number / 10^decimals.
 */

#define MAX_DIGITS      18u             /* Significant digits an int64_t holds */
#define MAX_DECIMALS    30u

/* Text, not pointers: separated by spaces, longest first */
static const char g_punct4[] = " >>>= ";
static const char g_punct3[] = " ... === !== **= <<= >>= >>> &&= ||= ?\?= ";
static const char g_punct2[] = " => == != <= >= && || ?? ?. ++ -- += -= *= /= %= &= |= ^= ** << >> ";
static const char g_punct1[] = "{}()[];,<>+-*/%&|^!~?:=.@";

/* Reserved words - never a name of a variable */
static const char g_keywords[] =
    " break case catch class const continue debugger default delete do else export extends finally for function if "
    "import in instanceof new return super switch this throw try typeof var void while with null true false ";

/* Names after which a '/' starts a regular expression */
static const char g_regex_after[] = " return typeof instanceof in of new delete void throw case do else yield await ";

static bool in_list(const char* list, const char* s, size_t n)
{
    for (const char* p = list; *p != '\0'; p++)
    {
        if (p[0] == ' ' && strncmp(p + 1, s, n) == 0 && p[1 + n] == ' ')
            return true;
    }
    return false;
}

bool is_keyword(const char* s, size_t n)
{
    return n > 0 && in_list(g_keywords, s, n);
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hex_value(char c) { return is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10; }
static bool name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (uint8_t)c >= 0x80u; }
static bool name_char(char c) { return name_start(c) || is_digit(c); }

/* ---- Errors ---- */

bool lex_failed(const lexer_t* l)
{
    return l->error->status != 0;
}

static void fail_status(lexer_t* l, uint32_t offset, int status, const char* message)
{
    if (l->error->status != 0)
        return;                                     /* The first error is the one */
    uint32_t line = 1, column = 1;
    for (uint32_t i = 0; i < offset && i < l->length; i++)
    {
        if (l->src[i] == '\n')
        {
            line++;
            column = 1;
        }
        else
            column++;
    }
    l->error->status = status;
    l->error->offset = offset;
    l->error->line = line;
    l->error->column = column;
    size_t n = strlen(message);
    if (n >= sizeof(l->error->message))
        n = sizeof(l->error->message) - 1U;
    memcpy(l->error->message, message, n);
    l->error->message[n] = '\0';
    l->tok.kind = T_EOF;
    l->pos = l->length;
}

void lex_fail(lexer_t* l, uint32_t offset, const char* message)
{
    fail_status(l, offset, -EBADMSG, message);
}

void lex_fail_at(lexer_t* l, const token_t* t, int status, const char* message)
{
    fail_status(l, t->start, status, message);
}

/* ---- Characters ---- */

static char peek(const lexer_t* l, uint32_t ahead)
{
    return (l->pos + ahead < l->length) ? l->src[l->pos + ahead] : '\0';
}

static void newline_at(lexer_t* l, uint32_t after)
{
    l->line++;
    l->line_start = after;
}

/* Spaces and comments; true when a line break was among them */
static bool skip_blank(lexer_t* l)
{
    bool newline = false;
    while (l->pos < l->length)
    {
        char c = l->src[l->pos];
        if (c == '\n')
        {
            l->pos++;
            newline_at(l, l->pos);
            newline = true;
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
            l->pos++;
        else if ((uint8_t)c == 0xC2u && (uint8_t)peek(l, 1) == 0xA0u)
            l->pos += 2;                            /* NO-BREAK SPACE */
        else if ((uint8_t)c == 0xEFu && (uint8_t)peek(l, 1) == 0xBBu && (uint8_t)peek(l, 2) == 0xBFu)
            l->pos += 3;                            /* A byte order mark */
        else if (c == '/' && peek(l, 1) == '/')
        {
            while (l->pos < l->length && l->src[l->pos] != '\n')
                l->pos++;
        }
        else if (c == '/' && peek(l, 1) == '*')
        {
            uint32_t start = l->pos;
            l->pos += 2;
            while (l->pos < l->length && !(l->src[l->pos] == '*' && peek(l, 1) == '/'))
            {
                if (l->src[l->pos] == '\n')
                {
                    newline_at(l, l->pos + 1U);
                    newline = true;
                }
                l->pos++;
            }
            if (l->pos >= l->length)
            {
                lex_fail(l, start, "unterminated comment");
                return newline;
            }
            l->pos += 2;
        }
        else
            break;
    }
    return newline;
}

/* ---- Strings and templates ---- */

static size_t put_utf8(char* out, uint32_t cp)
{
    if (cp < 0x80u)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u)
    {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u)
    {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* \u{...} or \uXXXX after the 'u'; false when it is not one */
static bool unicode_escape(lexer_t* l, uint32_t* cp)
{
    uint32_t v = 0;
    if (peek(l, 0) == '{')
    {
        l->pos++;
        int n = 0;
        while (is_hex(peek(l, 0)) && n < 8)
        {
            v = v * 16u + (uint32_t)hex_value(peek(l, 0));
            l->pos++;
            n++;
        }
        if (n == 0 || peek(l, 0) != '}' || v > 0x10FFFFu)
            return false;
        l->pos++;
        *cp = v;
        return true;
    }
    for (int i = 0; i < 4; i++)
    {
        if (!is_hex(peek(l, 0)))
            return false;
        v = v * 16u + (uint32_t)hex_value(peek(l, 0));
        l->pos++;
    }
    *cp = v;
    return true;
}

/*
 * The characters up to `quote` (', ", or ` - which also stops at ${),
 * decoded into the arena; l->pos at the closing quote (or the $ of ${).
 */
static bool decode(lexer_t* l, char quote, token_t* t)
{
    uint32_t start = l->pos;
    /* Where it ends (escapes skipped): decoded, it is never longer */
    uint32_t end = l->pos;
    while (end < l->length && l->src[end] != quote && !(quote == '`' && l->src[end] == '$' && end + 1U < l->length &&
                                                       l->src[end + 1U] == '{'))
        end += (l->src[end] == '\\') ? 2U : 1U;
    if (end > l->length)
        end = l->length;
    char* out = arena_alloc(l->arena, (size_t)(end - start) + 4U);
    if (out == NULL)
    {
        fail_status(l, start, -ENOMEM, "out of memory");
        return false;
    }
    size_t n = 0;
    uint32_t pending_high = 0;                      /* A high surrogate waiting for its low one */
    while (l->pos < l->length)
    {
        char c = l->src[l->pos];
        if (c == quote || (quote == '`' && c == '$' && peek(l, 1) == '{'))
            break;
        if (c == '\n')
        {
            if (quote != '`')
            {
                lex_fail(l, start - 1U, "unterminated string");
                return false;
            }
            newline_at(l, l->pos + 1U);
        }
        if (pending_high != 0 && !(c == '\\' && peek(l, 1) == 'u'))
        {
            n += put_utf8(out + n, 0xFFFDu);
            pending_high = 0;
        }
        if (c != '\\')
        {
            if (!(quote == '`' && c == '\r'))       /* A template's \r\n is \n */
                out[n++] = c;
            l->pos++;
            continue;
        }
        l->pos++;
        char e = peek(l, 0);
        l->pos++;
        uint32_t cp = 0;
        bool code = false;
        switch (e)
        {
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'v': out[n++] = '\v'; break;
            case '0':
                if (is_digit(peek(l, 0)))
                {
                    lex_fail(l, l->pos - 2U, "octal escapes are not allowed");
                    return false;
                }
                out[n++] = '\0';
                break;
            case 'x':
                if (!is_hex(peek(l, 0)) || !is_hex(peek(l, 1)))
                {
                    lex_fail(l, l->pos - 2U, "invalid \\x escape");
                    return false;
                }
                cp = (uint32_t)(hex_value(peek(l, 0)) * 16 + hex_value(peek(l, 1)));
                l->pos += 2;
                code = true;
                break;
            case 'u':
                if (!unicode_escape(l, &cp))
                {
                    lex_fail(l, l->pos - 2U, "invalid \\u escape");
                    return false;
                }
                code = true;
                break;
            case '\r':
                if (peek(l, 0) == '\n')
                    l->pos++;
                newline_at(l, l->pos);
                break;                              /* A line continuation */
            case '\n':
                newline_at(l, l->pos);
                break;
            case '\0':
                lex_fail(l, start - 1U, "unterminated string");
                return false;
            default:
                l->pos--;                           /* Any other character stands for itself (UTF-8 bytes too) */
                out[n++] = l->src[l->pos++];
                break;
        }
        if (!code)
            continue;
        if (cp >= 0xDC00u && cp <= 0xDFFFu && pending_high != 0)
        {
            cp = 0x10000u + ((pending_high - 0xD800u) << 10) + (cp - 0xDC00u);
            pending_high = 0;
        }
        else if (pending_high != 0)
        {
            n += put_utf8(out + n, 0xFFFDu);        /* A lone surrogate */
            pending_high = 0;
        }
        if (cp >= 0xD800u && cp <= 0xDBFFu)
            pending_high = cp;
        else
            n += put_utf8(out + n, (cp >= 0xDC00u && cp <= 0xDFFFu) ? 0xFFFDu : cp);
    }
    if (pending_high != 0)
        n += put_utf8(out + n, 0xFFFDu);
    if (l->pos >= l->length)
    {
        lex_fail(l, start - 1U, (quote == '`') ? "unterminated template" : "unterminated string");
        return false;
    }
    out[n] = '\0';
    t->text = out;
    t->length = n;
    return true;
}

static void template_chunk(lexer_t* l, token_t* t)
{
    t->kind = T_TEMPLATE;
    if (!decode(l, '`', t))
        return;
    if (l->src[l->pos] == '`')
    {
        t->tail = true;
        l->pos++;
    }
    else
        l->pos += 2;                                /* ${ */
}

void lex_template_continue(lexer_t* l)
{
    token_t t;
    memset(&t, 0, sizeof(t));
    t.start = l->tok.start;                         /* The '}' */
    t.line = l->tok.line;
    t.column = l->tok.column;
    l->pos = l->tok.start + 1U;
    template_chunk(l, &t);
    t.end = l->pos;
    l->tok = t;
    l->regex_ok = !t.tail;
}

/* ---- Numbers ---- */

static void number(lexer_t* l, token_t* t)
{
    uint32_t start = l->pos;
    int64_t v = 0;
    uint32_t digits = 0;
    int32_t decimals = 0;
    bool inexact = false;
    t->kind = T_NUMBER;

    char c = peek(l, 0), x = peek(l, 1);
    int radix = (c != '0') ? 10 : (x == 'x' || x == 'X') ? 16 : (x == 'o' || x == 'O') ? 8 : (x == 'b' || x == 'B') ? 2 : 10;
    if (radix != 10)
    {
        l->pos += 2;
        uint32_t n = 0;
        for (;; l->pos++)
        {
            char d = peek(l, 0);
            if (d == '_')
                continue;
            int dv = is_hex(d) ? hex_value(d) : 99;
            if (dv >= radix)
                break;
            if (v > (INT64_MAX - dv) / radix)
                inexact = true;
            else
                v = v * radix + dv;
            n++;
        }
        if (n == 0)
        {
            lex_fail(l, start, "a number without digits");
            return;
        }
    }
    else
    {
        bool fraction = false;
        for (;; l->pos++)
        {
            char d = peek(l, 0);
            if (d == '_')
                continue;
            if (d == '.' && !fraction)
            {
                fraction = true;
                continue;
            }
            if (!is_digit(d))
                break;
            if (digits == 0 && d == '0' && !fraction)
                continue;                           /* Leading zeros */
            if (digits < MAX_DIGITS)
            {
                v = v * 10 + (d - '0');
                digits++;
                if (fraction)
                    decimals++;
            }
            else if (fraction)
                inexact = inexact || d != '0';
            else
            {
                decimals--;                         /* An integer digit beyond what fits: x10 later */
                inexact = inexact || d != '0';
            }
        }
        if (peek(l, 0) == 'e' || peek(l, 0) == 'E')
        {
            uint32_t at = l->pos;
            l->pos++;
            bool negative = peek(l, 0) == '-';
            if (peek(l, 0) == '-' || peek(l, 0) == '+')
                l->pos++;
            if (!is_digit(peek(l, 0)))
            {
                lex_fail(l, at, "an exponent without digits");
                return;
            }
            int32_t e = 0;
            while (is_digit(peek(l, 0)))
            {
                if (e < 10000)
                    e = e * 10 + (peek(l, 0) - '0');
                l->pos++;
            }
            decimals += negative ? e : -e;
        }
        while (decimals < 0)
        {
            if (v > INT64_MAX / 10)
            {
                inexact = true;
                break;
            }
            v *= 10;
            decimals++;
        }
        while (decimals > (int32_t)MAX_DECIMALS || (decimals > 0 && v % 10 == 0 && v != 0))
        {
            inexact = inexact || (v % 10 != 0);
            v /= 10;
            decimals--;
        }
        if (v == 0)
            decimals = 0;
    }
    if (peek(l, 0) == 'n')
    {
        lex_fail(l, start, "BigInt is not supported");
        return;
    }
    if (name_char(peek(l, 0)))
    {
        lex_fail(l, start, "an identifier right after a number");
        return;
    }
    t->number = v;
    t->decimals = (uint8_t)((decimals < 0) ? 0 : decimals);
    t->inexact = inexact;
}

/* ---- Tokens ---- */

static bool punct(lexer_t* l, token_t* t)
{
    const char* s = l->src + l->pos;
    uint32_t left = l->length - l->pos;
    if ((left >= 4 && in_list(g_punct4, s, 4)) || (left >= 3 && in_list(g_punct3, s, 3)) ||
        (left >= 2 && in_list(g_punct2, s, 2) && !(s[0] == '?' && s[1] == '.' && left >= 3 && is_digit(s[2]))))
    {
        l->pos += (left >= 4 && in_list(g_punct4, s, 4)) ? 4U : (left >= 3 && in_list(g_punct3, s, 3)) ? 3U : 2U;
        t->kind = T_PUNCT;
        return true;
    }
    if (strchr(g_punct1, s[0]) != NULL && s[0] != '\0')
    {
        l->pos++;
        t->kind = T_PUNCT;
        return true;
    }
    return false;
}

static void regex(lexer_t* l, token_t* t)
{
    uint32_t start = l->pos;
    bool in_class = false;
    l->pos++;
    for (;;)
    {
        char c = peek(l, 0);
        if (c == '\0' || c == '\n')
        {
            lex_fail(l, start, "unterminated regular expression");
            return;
        }
        l->pos++;
        if (c == '\\')
            l->pos++;
        else if (c == '[')
            in_class = true;
        else if (c == ']')
            in_class = false;
        else if (c == '/' && !in_class)
            break;
    }
    while (name_char(peek(l, 0)))
        l->pos++;
    t->kind = T_REGEX;
}

void lex_next(lexer_t* l)
{
    token_t t;
    memset(&t, 0, sizeof(t));
    if (lex_failed(l))
    {
        l->tok = t;
        return;
    }
    t.newline = skip_blank(l);
    t.start = l->pos;
    t.line = l->line;
    t.column = l->pos - l->line_start + 1U;
    if (lex_failed(l))
    {
        l->tok.kind = T_EOF;
        return;
    }

    char c = peek(l, 0);
    if (l->pos >= l->length)
        t.kind = T_EOF;
    else if (name_start(c) || (c == '#' && name_start(peek(l, 1))))
    {
        t.kind = (c == '#') ? T_PRIVATE : T_NAME;
        l->pos++;
        while (name_char(peek(l, 0)))
            l->pos++;
        if (peek(l, 0) == '\\')
            lex_fail(l, l->pos, "escapes in names are not supported");
    }
    else if (c == '\\')
        lex_fail(l, l->pos, "escapes in names are not supported");
    else if (is_digit(c) || (c == '.' && is_digit(peek(l, 1))))
        number(l, &t);
    else if (c == '\'' || c == '"')
    {
        t.kind = T_STRING;
        l->pos++;
        if (decode(l, c, &t))
            l->pos++;
    }
    else if (c == '`')
    {
        l->pos++;
        template_chunk(l, &t);
    }
    else if (c == '/' && l->regex_ok)
        regex(l, &t);
    else if (!punct(l, &t))
        lex_fail(l, l->pos, "an unexpected character");

    t.end = l->pos;
    if (lex_failed(l))
    {
        l->tok.kind = T_EOF;
        return;
    }

    /* Whether a '/' after it starts a regular expression */
    const char* s = l->src + t.start;
    size_t n = t.end - t.start;
    if (t.kind == T_NAME)
        l->regex_ok = in_list(g_regex_after, s, n);
    else if (t.kind == T_PUNCT)
        l->regex_ok = !(n == 1 && (s[0] == ')' || s[0] == ']' || s[0] == '}'));
    else if (t.kind == T_TEMPLATE)
        l->regex_ok = !t.tail;
    else
        l->regex_ok = false;
    l->tok = t;
}

void lex_init(lexer_t* l, const char* src, size_t length, arena_t* arena, dmvs_js_error_t* error)
{
    memset(l, 0, sizeof(*l));
    l->src = src;
    l->length = (uint32_t)length;
    l->line = 1;
    l->regex_ok = true;
    l->arena = arena;
    l->error = error;
    if (length > 2 && src[0] == '#' && src[1] == '!')       /* A hashbang line */
    {
        while (l->pos < l->length && src[l->pos] != '\n')
            l->pos++;
    }
    lex_next(l);
}

bool tok_is(const lexer_t* l, const char* text)
{
    size_t n = strlen(text);
    return (l->tok.kind == T_PUNCT || l->tok.kind == T_NAME) && l->tok.end - l->tok.start == n &&
           strncmp(l->src + l->tok.start, text, n) == 0;
}

bool tok_name(const lexer_t* l, const char* name)
{
    return l->tok.kind == T_NAME && tok_is(l, name);
}

#include "private.h"

/*
 * The source the lexer reads: all of it in memory, or a stream read in
 * pieces into a window. The window holds the bytes from the earliest one
 * the parser may still need - the current token, or where a look ahead
 * started (a mark) - to the furthest one read; what is before that is
 * dropped when more is read, and the window grows only when a token or a
 * look ahead is longer than it.
 */

#define READ_PIECE      512u
#define FIRST_WINDOW    2048u

void source_memory(source_t* s, const char* text, size_t length)
{
    memset(s, 0, sizeof(*s));
    s->buf = (char*)text;
    s->len = (uint32_t)length;
    s->eof = true;
    s->peak = s->len;
}

void source_stream(source_t* s, dmvs_js_read_fn read, void* ctx)
{
    memset(s, 0, sizeof(*s));
    s->read = read;
    s->ctx = ctx;
    s->owned = true;
}

void source_release(source_t* s)
{
    if (s->owned && s->buf != NULL)
        Dmod_Free(s->buf);
    s->buf = NULL;
}

/* The first byte still needed */
static uint32_t lowest(const source_t* s)
{
    uint32_t low = s->keep;
    for (uint32_t i = 0; i < s->mark_count && i < SOURCE_MARKS; i++)
    {
        if (s->marks[i] < low)
            low = s->marks[i];
    }
    return low;
}

/* Read more of the stream: false at its end (or on an error) */
static bool refill(source_t* s)
{
    if (s->eof)
        return false;
    uint32_t low = lowest(s);
    if (low > s->base)
    {
        uint32_t drop = low - s->base;              /* What nobody needs any more */
        if (drop > s->len)
            drop = s->len;
        memmove(s->buf, s->buf + drop, s->len - drop);
        s->base += drop;
        s->len -= drop;
    }
    if (s->cap - s->len < READ_PIECE)
    {
        uint32_t cap = (s->cap == 0) ? FIRST_WINDOW : s->cap * 2u;
        char* bigger = Dmod_Malloc(cap);
        if (bigger == NULL)
        {
            s->failed = -ENOMEM;
            s->eof = true;
            return false;
        }
        if (s->buf != NULL)
        {
            memcpy(bigger, s->buf, s->len);
            Dmod_Free(s->buf);
        }
        s->buf = bigger;
        s->cap = cap;
    }
    int32_t n = s->read(s->ctx, s->buf + s->len, s->cap - s->len);
    if (n <= 0)
    {
        if (n < 0)
            s->failed = -EIO;
        s->eof = true;
        return false;
    }
    s->len += (uint32_t)n;
    if (s->len > s->peak)
        s->peak = s->len;
    return true;
}

bool source_more(source_t* s, uint32_t offset)
{
    while (offset >= s->base + s->len)
    {
        if (!refill(s))
            return false;
    }
    return true;
}

char source_at(source_t* s, uint32_t offset)
{
    if (offset < s->base + s->len && offset >= s->base)
        return s->buf[offset - s->base];
    if (offset < s->base || !source_more(s, offset))
        return '\0';
    return s->buf[offset - s->base];
}

const char* source_text(source_t* s, uint32_t offset)
{
    return s->buf + (offset - s->base);
}

void source_mark(source_t* s, uint32_t offset)
{
    if (s->mark_count < SOURCE_MARKS)
        s->marks[s->mark_count] = offset;
    s->mark_count++;
}

void source_unmark(source_t* s)
{
    if (s->mark_count > 0)
        s->mark_count--;
}

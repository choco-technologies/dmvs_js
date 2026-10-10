#include "dmod.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/**
 * @brief jsdump - the syntax tree of a script, as dmvs_js_dump() writes it.
 *
 * Usage: jsdump [-o OUTPUT] [-s PIECE] [-i] SCRIPT.js
 *
 * Prints the tree (or writes it to OUTPUT), or where the script cannot be
 * parsed: SCRIPT.js:LINE:COLUMN: MESSAGE. tests/acorn/tree.js writes
 * acorn's tree the same way - to compare the two.
 *
 *   -s PIECE  read the script as a stream, PIECE bytes at a time
 *             (dmvs_js_parse_stream()) instead of all of it at once
 *   -i        print what the tree took: the script, the most of it held
 *             at once, the tree's memory, its nodes
 */

typedef struct
{
    void*   file;
    size_t  piece;
} stream_t;

static int32_t read_piece(void* ctx, char* buffer, size_t size)
{
    stream_t* s = ctx;
    if (size > s->piece)
        size = s->piece;
    return (int32_t)Dmod_FileRead(buffer, 1, size, s->file);
}

static void usage(const char* name)
{
    Dmod_Printf("Usage: %s [-o OUTPUT] [-s PIECE] [-i] SCRIPT.js\n", name);
}

int main(int argc, char* argv[])
{
    const char* input = NULL;
    const char* output = NULL;
    size_t piece = 0;
    bool info = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
        {
            piece = 0;
            for (const char* p = argv[++i]; *p >= '0' && *p <= '9'; p++)
                piece = piece * 10u + (size_t)(*p - '0');
            if (piece == 0)
            {
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-i") == 0)
            info = true;
        else if (argv[i][0] != '-' && input == NULL)
            input = argv[i];
        else
        {
            usage(argv[0]);
            return 1;
        }
    }
    if (input == NULL)
    {
        usage(argv[0]);
        return 1;
    }

    void* f = Dmod_FileOpen(input, "rb");
    if (f == NULL)
    {
        Dmod_Printf("jsdump: cannot open %s\n", input);
        return -ENOENT;
    }
    dmvs_js_error_t error;
    dmvs_js_ast_t ast;
    char* source = NULL;
    if (piece != 0)
    {
        stream_t stream = { f, piece };
        ast = dmvs_js_parse_stream(read_piece, &stream, &error);
        Dmod_FileClose(f);
    }
    else
    {
        size_t size = (size_t)Dmod_FileSize(f);
        source = Dmod_Malloc(size + 1U);
        size_t n = (source != NULL) ? Dmod_FileRead(source, 1, size, f) : 0;
        Dmod_FileClose(f);
        if (source == NULL)
            return -ENOMEM;
        ast = dmvs_js_parse(source, n, &error);
        Dmod_Free(source);                          /* The tree does not refer to it */
    }
    if (ast == NULL)
    {
        Dmod_Printf("%s:%u:%u: %s\n", input, (unsigned)error.line, (unsigned)error.column, error.message);
        return error.status;
    }
    if (info)
    {
        dmvs_js_info_t i;
        (void)dmvs_js_info(ast, &i);
        Dmod_Printf("%s: %u bytes, %u at most at once, tree %u bytes, %u nodes\n", input, (unsigned)i.source,
                    (unsigned)i.window, (unsigned)i.tree, (unsigned)i.nodes);
    }
    size_t need = dmvs_js_dump(dmvs_js_root(ast), NULL, 0);
    char* text = Dmod_Malloc(need + 1U);
    int ret = 0;
    if (text == NULL)
        ret = -ENOMEM;
    else
    {
        dmvs_js_dump(dmvs_js_root(ast), text, need + 1U);
        if (output != NULL)
        {
            void* out = Dmod_FileOpen(output, "wb");
            ret = (out != NULL && Dmod_FileWrite(text, 1, need, out) == need) ? 0 : -EIO;
            if (out != NULL)
                Dmod_FileClose(out);
        }
        else
            Dmod_Printf("%s\n", text);
        Dmod_Free(text);
    }
    dmvs_js_free(ast);
    return ret;
}

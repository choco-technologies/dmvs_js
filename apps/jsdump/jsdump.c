#include "dmod.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/**
 * @brief jsdump - the syntax tree of a script, as dmvs_js_dump() writes it.
 *
 * Usage: jsdump [-o OUTPUT] SCRIPT.js
 *
 * Prints the tree (or writes it to OUTPUT), or where the script cannot be
 * parsed: SCRIPT.js:LINE:COLUMN: MESSAGE. tests/acorn/tree.js writes
 * acorn's tree the same way - to compare the two.
 */

static void usage(const char* name)
{
    Dmod_Printf("Usage: %s [-o OUTPUT] SCRIPT.js\n", name);
}

int main(int argc, char* argv[])
{
    const char* input = NULL;
    const char* output = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
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
    size_t size = (size_t)Dmod_FileSize(f);
    char* source = Dmod_Malloc(size + 1U);
    size_t n = (source != NULL) ? Dmod_FileRead(source, 1, size, f) : 0;
    Dmod_FileClose(f);
    if (source == NULL)
        return -ENOMEM;

    dmvs_js_error_t error;
    dmvs_js_ast_t ast = dmvs_js_parse(source, n, &error);
    if (ast == NULL)
    {
        Dmod_Printf("%s:%u:%u: %s\n", input, (unsigned)error.line, (unsigned)error.column, error.message);
        Dmod_Free(source);
        return error.status;
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
    Dmod_Free(source);
    return ret;
}

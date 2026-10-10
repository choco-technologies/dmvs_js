# dmvs_js

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmvs_js/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmvs_js/actions/workflows/ci.yml)

JavaScript for dmview views. A page's scripts are compiled into the view's
own code (dmvs) - nothing interprets JavaScript on the device. This module
is that compiler's front end: a parser of the JavaScript pages use into a
syntax tree.

## The parser

- **ES2022 scripts**: `let` / `const`, arrow and async functions,
  generators, classes (fields, static and private members, getters /
  setters), template literals (tagged too), destructuring with defaults and
  rest, spread, optional chaining, `??` and the logical assignments,
  `for ... of`, labels, regular expression literals; automatic semicolons
  as the language inserts them.
- **A compact tree**: every node has the same shape - a kind, an operator,
  flags, up to four children and the next node of a list - with where it
  is in the source (offsets, line, column). See
  [docs/api-reference.md](docs/api-reference.md).
- **Numbers exactly**: as `number / 10^decimals` (1.5 is 15 / 10^1), as a
  fixed-point compiler wants them.
- **For small targets**: no tables of pointers in the module's data (dmod
  does not relocate it), statements and expressions nested at most 64 deep
  (about 0.6 KiB of stack per level on a Cortex-M7), the tree in chunks of
  16 KiB freed at once.
- Not there: modules (`import` / `export`), escapes in names, BigInt, HTML
  comments.

The tree is the one [acorn](https://github.com/acornjs/acorn) makes, node
for node: lodash, Alpine.js, Chart.js, Vue and three.js (2.6 MB) parse into
the same trees - `tests/acorn/compare.sh` checks any script.

## Usage

```c
#include "dmvs_js.h"

dmvs_js_error_t error;
dmvs_js_ast_t ast = dmvs_js_parse(source, length, &error);
if (ast == NULL)
{
    Dmod_Printf("%u:%u: %s\n", error.line, error.column, error.message);
    return error.status;
}
for (const dmvs_js_node_t* s = dmvs_js_root(ast)->a; s != NULL; s = s->next)
    ...                                     /* the statements */
dmvs_js_free(ast);
```

`jsdump` prints a script's tree:

```bash
dmod_loader jsdump.dmf --args script.js
# (program [(var const [(declarator el (call (member document getElementById) ["clock"]))])])
```

## Building

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout.

## Testing

```bash
cd build
ctest --output-on-failure
```

The tests check the trees of scripts, errors and where they are, and two
real pages' scripts (`tests/fixtures`) against the trees acorn makes of
them (`*.tree`, written by `tests/acorn/tree.js`). To compare any script
with acorn (needs node and `npm install acorn`):

```bash
BUILD=build tests/acorn/compare.sh some.js
```

## License

MIT - see [LICENSE](LICENSE).

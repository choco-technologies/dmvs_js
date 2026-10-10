# dmvs_js API Reference

```c
#include "dmvs_js.h"
```

| Function | |
|----------|-|
| `dmvs_js_parse(source, length, error)` | Parse a script (UTF-8, not NUL-terminated necessarily) into a tree; NULL on failure, `error` (may be NULL) says why and where: `status` `-EBADMSG` (a syntax error), `-E2BIG` (nested deeper than 64), `-ENOMEM`, `-EINVAL`; `line`, `column` (from 1, in bytes), `offset`, `message` |
| `dmvs_js_root(ast)` | The `DMVS_JS_PROGRAM` node |
| `dmvs_js_free(ast)` | Release the tree and all its nodes and strings |
| `dmvs_js_dump(node, buffer, size)` | The tree of a node as an S-expression (as `snprintf`: returns the length of all of it) |
| `dmvs_js_kind_name(kind)`, `dmvs_js_op_name(op)` | `"call"`, `"??"`, ... |

## Nodes

`dmvs_js_node_t`: `kind`, `op`, `flags`, children `a` ... `d`, `next` (the
next node of a list), `text` / `length` (a name, a string's value - decoded,
UTF-8 - a label, a regular expression as written), `number` / `decimals`
(a number is `number / 10^decimals`), and its source: `start`, `end`
(offsets), `line`, `column`.

| Kind | a | b | c | d |
|------|---|---|---|---|
| `PROGRAM`, `BLOCK` | statements | | | |
| `VAR` (op `VAR_VAR` / `_LET` / `_CONST`) | `DECLARATOR`s (target, initializer) | | | |
| `FUNCTION` (text: name) | parameters | body: `BLOCK`, or an expression (`F_EXPRESSION`) | | |
| `IF` | test | then | else | |
| `FOR` | init | test | update | body |
| `FOR_IN`, `FOR_OF` | left | right | body | |
| `WHILE` / `DO_WHILE` | test / body | body / test | | |
| `SWITCH` / `CASE` | discriminant / test (NULL: default) | `CASE`s / statements | | |
| `TRY` | block | catch parameter | catch block | finally block |
| `RETURN`, `THROW`, `EXPRESSION`, `LABELED` (text) | value / statement | | | |
| `BREAK`, `CONTINUE` (text: label) | | | | |
| `CLASS` (text: name) | superclass | `FIELD`s (key, value - a `FUNCTION` for a method) | | |
| `IDENT`, `NUMBER`, `STRING`, `REGEX`, `BOOL` (op), `NULL`, `THIS`, `HOLE` | | | | |
| `TEMPLATE` | parts: `STRING` chunks and expressions | tag | | |
| `ARRAY` / `OBJECT` | elements / `PROPERTY`s (key, value) and `SPREAD`s | | | |
| `UNARY`, `UPDATE`, `SPREAD`, `AWAIT`, `YIELD` | operand | | | |
| `BINARY`, `LOGICAL`, `ASSIGN` (op) | left / target | right / value | | |
| `CONDITIONAL` | test | consequent | alternate | |
| `CALL`, `NEW` | callee | arguments | | |
| `MEMBER` | object | property (`IDENT`, an expression: `F_COMPUTED`) | | |
| `SEQUENCE` | expressions | | | |

Flags: `F_COMPUTED`, `F_OPTIONAL` (`?.` - on the link that has it),
`F_PREFIX`, `F_SHORTHAND`, `F_METHOD`, `F_GETTER`, `F_SETTER`, `F_ARROW`,
`F_EXPRESSION`, `F_ASYNC`, `F_GENERATOR`, `F_STATIC`, `F_DELEGATE`,
`F_PATTERN` (an array / object that is a destructuring target - a default
in it is an `ASSIGN`, a rest a `SPREAD`), `F_DECLARATION`, `F_INEXACT` (a
number with more digits than `number` holds). `super` and `new.target` are
`IDENT`s; a private name is an `IDENT` `#name`.

## Dump

`(kind [operator] [flags] ["text"] children)`; names, numbers, strings,
`true` / `false` / `null` / `this` as they are; a list of children as
`[ ... ]`; a missing child before others as `_`:

```
(program [(expression (call (member document getElementById) ["clock"]))])
```

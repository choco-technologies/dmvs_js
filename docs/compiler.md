# dmvs_js Compiler

```c
#include "dmvs_js.h"     /* includes dmvsi.h */
```

The compiler turns a page's scripts into the code of a dmvsi document:

- what the scripts do when they load becomes the document's init handler;
- functions become handlers for clicks and timers;
- variables become the view's variables.

todmvs then writes the document as a dmvs view. Nothing interprets
JavaScript on the device.

## How it compiles

The compiler is a partial evaluator. Anything known when the page is
converted is computed at that point. Static values are:

- numbers, strings and booleans;
- arrays and objects;
- functions;
- the host's elements.

Only values that change while the view runs become code: they live in
variables of the document.

- **Static code is computed**:
  - `const items = [3, 4, 5]; items.forEach(x => total += x * 2)` is unrolled.
  - `Math.round(Math.PI * 100) / 100` is `3.14`.
  - A `switch` or `if` on a static value compiles only the branch it takes.
  - A `for` with static bounds is unrolled, unless its body sets the
    loop variable or breaks under a condition. Then it becomes a loop of
    the view.
- **Names**: a pre-scan of all code given so far finds which names are
  ever assigned.
  - A `let` or `var` with such a name is a runtime variable.
  - Any other name is the value it is set to.
- **Numbers**: a name that may hold a fraction is fixed point, at 1/1000
  (`DMVS_JS_SCALE`). Examples are a fraction literal, a division,
  `parseFloat()`, or a parameter given such a value. Every other name is
  an integer. Booleans are 0 or 1. Strings are text variables of 64 bytes.
- **Functions**: a function is compiled once for each set of static
  arguments it is called with, plus its `this`. Each set gets its own
  handler.
  - Runtime arguments are set before the `CALL`.
  - A function that only computes a static value isn't a handler: the
    call is that value.
  - A recursive call (`a → b → a`) compiles another copy of the function,
    up to two levels deep. Deeper calls are reported.
  - `setTimeout(tick, 100)` inside `tick` uses the handler being compiled.
- **Timers**: every `setTimeout` / `setInterval` call site has variables
  for whether it is started and when it is due, plus a view timer that
  polls it. The timer runs every 10–50 ms, depending on the call's delay.
  - The call returns the site's number, so `if (timer)` works.
  - `const t = setInterval(() => { … clearInterval(t) })` works.
  - `clearInterval(t)` of a runtime `t` calls a handler that stops the
    site `t` holds.
- **Texts and numbers**: a runtime text made from a number keeps that
  number (`number_var`). So `parseFloat(el.innerText)` works when the host
  keeps a shadow of the element's number.

What isn't compiled is reported through the host's `report()` with its
line and column, and the rest is compiled. Examples are classes, `new`,
`try`, labels, spreads, and comparisons of runtime texts.

## The language

| | |
|---|---|
| Declarations | `const`, `let`, `var`, functions (hoisted), arrows, default parameters |
| Statements | `if` / `else`, `for`, `while`, `do … while`, `for … of` / `for … in` (unrolled), `switch`, `break` / `continue`, `return` |
| Expressions | arithmetic `+ - * / %` (`**` static only), comparisons, `! && \|\| ??`, `?:`, assignments with an operator, `++` / `--`, template literals, `typeof` |
| Math | `floor`, `round`, `ceil`, `trunc`, `min`, `max`, `abs` at runtime; `PI` |
| Numbers | `parseFloat`, `parseInt`, `Number`, `String`, `toFixed(d)`, `toString()` |
| Strings | `padStart` (a number's runtime text: `'0'` or `' '`). Static only: `toUpperCase`, `toLowerCase`, `trim`, `includes`, `indexOf`, `startsWith`, `endsWith`, `slice`, `substring`, `split`, `padEnd`, `repeat`, `charAt` |
| Arrays | `forEach`, `map`, `join`, `includes`, `indexOf`, `filter` / `find` / `some` / `every` (static tests). `a[i]` with a runtime `i` picks a number or a text |
| Timers | `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` |
| Other | `console.*` does nothing |

## The host

The DOM belongs to the host (dmvs_html). The compiler asks the host for
globals it doesn't know, such as `document`. When a script uses the
host's objects, the compiler passes the operation to the host, and the
host emits the actions that do it.

```c
typedef struct
{
    void*   ctx;
    bool    (*global)(void* ctx, dmvs_js_compiler_t c, const char* name, dmvs_js_value_t* value);
    int     (*get)(void* ctx, dmvs_js_compiler_t c, uint32_t object, const char* name, dmvs_js_value_t* value);
    int     (*set)(void* ctx, dmvs_js_compiler_t c, uint32_t object, const char* name, const dmvs_js_value_t* value);
    int     (*call)(void* ctx, dmvs_js_compiler_t c, uint32_t object, const char* method,
                    const dmvs_js_value_t* args, uint32_t count, dmvs_js_value_t* result);
    void    (*report)(void* ctx, uint32_t line, uint32_t column, const char* message);
} dmvs_js_host_t;
```

- An object is a `DMVS_JS_V_OBJECT` value with the host's handle, for
  example an element returned by `getElementById()`.
- `get`, `set` and `call` return 0, or `-ENOTSUP` for something the host
  doesn't do (the compiler reports it).
- While handling one of them, the host may emit actions with
  `dmvs_js_emit()`. It turns values into operands with
  `dmvs_js_number_operand()` and `dmvs_js_text_operand()`.
- A listener (`addEventListener('click', fn)`) becomes a handler through
  `dmvs_js_function_handler(c, &fn, element)`.
- A `DMVS_JS_V_RUNTIME` value is a variable of the document:
  - `DMVS_JS_T_NUMBER` at `scale` 0 or `DMVS_JS_SCALE`;
  - `DMVS_JS_T_BOOL`;
  - `DMVS_JS_T_TEXT`, where `number_var` is the number the text was made
    from (0 if none).

## Use

```c
dmvs_js_compiler_t c = dmvs_js_compiler_new(doc, &host);

/* onclick="..." code first: what it assigns is a variable, for the scripts too */
dmvs_js_ast_t click = dmvs_js_parse(onclick, strlen(onclick), &error);
dmvs_js_scan(c, click);

dmvs_js_compile(c, dmvs_js_parse(script, strlen(script), &error));   /* each script, in order */
dmvsi_handler_t h = dmvs_js_compile_handler(c, click, button);         /* `this`: the button */

dmvs_js_finish(c);           /* the init handler, the timers */
dmvs_js_compiler_free(c);    /* and the trees it was given */
```

The compiler keeps every tree it is given until it is freed: functions are
compiled when they are used, which may be long after their script was.
`dmvs_js_reports()` gives the number of reports.

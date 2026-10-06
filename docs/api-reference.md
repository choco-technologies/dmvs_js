# dmvs_js API Reference

The public compiler API is declared in [include/dmvs_js.h](../include/dmvs_js.h)
using DMOD API macros. **The compiler functions are not implemented yet.**
The types and contracts below define the interface for subsequent implementation.
The existing scaffold lifecycle (`create`, `destroy`, `is_valid`) remains
available through `dmvs_js_scaffold.h`, also included by `dmvs_js.h`;
scaffold handles are distinct from compiled program handles. The existing
implementation and its tests include only the scaffold header so they do not
register or import compiler functions that have no implementation yet.

## First milestone

Compile a small UI script to a typed program that a `dmvsi` / `todmvs`
adapter can lower to DMVS. The first end-to-end example is a button updating
a counter's text, bounded so that integer arithmetic has defined semantics:

```html
<button id="increment">+</button><span id="count">0</span>
<script>
let count = 0;
const label = document.getElementById('count');
document.getElementById('increment').addEventListener('click', () => {
    if (count < 100) count = count + 1;
    label.textContent = count;
});
</script>
```

Initially support classic scripts, `let` / `const`, integer / boolean /
string literals, assignments, integer `+` / `-`, integer comparisons,
boolean conditions, `if` / `else`, statically resolved DOM queries, click
listeners, inline `onclick`, `textContent`, and class add/remove/toggle.
Capture resolved element references and shared scalar variables for click
listeners. No arbitrary closure allocation or JavaScript execution engine
is required for this milestone.

JavaScript Number is not int32. Accept numeric operations only when range
analysis proves their values remain representable signed integers on every
reachable path; otherwise return UNSUPPORTED. Do not silently wrap, truncate
fractions, or substitute integer semantics for JavaScript semantics. The
bounded counter above is the numeric acceptance example. Boolean conditions
are typed; general coercions, mixed-type equality and string arithmetic are
outside this initial subset. Numeric text assignments lower to INT_TO_STRING.

Timers, Date, floating point, loops, modules, arbitrary functions, dynamic
selectors, DOM creation, innerHTML, and CSS style assignments are later
milestones. Unsupported constructs are errors, never silently omitted.
This first interface does **not** promise to compile the full car HMI.

## Three operations

| Operation | Contract |
|---|---|
| `dmvs_js_compile(request, &program)` | Synchronously compile an ordered batch of sources against a read-only host DOM. |
| `dmvs_js_program_view(program, &view)` | Borrow the immutable declarations, instructions, bindings and handlers of a successful result. |
| `dmvs_js_program_destroy(program)` | Release the result, including all owned strings and arrays; NULL is allowed. |

There is no persistent compiler session or global DOM. One batch gives
classic scripts their shared global scope; separate calls are independent.
Sources contain bytes rather than paths, so file access, URL mapping and
script extraction stay with the caller. Source order is document order.
Inline click sources are handler bodies, not top-level scripts: `this_node`
provides their element, and execution occurs only on a click.

The host supplies SCRIPT sources only when their extraction and execution
ordering fit this contract. `type="module"`, `async`, `defer` semantics and
parser-blocking DOM construction are not implied. The initial profile sees
the completed DOM, suitable for scripts at the end of the page. Scripts
whose behavior depends on a partially constructed DOM must be rejected by
the HTML integration, not silently reordered. Recognized Tailwind config
is handled by `dmvs_html` before the compiler, not accepted as general JS.

## DOM adapter

`dmvs_js_host_t` has two synchronous callbacks:

- `query`: resolve an ID or CSS selector, emitting unique stable node IDs
  in document order. An empty result is successful. ID queries emit at most
  one match. `querySelector` uses the first match; `querySelectorAll` can be
  resolved for later static expansion but general iteration is not promised
  in the first milestone.
- `read`: obtain an initial TEXT string or CLASS boolean for a known node.
  TEXT means `textContent`; CLASS names one class token. Unknown nodes or
  unsupported queries/properties return an error.

`dmvs_html` implements these callbacks using its existing DOM and selector
machinery. It retains ownership of nodes. IDs are nonzero, stable throughout
compilation and binding application, and never C pointers. Query callbacks
must propagate the collector's error and must not retain or asynchronously
invoke it. Compilation snapshots read values; host reads are not performed
by the generated program. Once a property is bound, compiled reads use its
variable so that handlers observe previous writes.

The DOM must not change during compilation. The compiler never invokes a
DOM mutation, layout or paint callback. Result bindings describe which
variable controls an element's text or class. There is at most one binding
per `(node, kind, name)`. Mutations from outside the generated program are
not supported by this initial contract.

## Result and backend contract

The program view exposes a small semantic instruction set rather than an
AST or raw assembly. This keeps Tree-sitter out of both the ABI and backend.
All indices are zero-based; node ID zero alone is reserved. Empty arrays
have count zero and may have NULL pointers. The compiler validates every
index, operand type and handler range before returning success.

Variables are typed and have matching initial values. BOOL starts as false
unless specified otherwise; INT starts as zero; STRING starts empty. These
initial values are storage initialization, not a replacement for ordered
script execution. Explicit initializers and top-level property writes run
in the single optional INIT handler, in source order, before first drawing.
Reading a lexical variable before initialization is a compilation error in
this profile. Every string has an explicit capacity; all reachable values
must fit. Strings and variables cannot be truncated silently.

Instructions use only the fields documented by their opcode. Unused fields
are zero-initialized and ignored. SET preserves type. ADD/SUB take INTs and
produce INT; EQ takes two INTs or two BOOLs and produces BOOL; LT takes INTs
and produces BOOL; INT_TO_STRING takes INT and produces STRING. JUMP_FALSE
takes BOOL. A backend may represent BOOL as an integer containing 0 or 1.

Each handler owns a disjoint nonempty instruction range. Jumps stay inside
that range; every reachable path ends in RETURN. This initial profile emits
no backward jumps. There is at most one CLICK handler per node: multiple
listeners are combined in registration order. Mixing an inline handler and
registered listeners on the same node is initially rejected until their
ordering semantics are implemented. Events are serialized, without bubbling
or a JavaScript Event object in this first profile. Initialization may
register listeners, but handlers cannot dynamically register more listeners.

The HTML/backend integration will:

1. Resolve all binding IDs against the same DOM and check supported effects.
2. Map text bindings to dynamic text and reserve font glyphs for possible
   values (digits and minus for integer text, plus known string literals).
3. Map class bindings to computed style/layout variants, with an explicit
   variant budget; reject effects that the target cannot represent.
4. Lower variables, INIT, CLICK and instructions to an extended `dmvsi`
   program representation, then serialize it through `libtodmvs`.

Current `dmvsi` does not express all these operations or dynamic text. This
adapter and the required `dmvsi` extension are a **follow-up**, not an API
claimed to exist today. No raw DMVS snippets are appended to a document.
Validate and stage the entire conversion before publishing output, so an
unsupported binding does not leave a partially converted page. Styling
variants and layout remain `dmvs_html` responsibilities.

## Ownership, errors and embedded constraints

- Input source descriptors, their bytes, the host and its user context are
  borrowed until `compile` returns. They can then be released. The compiler
  copies all strings needed by the successful result.
- A string returned by `read` stays valid until the next host callback or
  return from `compile`, whichever comes first. Diagnostics and their
  location/message are valid only during the diagnostic callback.
- Program-view arrays and strings are borrowed until `program_destroy`.
  They must not be changed or freed individually. The view getter allocates
  nothing; NULL program/view arguments return INVALID_ARGUMENT.
- `compile` sets a non-NULL output pointer to NULL before work. On any error
  it frees its own intermediate allocations and returns no partial result.
  It never retains the host. Host errors become HOST_ERROR, except compiler
  collector errors which preserve their original status. Diagnostics include
  the originating host status in the message.
- Diagnostic locations use source-relative UTF-8 byte offsets, not character
  columns. A NULL location denotes an error without a source location. The
  caller maps fragment offsets to HTML/file lines. No diagnostic callback
  is required to obtain a useful status code.
- All limits must be nonzero; zero is invalid, never unlimited. Counts and
  aggregate source sizes are checked with overflow-safe arithmetic. No
  hidden desktop defaults are part of this interface.
- Compiler-managed memory includes parser allocations, work buffers and the
  result during compilation. Caller-owned sources and host DOM storage are
  excluded. Backend layout, variant generation and asset conversion need
  their own budgets. The complete pipeline must measure all of them.
- Allocation uses DMOD SAL internally. Exhausted configured budgets return
  LIMIT_EXCEEDED; allocator failure returns OUT_OF_MEMORY. No filesystem,
  network, process, thread or clock API is needed by this compiler interface.
- A Tree-sitter integration must prove safe OOM behavior and DMOD loading
  before implementation can claim these guarantees. Installing an allocator
  hook alone is not proof. The parser choice remains an implementation detail.
- Calls are synchronous. Initially the caller serializes compilations and
  must not re-enter compilation from host/diagnostic callbacks. Independent
  completed results remain valid across subsequent compilations.

Request and host structures require the declared ABI version and matching
structure size; unknown versions/sizes return INVALID_ARGUMENT. The result
view carries the same information for the backend to check. This is not a
serialized wire format or a promise of future binary-compatible extension.
The three compiler functions are declared with `dmod_dmvs_js_api(...)`.
Their future definitions will use `dmod_dmvs_js_api_declaration(...)`.

## Caller lifecycle (illustrative C)

```c
dmvs_js_source_t source = {
    .kind = DMVS_JS_SCRIPT,
    .name = { "page.html#script-1", sizeof("page.html#script-1") - 1 },
    .text = { script_bytes, script_size },
    .this_node = 0
};
dmvs_js_request_t request = {
    .abi_version = DMVS_JS_ABI_VERSION,
    .struct_size = sizeof(dmvs_js_request_t),
    .sources = &source,
    .source_count = 1,
    .host = &html_host,       /* Versioned adapter initialized by the caller. */
    .limits = board_limits  /* Every limit explicitly supplied by the caller. */
};
dmvs_js_program_t program = NULL;
dmvs_js_status_t status = dmvs_js_compile(&request, &program);
if (status == DMVS_JS_OK) {
    const dmvs_js_program_view_t* view = NULL;
    status = dmvs_js_program_view(program, &view);
    if (status == DMVS_JS_OK) {
        /* Inspect/validate view; future HTML + dmvsi adapter consumes it here.
         * That adapter must copy anything it retains beyond this block. */
    }
}
dmvs_js_program_destroy(program);
```

## Follow-up validation

Implementation acceptance tests should cover the bounded counter through
DMVS assembly and execution, shared scope across script fragments, inline
`this`, listener ordering, one binding shared by multiple handlers, syntax
and unsupported-feature locations, every limit, failed host callbacks,
allocation failure cleanup and result lifetime after freeing input bytes.
An embedded test must use the real DMOD loader as well as measure memory.
Until the compiler is implemented, its interface can be checked through
C/C++ compilation and the documented caller example. Existing runtime tests
exercise only the scaffold lifecycle, not the compiler declarations.

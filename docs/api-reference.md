# dmvs_js API

The compiler API is declared but not yet implemented.

| Header | Contents |
| --- | --- |
| [dmvs_js.h](../include/dmvs_js.h) | DMOD function declarations and their contracts |
| [dmvs_js_types.h](../include/dmvs_js_types.h) | Source descriptors, DOM callbacks, limits and compiled program types |

| Function | Purpose |
| --- | --- |
| `dmvs_js_compile(request, &program)` | Compile sources against an optional read-only DOM adapter. |
| `dmvs_js_program_view(program, &view)` | Borrow immutable variables, instructions, bindings and handlers. |
| `dmvs_js_program_destroy(program)` | Release the program and its owned data. |

`dmvs_js_request_t` supplies sources, resource limits and optional diagnostics.
`dmvs_js_host_t` lets `dmvs_html` resolve nodes and read initial properties.
The result describes behavior and UI bindings for a backend to translate to DMVS.

Parameter requirements, error codes, callback rules and ownership are documented
in the headers with Doxygen.

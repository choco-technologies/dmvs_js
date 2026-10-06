# dmvs_js

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmvs_js/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmvs_js/actions/workflows/ci.yml)

JavaScript compiler API for DMVS, with a DOM adapter for `dmvs_html`.
The compiler functions are declared but not yet implemented.

## API

- [dmvs_js.h](include/dmvs_js.h): functions with Doxygen contracts.
- [dmvs_js_types.h](include/dmvs_js_types.h): types and callbacks with Doxygen documentation.
- [API reference](docs/api-reference.md): interface overview.

## Building

```bash
cmake -S . -B build -DDMOD_DIR=/path/to/dmod
cmake --build build
```

Omit `DMOD_DIR` to fetch dmod from GitHub. The module currently contains only
DMOD lifecycle hooks; it does not export implementations of the compiler API.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

The API test stubs are built but disabled in CTest until implemented. Running
the test module directly reports explicit TODO failures, not passing tests.

## License

MIT — see [LICENSE](LICENSE).

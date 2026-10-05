#ifndef DMVS_JS_H
#define DMVS_JS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmvs_js_defs.h"

/**
 * Public API for the dmvs_js module.
 *
 * Functions are declared with the dmod_dmvs_js_api(...) macro - dmod's
 * standard pattern for functions callable from other modules (or from this
 * module's own tests/), resolved dynamically by the loader rather than
 * through normal static linkage. See dm_sw_ring/include/dm_sw_ring.h for a
 * fully worked real-world example of the same shape.
 *
 * Definitions in src/dmvs_js.c use the matching
 * dmod_dmvs_js_api_declaration(...) macro - a plain C function
 * definition here will NOT satisfy these declarations at link time.
 *
 * This is an example interface using the usual "opaque handle" pattern -
 * replace the handle, functions, and struct definition in
 * src/dmvs_js.c with your module's real API.
 */

/* Opaque handle - the real struct is defined in src/dmvs_js.c */
typedef struct dmvs_js* dmvs_js_t;

/**
 * Create a new dmvs_js instance.
 *
 * @return A valid handle on success, or NULL on allocation failure.
 */
dmod_dmvs_js_api(1.0, dmvs_js_t, _create, ( void ));

/**
 * Destroy an instance created by dmvs_js_create(). Safe to call with
 * NULL.
 */
dmod_dmvs_js_api(1.0, void, _destroy, ( dmvs_js_t handle ));

/**
 * Example accessor - replace with your module's real API.
 *
 * @return true if handle is a valid, non-NULL instance.
 */
dmod_dmvs_js_api(1.0, bool, _is_valid, ( dmvs_js_t handle ));

#endif // DMVS_JS_H

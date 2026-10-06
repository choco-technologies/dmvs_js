/* Existing scaffold API, separate from the pending compiler implementation. */
#ifndef DMVS_JS_SCAFFOLD_H
#define DMVS_JS_SCAFFOLD_H

#include <stdbool.h>
#include "dmod_types.h"
#include "dmvs_js_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Existing scaffold lifecycle, retained until the compiler is implemented.
 * A scaffold instance is not a compiled program. */
typedef struct dmvs_js* dmvs_js_t;
dmod_dmvs_js_api(1.0, dmvs_js_t, _create, ( void ));
dmod_dmvs_js_api(1.0, void, _destroy, ( dmvs_js_t handle ));
dmod_dmvs_js_api(1.0, bool, _is_valid, ( dmvs_js_t handle ));

#ifdef __cplusplus
}
#endif
#endif

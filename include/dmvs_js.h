/**
 * @file dmvs_js.h
 * @brief Public JavaScript compiler API.
 *
 * The functions below are declarations only; no compiler implementation is
 * provided yet. Types, callbacks and ownership rules are in dmvs_js_types.h.
 */
#ifndef DMVS_JS_H
#define DMVS_JS_H

#include "dmvs_js_types.h"
#include "dmod_types.h"
#include "dmvs_js_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compile JavaScript sources into an owned, immutable program.
 *
 * Processes classic scripts in array order with a shared global scope. Inline
 * click sources become event handlers rather than executing at initialization.
 * DOM queries and initial property reads use the supplied host adapter; this
 * function neither mutates the DOM nor executes the resulting program.
 *
 * @param[in] request Compilation inputs and limits. Must not be NULL. All
 * referenced source bytes, host data and callback contexts must remain valid
 * and unchanged until this call returns. See dmvs_js_request_t for validation.
 * @param[out] program Required output address. Set to NULL before processing.
 * On success, receives an owned handle to release with dmvs_js_program_destroy().
 * The result retains no pointers into the request or host-owned memory.
 *
 * @retval DMVS_JS_OK Compilation succeeded and *program is non-NULL.
 * @retval DMVS_JS_INVALID_ARGUMENT NULL argument, invalid descriptor, zero
 * limit, or unsupported ABI version/structure size.
 * @retval DMVS_JS_OUT_OF_MEMORY The allocator could not satisfy an allocation.
 * @retval DMVS_JS_LIMIT_EXCEEDED A configured resource budget was exceeded.
 * @retval DMVS_JS_SYNTAX_ERROR A source contains invalid JavaScript syntax.
 * @retval DMVS_JS_UNSUPPORTED A construct or value cannot be represented by
 * the compiler's supported semantics; no code is silently omitted.
 * @retval DMVS_JS_HOST_ERROR A host query or property read failed.
 *
 * @note On failure, all intermediate allocations are released and no partial
 * program is returned. If program itself is NULL, it cannot be initialized.
 * Diagnostics are optional and do not replace the returned status.
 * @warning Calls are synchronous and must be serialized by the caller. Host
 * and diagnostic callbacks must not re-enter compilation. Caller-owned DOM
 * storage is outside the compiler's memory budget.
 * @see dmvs_js_host_t, dmvs_js_limits_t, dmvs_js_diagnostic_fn
 */
dmod_dmvs_js_api(1.0, dmvs_js_status_t, _compile, (
    const dmvs_js_request_t* request, dmvs_js_program_t* program ));

/**
 * @brief Borrow the declarations, instructions and bindings of a program.
 *
 * Provides read-only access without allocating or copying data. The returned
 * structure and every array/string reachable from it belong to program.
 * A backend must copy any data it needs after the program is destroyed.
 *
 * @param[in] program Live handle returned by a successful dmvs_js_compile().
 * @param[out] view Required output address. Initialized to NULL, then set to
 * the immutable program view on success.
 * @retval DMVS_JS_OK The view is available and *view is non-NULL.
 * @retval DMVS_JS_INVALID_ARGUMENT program or view is NULL.
 *
 * @warning The caller must not modify or free the view or its members.
 * Destroying program invalidates all borrowed pointers. A non-NULL stale or
 * foreign handle is invalid usage and is not guaranteed to be detected.
 * @see dmvs_js_program_view_t, dmvs_js_program_destroy
 */
dmod_dmvs_js_api(1.0, dmvs_js_status_t, _program_view, (
    dmvs_js_program_t program, const dmvs_js_program_view_t** view ));

/**
 * @brief Release a compiled program and all memory it owns.
 *
 * Frees the result's declarations, code, bindings and copied strings. It does
 * not release caller-owned source buffers, host contexts or DOM nodes.
 *
 * @param[in] program Handle returned by dmvs_js_compile(), or NULL for a no-op.
 * @warning After this call the handle and all borrowed views are invalid.
 * Destroy each non-NULL handle exactly once. The caller must ensure that no
 * other code is reading the program while it is being destroyed.
 * @see dmvs_js_program_view
 */
dmod_dmvs_js_api(1.0, void, _program_destroy, ( dmvs_js_program_t program ));

#ifdef __cplusplus
}
#endif
#endif

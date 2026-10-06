/* Review-only proposal. Not an installed header or an implemented API.
 * See api-proposal.md for semantics, ownership and the initial JS subset. */
#ifndef DMVS_JS_PROPOSED_H
#define DMVS_JS_PROPOSED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMVS_JS_PROPOSAL_ABI 1u

typedef struct dmvs_js_program* dmvs_js_program_t;
typedef uint32_t dmvs_js_node_t; /* Host-owned stable ID; 0 means no node. */

typedef struct {
    const char* data;
    size_t size;                 /* UTF-8 bytes, excludes any trailing NUL. */
} dmvs_js_string_t;

typedef enum {
    DMVS_JS_OK = 0,
    DMVS_JS_INVALID_ARGUMENT = -1,
    DMVS_JS_OUT_OF_MEMORY = -2,
    DMVS_JS_LIMIT_EXCEEDED = -3,
    DMVS_JS_SYNTAX_ERROR = -4,
    DMVS_JS_UNSUPPORTED = -5,
    DMVS_JS_HOST_ERROR = -6
} dmvs_js_status_t;

typedef struct {
    uint32_t source;             /* Zero-based index in request.sources. */
    size_t begin, end;           /* Half-open byte offsets within that source. */
} dmvs_js_location_t;

typedef enum { DMVS_JS_SCRIPT, DMVS_JS_INLINE_CLICK } dmvs_js_source_kind_t;
typedef struct {
    dmvs_js_source_kind_t kind;
    dmvs_js_string_t name;       /* Diagnostic identity, not a path to open. */
    dmvs_js_string_t text;
    dmvs_js_node_t this_node;    /* Required for INLINE_CLICK; 0 for SCRIPT. */
} dmvs_js_source_t;

typedef enum { DMVS_JS_INT, DMVS_JS_BOOL, DMVS_JS_STRING } dmvs_js_type_t;
typedef struct {
    dmvs_js_type_t type;
    union {
        int32_t integer;
        bool boolean;
        dmvs_js_string_t string;
    } as;
} dmvs_js_value_t;

typedef enum { DMVS_JS_QUERY_ID, DMVS_JS_QUERY_CSS } dmvs_js_query_kind_t;
typedef enum { DMVS_JS_TEXT, DMVS_JS_CLASS } dmvs_js_property_kind_t;
typedef struct {
    dmvs_js_node_t node;
    dmvs_js_property_kind_t kind;
    dmvs_js_string_t name;       /* Class token for CLASS; empty for TEXT. */
} dmvs_js_property_t;

/* emit is synchronous. Stop immediately if it returns anything but OK. */
typedef dmvs_js_status_t (*dmvs_js_match_fn)(void* user, dmvs_js_node_t node);
typedef struct {
    uint32_t abi_version;
    size_t struct_size;
    void* user;
    dmvs_js_status_t (*query)(void* user, dmvs_js_query_kind_t kind,
        dmvs_js_string_t selector, dmvs_js_match_fn emit, void* emit_user);
    dmvs_js_status_t (*read)(void* user, const dmvs_js_property_t* property,
        dmvs_js_value_t* value);
} dmvs_js_host_t;

typedef void (*dmvs_js_diagnostic_fn)(void* user, dmvs_js_status_t status,
    const dmvs_js_location_t* location, dmvs_js_string_t message);

typedef struct {
    size_t max_memory_bytes;     /* Total compiler-managed live allocations. */
    size_t max_source_bytes;     /* Sum of source text sizes. */
    uint32_t max_sources;
    uint32_t max_nesting;
    uint32_t max_nodes;          /* Distinct node IDs resolved from the host. */
    uint32_t max_variables;
    uint32_t max_instructions;
    uint32_t max_handlers;
    uint32_t max_bindings;
    uint32_t max_string_bytes;   /* Per string, excludes trailing NUL. */
} dmvs_js_limits_t;

typedef struct {
    uint32_t abi_version;
    size_t struct_size;
    const dmvs_js_source_t* sources;
    uint32_t source_count;
    const dmvs_js_host_t* host;  /* NULL for scripts that do not use the DOM. */
    dmvs_js_limits_t limits;
    dmvs_js_diagnostic_fn diagnostic; /* Optional. */
    void* diagnostic_user;
} dmvs_js_request_t;

/* A compact, typed program for a backend; not a Tree-sitter tree or DMVS text.
 * Variable IDs and instruction indices below are zero-based. */
typedef struct {
    dmvs_js_type_t type;
    dmvs_js_value_t initial;
    uint32_t string_capacity;    /* STRING only; bytes excluding trailing NUL. */
} dmvs_js_variable_t;

typedef enum { DMVS_JS_LITERAL, DMVS_JS_VARIABLE } dmvs_js_operand_kind_t;
typedef struct {
    dmvs_js_operand_kind_t kind;
    union {
        dmvs_js_value_t literal;
        uint32_t variable;
    } as;
} dmvs_js_operand_t;

typedef enum {
    DMVS_JS_OP_SET,              /* destination = left */
    DMVS_JS_OP_ADD,              /* destination = left + right (INT) */
    DMVS_JS_OP_SUB,              /* destination = left - right (INT) */
    DMVS_JS_OP_EQ,               /* destination = left === right (INT/BOOL) */
    DMVS_JS_OP_LT,               /* destination = left < right (INT) */
    DMVS_JS_OP_INT_TO_STRING,    /* destination = decimal(left) */
    DMVS_JS_OP_JUMP,             /* Continue at target. */
    DMVS_JS_OP_JUMP_FALSE,       /* Continue at target when left is false. */
    DMVS_JS_OP_RETURN            /* End this handler. */
} dmvs_js_opcode_t;

typedef struct {
    dmvs_js_opcode_t opcode;
    uint32_t destination;
    dmvs_js_operand_t left, right;
    uint32_t target;
    dmvs_js_location_t location;
} dmvs_js_instruction_t;

typedef struct {
    dmvs_js_property_t property;
    uint32_t variable;           /* STRING for TEXT; BOOL for CLASS. */
} dmvs_js_binding_t;

typedef enum { DMVS_JS_INIT, DMVS_JS_CLICK } dmvs_js_event_t;
typedef struct {
    dmvs_js_event_t event;
    dmvs_js_node_t node;         /* 0 for INIT. */
    uint32_t entry, count;       /* Instruction range [entry, entry + count). */
} dmvs_js_handler_t;

typedef struct {
    uint32_t abi_version;
    size_t struct_size;
    const dmvs_js_variable_t* variables;
    uint32_t variable_count;
    const dmvs_js_instruction_t* instructions;
    uint32_t instruction_count;
    const dmvs_js_binding_t* bindings;
    uint32_t binding_count;
    const dmvs_js_handler_t* handlers;
    uint32_t handler_count;
} dmvs_js_program_view_t;

/* Plain C signatures for review. Implementation will export these through
 * dmod_dmvs_js_api(...) / dmod_dmvs_js_api_declaration(...). */
dmvs_js_status_t dmvs_js_compile(const dmvs_js_request_t* request,
    dmvs_js_program_t* program);
dmvs_js_status_t dmvs_js_program_view(dmvs_js_program_t program,
    const dmvs_js_program_view_t** view);
void dmvs_js_program_destroy(dmvs_js_program_t program);

#ifdef __cplusplus
}
#endif
#endif

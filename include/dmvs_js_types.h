/**
 * @file dmvs_js_types.h
 * @brief Sources, DOM callbacks and typed output of the JavaScript compiler.
 *
 * This header has no DMOD dependency. Structures are in-process C interfaces,
 * not serialized data. Variable IDs and instruction indices are zero-based;
 * DOM node IDs are host-defined and nonzero.
 */
#ifndef DMVS_JS_TYPES_H
#define DMVS_JS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ABI version required in request, host and program-view descriptors. */
#define DMVS_JS_ABI_VERSION 1u

/**
 * @brief Opaque owner of a compiled program.
 * Obtained from dmvs_js_compile() and released by dmvs_js_program_destroy().
 * NULL represents no program; independent handles own independent results.
 */
typedef struct dmvs_js_program* dmvs_js_program_t;

/**
 * @brief Host-assigned DOM identity, not an address.
 * Zero means no node. IDs must remain stable during compilation and until the
 * caller has resolved the returned bindings against the original DOM.
 */
typedef uint32_t dmvs_js_node_t;

/**
 * @brief Length-delimited UTF-8 string; NUL termination is not required.
 * This descriptor does not transfer ownership. Input strings are borrowed for
 * compilation; strings in a program view remain valid until program destruction.
 */
typedef struct {
    const char* data; /**< Bytes; may be NULL only when size is zero. */
    size_t size;     /**< Byte count, excluding any trailing NUL. */
} dmvs_js_string_t;

/** @brief Result of an API operation or synchronous callback. */
typedef enum {
    DMVS_JS_OK = 0,                /**< Operation succeeded. */
    DMVS_JS_INVALID_ARGUMENT = -1, /**< Invalid argument, descriptor or ABI. */
    DMVS_JS_OUT_OF_MEMORY = -2,    /**< Allocator failure. */
    DMVS_JS_LIMIT_EXCEEDED = -3,   /**< Configured resource budget exceeded. */
    DMVS_JS_SYNTAX_ERROR = -4,     /**< Invalid source syntax. */
    DMVS_JS_UNSUPPORTED = -5,      /**< Semantics cannot be compiled faithfully. */
    DMVS_JS_HOST_ERROR = -6        /**< Host DOM operation failed. */
} dmvs_js_status_t;

/**
 * @brief Source-relative byte range used by diagnostics and instructions.
 * The range is [begin, end), with begin <= end <= source.text.size. Offsets are
 * UTF-8 byte positions, not character columns. The caller maps them to file or
 * HTML fragment locations. A zero-length range may identify an insertion point.
 */
typedef struct {
    uint32_t source; /**< Index into dmvs_js_request_t.sources. */
    size_t begin;   /**< Inclusive start offset. */
    size_t end;     /**< Exclusive end offset. */
} dmvs_js_location_t;

/** @brief How a source fragment participates in compilation. */
typedef enum {
    DMVS_JS_SCRIPT,      /**< Classic script sharing the batch's global scope. */
    DMVS_JS_INLINE_CLICK /**< onclick body; this refers to source.this_node. */
} dmvs_js_source_kind_t;

/**
 * @brief Caller-provided script fragment.
 * Sources and bytes are borrowed until compilation returns. Names are used
 * only for diagnostics: the compiler does not open files or fetch URLs.
 */
typedef struct {
    dmvs_js_source_kind_t kind; /**< Script or inline click handler. */
    dmvs_js_string_t name;      /**< Diagnostic identity; may be empty. */
    dmvs_js_string_t text;      /**< JavaScript source bytes. */
    dmvs_js_node_t this_node;   /**< Nonzero for INLINE_CLICK; zero for SCRIPT. */
} dmvs_js_source_t;

/**
 * @brief Scalar representation used by the compiler output.
 * INT describes an int32 representation, not the complete JavaScript Number
 * type. A compiler must reject operations whose semantics it cannot preserve.
 */
typedef enum {
    DMVS_JS_INT,   /**< Signed 32-bit integer. */
    DMVS_JS_BOOL,  /**< Logical false or true. */
    DMVS_JS_STRING /**< UTF-8 byte string. */
} dmvs_js_type_t;

/** @brief Tagged scalar value; only the union member selected by type is valid. */
typedef struct {
    dmvs_js_type_t type; /**< Discriminator for as. */
    union {
        int32_t integer;         /**< Value when type is DMVS_JS_INT. */
        bool boolean;            /**< Value when type is DMVS_JS_BOOL. */
        dmvs_js_string_t string; /**< Value when type is DMVS_JS_STRING. */
    } as; /**< Payload; string ownership follows its containing descriptor. */
} dmvs_js_value_t;

/** @brief Query passed to the caller's DOM adapter. */
typedef enum {
    DMVS_JS_QUERY_ID, /**< Literal element ID, without a leading '#'. */
    DMVS_JS_QUERY_CSS /**< CSS selector evaluated by the host. */
} dmvs_js_query_kind_t;

/** @brief DOM property readable at compilation and bindable to program state. */
typedef enum {
    DMVS_JS_TEXT, /**< textContent; value type is STRING. */
    DMVS_JS_CLASS /**< Membership of one class token; value type is BOOL. */
} dmvs_js_property_kind_t;

/** @brief Property key; (node, kind, name) uniquely identifies a binding. */
typedef struct {
    dmvs_js_node_t node;        /**< Nonzero host node ID. */
    dmvs_js_property_kind_t kind; /**< Property to read or bind. */
    dmvs_js_string_t name;      /**< Nonempty class token for CLASS; empty for TEXT. */
} dmvs_js_property_t;

/**
 * @brief Collect one node matched by a host query.
 * @param[in] user Compiler context supplied to the query as emit_user.
 * @param[in] node Nonzero matching node ID, emitted once per query.
 * @return DMVS_JS_OK to continue, or an error that the host must immediately
 * return unchanged. No more matches may be emitted after an error.
 * @warning Valid only during the query callback; do not retain or call later.
 */
typedef dmvs_js_status_t (*dmvs_js_match_fn)(void* user, dmvs_js_node_t node);

/**
 * @brief Resolve a selector against the host's stable DOM.
 * @param[in] user Host context from dmvs_js_host_t.user.
 * @param[in] kind ID or CSS query.
 * @param[in] selector Borrowed selector bytes, valid only for this callback.
 * @param[in] emit Compiler collector to call synchronously for each match.
 * @param[in] emit_user Opaque collector context; forward unchanged to emit.
 * @return DMVS_JS_OK, including when no nodes match; otherwise a host error
 * or the exact error returned by emit. Compiler collector errors keep their
 * status; other query errors become DMVS_JS_HOST_ERROR in dmvs_js_compile().
 * @note Emit unique nodes in document order, at most one for an ID query.
 * Unsupported selector syntax must return an error, not an empty match set.
 */
typedef dmvs_js_status_t (*dmvs_js_query_fn)(void* user,
    dmvs_js_query_kind_t kind, dmvs_js_string_t selector,
    dmvs_js_match_fn emit, void* emit_user);

/**
 * @brief Read an initial property value from the host DOM.
 * @param[in] user Host context from dmvs_js_host_t.user.
 * @param[in] property Borrowed key valid for this callback; never NULL.
 * @param[out] value Required output: STRING for TEXT, BOOL for CLASS.
 * @return DMVS_JS_OK when value is initialized; otherwise a host error, mapped
 * to DMVS_JS_HOST_ERROR by dmvs_js_compile(). Unknown nodes are errors.
 * @note Returned string bytes must remain valid until the next host callback
 * or until compilation returns, whichever occurs first. The compiler copies
 * retained data. Absent classes read as false; empty text reads as an empty
 * string. The callback must not mutate the DOM.
 */
typedef dmvs_js_status_t (*dmvs_js_read_fn)(void* user,
    const dmvs_js_property_t* property, dmvs_js_value_t* value);

/**
 * @brief Read-only bridge to the caller's DOM, e.g. dmvs_html.
 * Both callbacks are required when a host is supplied. All calls are
 * synchronous during compilation; no host callback is retained in the result.
 * The DOM must remain unchanged throughout compilation.
 */
typedef struct {
    uint32_t abi_version;   /**< Must equal DMVS_JS_ABI_VERSION. */
    size_t struct_size;     /**< Must equal sizeof(dmvs_js_host_t). */
    void* user;            /**< Caller-owned callback context; may be NULL. */
    dmvs_js_query_fn query; /**< Resolve nodes without exposing DOM pointers. */
    dmvs_js_read_fn read;   /**< Read initial text or class membership. */
} dmvs_js_host_t;

/**
 * @brief Report a compilation diagnostic synchronously.
 * @param[in] user Caller context from dmvs_js_request_t.diagnostic_user.
 * @param[in] status Error category; host diagnostics also describe host status.
 * @param[in] location Source range, or NULL for a non-source-specific error.
 * @param[in] message Human-readable UTF-8 message, not necessarily terminated.
 * @note Location and message are borrowed only for the callback duration.
 * Copy them if needed later. The callback cannot cancel or re-enter compilation.
 */
typedef void (*dmvs_js_diagnostic_fn)(void* user, dmvs_js_status_t status,
    const dmvs_js_location_t* location, dmvs_js_string_t message);

/**
 * @brief Explicit per-compilation budgets; every field must be nonzero.
 * Zero never means unlimited. Exceeding a budget returns LIMIT_EXCEEDED rather
 * than truncating output. Host DOM and source buffers are caller-owned and do
 * not count against max_memory_bytes. Backend layout/assets need their own
 * budgets. Allocator failure within the budget returns OUT_OF_MEMORY.
 */
typedef struct {
    size_t max_memory_bytes;    /**< Peak live compiler allocations, including parser and result. */
    size_t max_source_bytes;    /**< Maximum sum of source text byte lengths. */
    uint32_t max_sources;      /**< Maximum number of source fragments. */
    uint32_t max_nesting;      /**< Maximum syntactic nesting depth. */
    uint32_t max_nodes;        /**< Maximum distinct resolved host node IDs. */
    uint32_t max_variables;    /**< Maximum generated variables, including temporaries. */
    uint32_t max_instructions; /**< Maximum instructions across all handlers. */
    uint32_t max_handlers;     /**< Maximum initialization and click handlers combined. */
    uint32_t max_bindings;     /**< Maximum distinct DOM property bindings. */
    uint32_t max_string_bytes; /**< Maximum literal/value string length, excluding NUL. */
} dmvs_js_limits_t;

/**
 * @brief Complete input to one synchronous compilation.
 * Sources share classic-script global scope and are processed in array order.
 * The host sees the DOM snapshot supplied by the caller; browser loading,
 * async/defer ordering and resource resolution are not performed by this API.
 * All referenced memory remains caller-owned and valid until return.
 */
typedef struct {
    uint32_t abi_version;         /**< Must equal DMVS_JS_ABI_VERSION. */
    size_t struct_size;           /**< Must equal sizeof(dmvs_js_request_t). */
    const dmvs_js_source_t* sources; /**< Non-NULL array of source_count descriptors. */
    uint32_t source_count;        /**< Nonzero number of fragments. */
    const dmvs_js_host_t* host;   /**< Optional for DOM-free scripts; otherwise required. */
    dmvs_js_limits_t limits;     /**< Explicit compiler budgets. */
    dmvs_js_diagnostic_fn diagnostic; /**< Optional diagnostic callback; NULL disables it. */
    void* diagnostic_user;       /**< Caller-owned diagnostic context; may be NULL. */
} dmvs_js_request_t;

/**
 * @brief Typed storage slot in the compiled program.
 * The backend initializes storage before executing INIT. Source-level
 * initializers that require execution are emitted in INIT in source order.
 */
typedef struct {
    dmvs_js_type_t type;       /**< Storage type, equal to initial.type. */
    dmvs_js_value_t initial;   /**< Initial storage value; strings owned by program. */
    uint32_t string_capacity;  /**< STRING capacity in bytes without NUL; zero for other types. */
} dmvs_js_variable_t;

/** @brief How an instruction obtains an operand value. */
typedef enum {
    DMVS_JS_LITERAL, /**< Read as.literal. */
    DMVS_JS_VARIABLE /**< Read the variable indexed by as.variable. */
} dmvs_js_operand_kind_t;

/** @brief Tagged operand; use only the union member selected by kind. */
typedef struct {
    dmvs_js_operand_kind_t kind; /**< Payload discriminator. */
    union {
        dmvs_js_value_t literal; /**< Constant value, owned by program. */
        uint32_t variable;       /**< Zero-based variable index. */
    } as; /**< Literal or reference. */
} dmvs_js_operand_t;

/**
 * @brief Typed operations for a backend to lower to DMVS.
 * ADD/SUB require representable INT results; they do not specify wrapping JS
 * arithmetic. Instructions use only the fields described by their opcode.
 */
typedef enum {
    DMVS_JS_OP_SET,           /**< destination = left; types must match. */
    DMVS_JS_OP_ADD,           /**< INT destination = INT left + INT right. */
    DMVS_JS_OP_SUB,           /**< INT destination = INT left - INT right. */
    DMVS_JS_OP_EQ,            /**< BOOL destination = left === right; both INT or both BOOL. */
    DMVS_JS_OP_LT,            /**< BOOL destination = INT left < INT right. */
    DMVS_JS_OP_INT_TO_STRING, /**< STRING destination = decimal(INT left), no truncation. */
    DMVS_JS_OP_JUMP,          /**< Continue at target; other operand fields unused. */
    DMVS_JS_OP_JUMP_FALSE,    /**< Jump to target when BOOL left is false. */
    DMVS_JS_OP_RETURN         /**< End handler; destination, operands and target unused. */
} dmvs_js_opcode_t;

/**
 * @brief One instruction in the immutable program.
 * Indices and operand types are validated by the compiler. Unused fields are
 * zero-initialized and ignored by the backend. Jumps remain in the current
 * handler range; every reachable path terminates in RETURN.
 */
typedef struct {
    dmvs_js_opcode_t opcode;    /**< Operation and its field usage. */
    uint32_t destination;      /**< Result variable index when the opcode writes a value. */
    dmvs_js_operand_t left;    /**< First operand when required by opcode. */
    dmvs_js_operand_t right;   /**< Second operand for binary operations. */
    uint32_t target;           /**< Absolute instruction index for jump operations. */
    dmvs_js_location_t location; /**< Origin in the compilation source batch. */
} dmvs_js_instruction_t;

/**
 * @brief Connect a program variable to a DOM property.
 * There is at most one binding per property key. The backend resolves node IDs
 * and applies text/class changes through its layout/paint integration; the
 * compiled program does not call the original host to mutate the DOM.
 * Compiled reads of a bound property observe its variable after prior writes.
 */
typedef struct {
    dmvs_js_property_t property; /**< Target node and property, with program-owned name. */
    uint32_t variable;           /**< Variable index: STRING for TEXT, BOOL for CLASS. */
} dmvs_js_binding_t;

/** @brief Event that causes a handler to execute. */
typedef enum {
    DMVS_JS_INIT, /**< Initialization before first drawing; at most one handler. */
    DMVS_JS_CLICK /**< Direct click on the associated node. */
} dmvs_js_event_t;

/**
 * @brief Entry point and code range for an event.
 * Ranges are nonempty, disjoint and inside the instruction array. Handlers run
 * serially to RETURN. At most one CLICK handler is emitted per node; multiple
 * listeners are combined in registration order. No event object or bubbling
 * is represented by this descriptor.
 */
typedef struct {
    dmvs_js_event_t event; /**< Trigger type. */
    dmvs_js_node_t node;   /**< Zero for INIT; nonzero for CLICK. */
    uint32_t entry;       /**< First instruction's absolute index. */
    uint32_t count;       /**< Length of range [entry, entry + count). */
} dmvs_js_handler_t;

/**
 * @brief Borrowed, immutable program data for a code generator.
 * All arrays and nested strings belong to the program and remain valid until
 * dmvs_js_program_destroy(). Zero-count arrays may be NULL. Indices refer to
 * these arrays, never to addresses or serialized offsets. A backend must check
 * the ABI before consuming data and must not modify or free any member.
 */
typedef struct {
    uint32_t abi_version; /**< Equals DMVS_JS_ABI_VERSION. */
    size_t struct_size;   /**< Equals sizeof(dmvs_js_program_view_t). */
    const dmvs_js_variable_t* variables; /**< Typed storage declarations. */
    uint32_t variable_count; /**< Number of variables. */
    const dmvs_js_instruction_t* instructions; /**< Handler code. */
    uint32_t instruction_count; /**< Number of instructions. */
    const dmvs_js_binding_t* bindings; /**< DOM property connections. */
    uint32_t binding_count; /**< Number of bindings. */
    const dmvs_js_handler_t* handlers; /**< Event entry points. */
    uint32_t handler_count; /**< Number of handlers. */
} dmvs_js_program_view_t;

#ifdef __cplusplus
}
#endif
#endif

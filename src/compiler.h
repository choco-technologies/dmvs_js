#ifndef DMVS_JS_COMPILER_H
#define DMVS_JS_COMPILER_H

#include "private.h"

/*
 * The compiler: a partial evaluator. What is known when the page is
 * converted (static values: numbers, strings, the host's objects, arrays,
 * objects, functions) is computed; what changes while the view runs
 * (runtime values: variables of the document) becomes actions of the
 * handler being made - the init handler for what a script does when it
 * loads, a handler of its own for every function (a specialization of it:
 * a function called with other static arguments is another handler).
 */

#define TEXT_SIZE       64u             /* Bytes of a text variable the compiler makes */
#define MAX_TEMPS       24u             /* Temporaries of a kind in one handler */
#define MAX_ARGS        8u              /* Arguments of a call */
#define MAX_NAMES       512u            /* Names the pre-scan keeps */
#define MAX_SITES       32u             /* setTimeout / setInterval calls */
#define MAX_SELECT      32u             /* Elements of an array a runtime index picks from */
#define MAX_LOOPS       32u             /* Loops (and switches) nested */
#define FIX             DMVS_JS_SCALE   /* The scale of numbers with fractions */

typedef dmvs_js_value_t value_t;
typedef dmvs_js_node_t node_t;
typedef struct compiler compiler_t;
typedef struct scope scope_t;
typedef struct spec spec_t;

/* ---- The compiler's own values (DMVS_JS_V_INTERNAL) ---- */

#define O_ARRAY         1u
#define O_OBJECT        2u
#define O_FUNCTION      3u
#define O_BUILTIN       4u              /* A function or object of the language (Math.floor, setTimeout, ...) */
#define O_SELECT        5u              /* An element of a static array picked by a runtime index */

typedef struct object object_t;
struct object
{
    uint8_t         kind;               /* O_* */
    uint32_t        count;              /* ARRAY / OBJECT: elements, properties */
    value_t*        values;
    const char**    keys;               /* OBJECT */
    const node_t*   node;               /* FUNCTION: its node */
    scope_t*        env;                /* FUNCTION: where it was made */
    value_t         self;               /* FUNCTION: its `this` (an arrow's: where it was made); BUILTIN: the receiver */
    uint32_t        builtin;            /* BUILTIN: B_* */
    const char*     method;             /* BUILTIN: a static string's / array's method */
    const object_t* array;              /* SELECT: the array */
    dmvsi_var_t     index;              /* SELECT: the index (an integer variable) */
};

/* A loop being compiled: a runtime LOOP, or unrolled (break / continue are flags then) */
typedef struct loop loop_t;
struct loop
{
    bool            runtime;
    const node_t*   update;             /* A runtime for's: compiled before a continue */
    bool            broken;             /* Unrolled: a break was reached */
    bool            continued;          /* Unrolled: a continue was reached */
    bool            reported;
    uint32_t        blocks;             /* The IFs open where it is (one more: a break is under a runtime condition) */
};

/* ---- Names ---- */

typedef struct binding binding_t;
struct binding
{
    binding_t*      next;
    const char*     name;
    bool            constant;           /* const: never assigned */
    value_t         value;              /* Static, or RUNTIME: the variable it is */
};

struct scope
{
    scope_t*        parent;
    binding_t*      bindings;
    spec_t*         spec;               /* The function it is in (NULL: the top level) */
};

/* ---- Code: a handler being made ---- */

typedef struct code code_t;
struct code
{
    code_t*         outer;
    dmvsi_action_t* actions;
    uint32_t        count;
    uint32_t        capacity;
    const char*     prefix;             /* Of its variables' names */
    dmvsi_var_t     num_temps[MAX_TEMPS];
    dmvsi_var_t     text_temps[MAX_TEMPS];
    uint32_t        num_used;           /* Temporaries in use - released after each statement */
    uint32_t        text_used;
    uint32_t        loops;              /* LOOPs open */
    uint32_t        blocks;             /* IFs / LOOPs open */
    spec_t*         spec;               /* The function it is (NULL: the init handler, a piece of code) */
};

/* A function made into a handler for static arguments */
struct spec
{
    spec_t*         next;
    const object_t* fn;
    value_t         self;
    value_t         args[MAX_ARGS];     /* RUNTIME: a parameter set at each call (args[i].var) */
    uint32_t        argc;
    bool            compiling;
    bool            done;
    dmvsi_handler_t handler;            /* 0: nothing to run */
    value_t         ret;                /* RUNTIME: what it returns, in a variable; else static */
    bool            returns;            /* It returns a value */
    bool            ret_runtime;
    bool            ret_static_only;    /* Every return was this one static value */
    uint32_t        ret_sets;           /* SETs of `ret` in its code */
    const char*     name;
};

/*
 * A setTimeout / setInterval call (for each function it is given): what
 * it returns is its number (1 ...). Started, it is `active` and `due` at a
 * time ($time); a timer of the view polls it.
 */
typedef struct
{
    const node_t*   node;
    spec_t*         spec;               /* Where it is called (specializations have their own) */
    dmvsi_var_t     active;
    dmvsi_var_t     due;
    dmvsi_var_t     left;               /* The poll's: due - now */
    uint32_t        ms;
    bool            repeat;
    dmvsi_handler_t handler;            /* The callback */
} site_t;

/* A tree the compiler was given: it keeps it (its names, its functions) until it is freed */
typedef struct program program_t;
struct program
{
    program_t*      next;
    dmvs_js_ast_t   ast;
    const node_t*   root;
};

struct compiler
{
    dmvsi_doc_t     doc;
    dmvs_js_host_t  host;
    arena_t         arena;
    scope_t*        global;
    code_t*         code;               /* Being made */
    code_t          init;               /* What the scripts do when they load */
    spec_t*         specs;
    site_t          sites[MAX_SITES];
    uint32_t        site_count;
    dmvsi_handler_t clear;              /* clearInterval(a variable): stops the site it holds (made at the end) */
    dmvsi_var_t     clear_id;
    const node_t*   at;                 /* The node being compiled (reports) */
    loop_t*         loops[MAX_LOOPS];   /* Of the function being compiled */
    uint32_t        loop_depth;
    uint32_t        reports;
    bool            failed;             /* Out of memory */

    /* The pre-scan: names assigned anywhere, names that may hold fractions */
    const char*     assigned[MAX_NAMES];
    uint32_t        assigned_count;
    const char*     fractional[MAX_NAMES];
    uint32_t        fractional_count;
    program_t*      programs;           /* Every piece of code seen (the pre-scan looks at all) - the compiler's */
};

/* values.c */
void            report(compiler_t* c, const char* message);
value_t         v_undefined(void);
value_t         v_number(double n);
value_t         v_bool(bool b);
value_t         v_string(compiler_t* c, const char* s, size_t n);
value_t         v_runtime(uint8_t type, uint8_t scale, dmvsi_var_t var);
value_t         v_internal(const object_t* o);
object_t*       new_object(compiler_t* c, uint8_t kind);
const object_t* as_object(const value_t* v, uint8_t kind);
bool            is_static(const value_t* v);
bool            static_number(const value_t* v, double* n);     /* NUMBER, BOOL, NULL (0) */
bool            truthy(const value_t* v);
bool            same_static(const value_t* a, const value_t* b);
value_t         to_static_string(compiler_t* c, const value_t* v, bool* ok);

int             emit(compiler_t* c, const dmvsi_action_t* a);
int             emit_op(compiler_t* c, uint8_t kind, dmvsi_var_t var, dmvsi_var_t operand, int32_t value, const char* text);
int             emit_end(compiler_t* c);
dmvsi_var_t     new_var(compiler_t* c, const char* name, uint8_t type, int32_t initial, const char* text);
dmvsi_var_t     temp(compiler_t* c, uint8_t type);
void            release_temps(compiler_t* c);
bool            number_operand(compiler_t* c, const value_t* v, uint8_t scale, dmvsi_var_t* var, int32_t* imm);
value_t         number_copy(compiler_t* c, const value_t* v, uint8_t scale);      /* Into a temporary */
value_t         to_bool(compiler_t* c, const value_t* v);
bool            text_operand(compiler_t* c, const value_t* v, dmvsi_var_t* var, const char** text);
value_t         to_text(compiler_t* c, const value_t* v);
int             number_text(compiler_t* c, dmvsi_var_t text, const value_t* number, bool append);
int             fixed_text(compiler_t* c, dmvsi_var_t text, const value_t* number, uint32_t decimals, bool append);
value_t         concat(compiler_t* c, const value_t* parts, uint32_t count);
int             assign_to(compiler_t* c, const value_t* target, const value_t* v);     /* target: RUNTIME */
uint8_t         number_scale(const value_t* v);
bool            parse_number(const char* s, size_t n, double* out);

/* compile.c */
value_t         call_value(compiler_t* c, const value_t* callee, const value_t* self, const value_t* args, uint32_t count);
dmvsi_handler_t function_handler(compiler_t* c, const value_t* fn, const value_t* self);
bool            fractional_name(const compiler_t* c, const char* name);

/* builtins.c */
#define B_MATH          1u
#define B_MATH_FLOOR    2u
#define B_MATH_ROUND    3u
#define B_MATH_CEIL     4u
#define B_MATH_TRUNC    5u
#define B_MATH_MIN      6u
#define B_MATH_MAX      7u
#define B_MATH_ABS      8u
#define B_PARSE_FLOAT   9u
#define B_PARSE_INT     10u
#define B_NUMBER        11u
#define B_STRING        12u
#define B_SET_TIMEOUT   13u
#define B_SET_INTERVAL  14u
#define B_CLEAR_TIMER   15u
#define B_CONSOLE       16u
#define B_CONSOLE_LOG   17u
#define B_TO_FIXED      18u             /* Methods: `self` the receiver */
#define B_TO_STRING     19u
#define B_PAD_START     20u
#define B_FOR_EACH      21u
#define B_MATH_PI       22u
#define B_STRING_METHOD 23u             /* A static string's methods (computed when the page is converted) */
#define B_ARRAY_METHOD  25u             /* A static array's (join, includes, indexOf, map) */
#define B_NOOP          24u             /* What does nothing in a view (console.warn, ...) */

bool            builtin_global(compiler_t* c, const char* name, value_t* out);
bool            builtin_member(compiler_t* c, const value_t* object, const char* name, value_t* out);
value_t         builtin_call(compiler_t* c, const object_t* fn, const value_t* args, uint32_t count);
int             finish_timers(compiler_t* c);

#endif /* DMVS_JS_COMPILER_H */

/// bun_embed.h — High-performance C API for embedding Bun's JS runtime.
///
/// Design goals:
///   - Zero-copy value passing between C and JavaScript (BunValue = uint64_t)
///   - No JSON bridge overhead
///   - Direct registration of functions/objects/getters/setters
///
/// Typical flow:
///   1. bun_initialize()
///   2. bun_context() to get JS context
///   3. Register values/functions on global object
///   4. bun_eval_string()/bun_eval_file()
///   5. bun_run_pending_jobs() in host loop
///   6. bun_destroy()

#ifndef BUN_EMBED_H
#define BUN_EMBED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a Bun runtime instance.
typedef struct BunRuntime BunRuntime;

/// Opaque execution context (backed by JSGlobalObject* internally).
typedef struct BunContext BunContext;

/// Encoded JavaScript value (NaN-boxed JSValue).
typedef uint64_t BunValue;

/// Result from evaluating JavaScript code.
typedef struct {
    int success; // 1 if evaluation succeeded, 0 if failed
    const char* error; // Error description if success == 0. Owned by runtime, valid until next bun_eval_string* call.
} BunEvalResult;

/// Host function callback callable from JavaScript.
/// argv points to contiguous BunValue arguments valid only for this call.
typedef BunValue (*BunHostFn)(BunContext* ctx, int argc, const BunValue* argv, void* userdata);

/// Custom getter callback for bun_define_accessor().
typedef BunValue (*BunGetterFn)(BunContext* ctx, BunValue this_value);

/// Custom setter callback for bun_define_accessor().
typedef void (*BunSetterFn)(BunContext* ctx, BunValue this_value, BunValue value);

/// Finalizer callback for bun_define_finalizer().
///
/// This runs during GC finalization. It must not call back into Bun/JS APIs.
/// Use it only to release native resources associated with the object.
typedef void (*BunFinalizerFn)(void* userdata);

/// Predefined immediate values in JSValue64 mode.
#define BUN_UNDEFINED ((BunValue)0xAULL)
#define BUN_NULL ((BunValue)0x2ULL)
#define BUN_TRUE ((BunValue)0x7ULL)
#define BUN_FALSE ((BunValue)0x6ULL)

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

/// Initialize a Bun runtime instance. Must be called from the thread that will
/// drive the event loop (typically the main/GUI thread).
/// @param cwd  Working directory (UTF-8, null-terminated). Pass NULL for current dir.
/// @return  Runtime handle, or NULL on failure.
BunRuntime* bun_initialize(const char* cwd);

/// Destroy a Bun runtime and free all resources.
void bun_destroy(BunRuntime* rt);

/// Get the current JS context for this runtime.
BunContext* bun_context(BunRuntime* rt);

// --------------------------------------------------------------------------
// Evaluation
// --------------------------------------------------------------------------

/// Evaluate a JavaScript/TypeScript expression or script.
/// @param rt    Runtime handle.
/// @param code  UTF-8 source code, null-terminated.
/// @return      Evaluation result. The result and error strings are valid until
///              the next bun_eval*/bun_run_pending_jobs call on this runtime.
BunEvalResult bun_eval_string(BunRuntime* rt, const char* code);

/// Load and evaluate a JavaScript/TypeScript file as an ES module.
/// @param rt    Runtime handle.
/// @param path  UTF-8 file path, null-terminated. Relative paths resolve from cwd.
/// @return      Evaluation result.
BunEvalResult bun_eval_file(BunRuntime* rt, const char* path);

// --------------------------------------------------------------------------
// Event Loop Integration
// --------------------------------------------------------------------------

/// Drive the Bun event loop non-blockingly. Call this periodically in your GUI
/// main loop to process pending timers, promises, I/O callbacks, etc.
///
/// This performs a single non-blocking tick: it processes all currently ready
/// events and returns immediately. It will NOT block waiting for new events.
///
/// @param rt  Runtime handle.
/// @return    1 if the event loop has more pending work, 0 if idle.
int bun_run_pending_jobs(BunRuntime* rt);

/// Get the underlying OS event loop file descriptor (epoll fd on Linux,
/// kqueue fd on macOS). You can monitor this fd in your GUI's event loop
/// (e.g., via CFFileDescriptor/GSource) and call bun_run_pending_jobs()
/// when it becomes readable. Returns -1 on Windows or if unavailable.
int bun_get_event_fd(BunRuntime* rt);

/// Thread-safe: wake up the event loop from any thread. After calling this,
/// the next bun_run_pending_jobs() will process any concurrently queued work.
void bun_wakeup(BunRuntime* rt);

// --------------------------------------------------------------------------
// Value Creation
// --------------------------------------------------------------------------

BunValue bun_bool(int value);
BunValue bun_number(double value);
BunValue bun_int32(int32_t value);
BunValue bun_string(BunContext* ctx, const char* utf8, size_t len);
BunValue bun_object(BunContext* ctx);
BunValue bun_array(BunContext* ctx, size_t len);
BunValue bun_global(BunContext* ctx);
BunValue bun_function(BunContext* ctx, const char* name, BunHostFn fn, void* userdata, int arg_count);

// --------------------------------------------------------------------------
// Value Introspection & Conversion
// --------------------------------------------------------------------------

int bun_is_undefined(BunValue value);
int bun_is_null(BunValue value);
int bun_is_bool(BunValue value);
int bun_is_number(BunValue value);
int bun_is_string(BunValue value);
int bun_is_object(BunValue value);
int bun_is_callable(BunValue value);

int bun_to_bool(BunValue value);
double bun_to_number(BunContext* ctx, BunValue value);
int32_t bun_to_int32(BunValue value);

/// Returns a newly allocated UTF-8 string. Caller must free() the returned pointer.
/// On failure, returns NULL.
char* bun_to_utf8(BunContext* ctx, BunValue value, size_t* out_len);

// --------------------------------------------------------------------------
// Object & Property Operations
// --------------------------------------------------------------------------

int bun_set(BunContext* ctx, BunValue object, const char* key, size_t key_len, BunValue value);
BunValue bun_get(BunContext* ctx, BunValue object, const char* key, size_t key_len);

int bun_set_index(BunContext* ctx, BunValue object, uint32_t index, BunValue value);
BunValue bun_get_index(BunContext* ctx, BunValue object, uint32_t index);

int bun_define_getter(
    BunContext* ctx,
    BunValue object,
    const char* key,
    size_t key_len,
    BunGetterFn getter,
    int dont_enum,
    int dont_delete);

int bun_define_setter(
    BunContext* ctx,
    BunValue object,
    const char* key,
    size_t key_len,
    BunSetterFn setter,
    int dont_enum,
    int dont_delete);

int bun_define_accessor(
    BunContext* ctx,
    BunValue object,
    const char* key,
    size_t key_len,
    BunGetterFn getter,
    BunSetterFn setter,
    int read_only,
    int dont_enum,
    int dont_delete);

int bun_define_finalizer(
    BunContext* ctx,
    BunValue object,
    BunFinalizerFn finalizer,
    void* userdata);

int bun_set_prototype(BunContext* ctx, BunValue object, BunValue proto);

void bun_set_opaque(BunContext* ctx, BunValue object, void* opaque_ptr);
void* bun_get_opaque(BunContext* ctx, BunValue object);

// --------------------------------------------------------------------------
// Function Call & GC Lifetime
// --------------------------------------------------------------------------

BunValue bun_call(BunContext* ctx, BunValue fn, BunValue this_value, int argc, const BunValue* argv);
int bun_call_async(BunRuntime* rt, BunValue fn, BunValue this_value, int argc, const BunValue* argv);

void bun_protect(BunContext* ctx, BunValue value);
void bun_unprotect(BunContext* ctx, BunValue value);

#ifdef __cplusplus
}
#endif

#endif // BUN_EMBED_H

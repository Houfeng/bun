/// bun_embed.h — Minimal C API for embedding Bun's JS runtime into a host application.
///
/// Usage:
///   1. Call bun_initialize() once at startup.
///   2. Call bun_eval() / bun_eval_file() to execute JavaScript/TypeScript code.
///   3. Call bun_eval_pending_jobs() periodically (e.g. in your GUI main loop) to
///      drive the event loop (timers, promises, I/O, etc.).
///   4. Use bun_inject_native_function() to expose C functions to the JS environment.
///   5. Call bun_destroy() when done.

#ifndef BUN_EMBED_H
#define BUN_EMBED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a Bun runtime instance.
typedef struct BunRuntime BunRuntime;

/// Result from evaluating JavaScript code.
typedef struct {
    int success;         // 1 if evaluation succeeded, 0 if failed
    const char *error;   // Error description if success == 0. Owned by runtime, valid until next bun_eval* call.
} BunEvalResult;

/// Type for a C function callable from JavaScript.
/// @param argc  Number of arguments passed from JS.
/// @param argv  Array of arguments as UTF-8 JSON-stringified values. Each is a
///              null-terminated string valid only for the duration of this call.
/// @param userdata  The userdata pointer passed to bun_inject_native_function().
/// @return  A UTF-8 JSON string representing the return value (e.g. "42",
///          "\"hello\"", "null"). The runtime takes ownership; allocate with
///          malloc(). Return NULL to return undefined to JS.
typedef char* (*BunNativeFunction)(int argc, const char **argv, void *userdata);

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

/// Initialize a Bun runtime instance. Must be called from the thread that will
/// drive the event loop (typically the main/GUI thread).
/// @param cwd  Working directory (UTF-8, null-terminated). Pass NULL for current dir.
/// @return  Runtime handle, or NULL on failure.
BunRuntime *bun_initialize(const char *cwd);

/// Destroy a Bun runtime and free all resources.
void bun_destroy(BunRuntime *rt);

// --------------------------------------------------------------------------
// Evaluation
// --------------------------------------------------------------------------

/// Evaluate a JavaScript/TypeScript expression or script.
/// @param rt    Runtime handle.
/// @param code  UTF-8 source code, null-terminated.
/// @return      Evaluation result. The result and error strings are valid until
///              the next bun_eval*/bun_eval_pending_jobs call on this runtime.
BunEvalResult bun_eval(BunRuntime *rt, const char *code);

/// Load and evaluate a JavaScript/TypeScript file as an ES module.
/// @param rt    Runtime handle.
/// @param path  UTF-8 file path, null-terminated. Relative paths resolve from cwd.
/// @return      Evaluation result.
BunEvalResult bun_eval_file(BunRuntime *rt, const char *path);

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
int bun_eval_pending_jobs(BunRuntime *rt);

/// Get the underlying OS event loop file descriptor (epoll fd on Linux,
/// kqueue fd on macOS). You can monitor this fd in your GUI's event loop
/// (e.g., via CFFileDescriptor/GSource) and call bun_eval_pending_jobs()
/// when it becomes readable. Returns -1 on Windows or if unavailable.
int bun_get_event_fd(BunRuntime *rt);

/// Thread-safe: wake up the event loop from any thread. After calling this,
/// the next bun_eval_pending_jobs() will process any concurrently queued work.
void bun_wakeup(BunRuntime *rt);

// --------------------------------------------------------------------------
// Native Function Injection
// --------------------------------------------------------------------------

/// Register a C function that becomes callable from JavaScript as a global function.
///
/// JS code can then call it as: `globalThis.<name>(...args)`
///
/// Arguments are passed as JSON strings. The return value should be a
/// malloc'd JSON string (or NULL for undefined).
///
/// @param rt        Runtime handle.
/// @param name      Function name (UTF-8, null-terminated).
/// @param fn        Native function pointer.
/// @param userdata  Arbitrary pointer passed to fn on every call.
/// @return          1 on success, 0 on failure.
int bun_inject_native_function(BunRuntime *rt, const char *name, BunNativeFunction fn, void *userdata);

/// Register a C function with direct JSC interop (advanced).
/// The function receives raw JSC values. Use only if you link against JSC.
/// Signature: JSValue (*)(JSGlobalObject*, CallFrame*) with C calling convention.
///
/// @param rt        Runtime handle.
/// @param name      Function name (UTF-8, null-terminated).
/// @param fn_ptr    JSC-compatible host function pointer (cast to void*).
/// @param arg_count Number of expected arguments (.length property in JS).
/// @return          1 on success, 0 on failure.
int bun_inject_native_function_raw(BunRuntime *rt, const char *name, void *fn_ptr, int arg_count);

#ifdef __cplusplus
}
#endif

#endif // BUN_EMBED_H

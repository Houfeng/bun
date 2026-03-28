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

/// Opaque runtime-local class handle returned by bun_class_register().
typedef struct BunClass BunClass;

/// Encoded JavaScript value (NaN-boxed JSValue).
typedef uint64_t BunValue;

/// Result from evaluating JavaScript code.
typedef struct {
    int success; ///< 1 if evaluation succeeded, 0 if failed.
    /// Error description when success == 0. Points into the runtime's internal
    /// buffer; valid until the next bun_eval*() or bun_call() call on this
    /// runtime. Do not free(). NULL when success == 1.
    const char* error;
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

/// Native instance method callback for bun_class_register().
typedef BunValue (*BunClassMethodFn)(BunContext* ctx, BunValue this_value, void* native_ptr,
    int argc, const BunValue* argv, void* userdata);

/// Native property getter callback for bun_class_register().
typedef BunValue (*BunClassGetterFn)(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata);

/// Native property setter callback for bun_class_register().
typedef void (*BunClassSetterFn)(BunContext* ctx, BunValue this_value, void* native_ptr,
    BunValue value, void* userdata);

/// Finalizer callback for bun_class_new().
///
/// Runs at most once, either when bun_class_dispose() is called or when the JS
/// object is eventually GC'd. It must not call back into Bun/JS APIs.
typedef void (*BunClassFinalizerFn)(void* native_ptr, void* userdata);

typedef struct {
    const char* name;
    size_t name_len;
    BunClassMethodFn callback;
    void* userdata;
    int arg_count;
    int dont_enum;
    int dont_delete;
} BunClassMethodDescriptor;

typedef struct {
    const char* name;
    size_t name_len;
    BunClassGetterFn getter;
    BunClassSetterFn setter;
    void* userdata;
    int read_only;
    int dont_enum;
    int dont_delete;
} BunClassPropertyDescriptor;

typedef struct {
    const char* name;
    size_t name_len;
    const BunClassPropertyDescriptor* properties;
    size_t property_count;
    const BunClassMethodDescriptor* methods;
    size_t method_count;
} BunClassDescriptor;

/// Predefined immediate values in JSValue64 mode.
#define BUN_UNDEFINED ((BunValue)0xAULL)
#define BUN_NULL ((BunValue)0x2ULL)
#define BUN_TRUE ((BunValue)0x7ULL)
#define BUN_FALSE ((BunValue)0x6ULL)

/// Sentinel returned by bun_call() when a JavaScript exception was thrown.
/// This is JSC's internal zero/exception sentinel and is never a valid
/// return value from a successful call.
#define BUN_EXCEPTION ((BunValue)0ULL)

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

/// Element type for bun_typed_array().
typedef enum {
    BUN_INT8_ARRAY = 0,
    BUN_UINT8_ARRAY = 1,
    BUN_UINT8C_ARRAY = 2, ///< Uint8ClampedArray
    BUN_INT16_ARRAY = 3,
    BUN_UINT16_ARRAY = 4,
    BUN_INT32_ARRAY = 5,
    BUN_UINT32_ARRAY = 6,
    BUN_FLOAT32_ARRAY = 7,
    BUN_FLOAT64_ARRAY = 8,
    BUN_BIGINT64_ARRAY = 9,
    BUN_BIGUINT64_ARRAY = 10,
} BunTypedArrayKind;

typedef struct {
    void* data;
    size_t byte_length;
} BunArrayBufferInfo;

typedef struct {
    void* data;
    size_t byte_offset;
    size_t byte_length;
    size_t element_count;
    BunTypedArrayKind kind;
} BunTypedArrayInfo;

/// Wrap C memory as a JS ArrayBuffer (zero-copy).
///
/// The JS ArrayBuffer directly references `data` — no copy is made.
/// When the GC eventually collects the buffer, `finalizer(userdata)` is called.
/// Pass NULL for finalizer only if the memory outlives the entire runtime
/// (e.g. static data, or memory you free yourself at bun_destroy() time).
///
/// MEMORY LIFETIME CONTRACT
///   `data` MUST stay valid until finalizer is called (or until bun_destroy()
///   if no finalizer). Do NOT free `data` before the runtime is destroyed.
///
/// KNOWN LEAK RISK — bun_destroy() does not guarantee finalizer invocation
///   JSC does not guarantee that every in-flight GC finalizer runs during VM
///   teardown. If the runtime is destroyed while a buffer is still reachable
///   from JS (e.g. it was bun_protect()'d and never unprotect()'d, or it is
///   referenced by a long-lived closure), the finalizer may NOT be called.
///   For truly critical resources (sockets, file handles, GPU buffers), keep
///   your own list and release them explicitly before calling bun_destroy().
///
/// AVOID DOUBLE-WRAP
///   Wrapping the same `data` pointer twice creates two independent finalizers;
///   both will fire and cause a double-free. Each C pointer must be passed to
///   at most one bun_array_buffer() / bun_typed_array() call.
///
/// @param ctx        JS context.
/// @param data       Backing memory (must remain valid until finalizer fires).
/// @param len        Byte length.
/// @param finalizer  Called on GC collection (may be NULL). Must NOT re-enter Bun/JS.
/// @param userdata   Forwarded verbatim to finalizer.
/// @return           BunValue holding the JS ArrayBuffer, or BUN_UNDEFINED on failure.
///                   On failure the finalizer is NOT called — the caller retains ownership.
BunValue bun_array_buffer(BunContext* ctx, void* data, size_t len,
    BunFinalizerFn finalizer, void* userdata);

/// Wrap C memory as a JS TypedArray view (zero-copy).
///
/// Equivalent to `new Float32Array(arraybuffer)` but zero-copy — the view
/// directly references the C pointer. finalizer(userdata) fires when the
/// underlying ArrayBuffer is GC'd (which may be later than the TypedArray
/// itself if JS code has taken a reference to `.buffer`).
///
/// The same MEMORY LIFETIME CONTRACT, LEAK RISK, and AVOID DOUBLE-WRAP rules
/// from bun_array_buffer() apply here.
///
/// CONCURRENT ACCESS
///   Do NOT write to `data` while bun_run_pending_jobs() may be executing;
///   there is no internal lock. Synchronize access at the application level.
///
/// @param ctx           JS context.
/// @param kind          Element type (BUN_FLOAT32_ARRAY, BUN_INT32_ARRAY, …).
/// @param data          Backing memory.
/// @param element_count Number of *elements* (not bytes). A size_t overflow
///                      check is performed; the call returns BUN_UNDEFINED and
///                      does NOT invoke the finalizer if it would overflow.
/// @param finalizer     Called on GC collection (may be NULL).
/// @param userdata      Forwarded verbatim to finalizer.
/// @return              BunValue holding the TypedArray, or BUN_UNDEFINED on failure.
///                      On failure the finalizer is NOT called — caller retains ownership.
BunValue bun_typed_array(BunContext* ctx, BunTypedArrayKind kind,
    void* data, size_t element_count,
    BunFinalizerFn finalizer, void* userdata);

/// Read the backing pointer and length from an ArrayBuffer.
///
/// This performs no coercion. Returns 1 only when `value` is an attached
/// ArrayBuffer and `out` is non-NULL.
int bun_get_array_buffer(BunContext* ctx, BunValue value, BunArrayBufferInfo* out);

/// Read the backing pointer and metadata from a TypedArray.
///
/// This performs no coercion. Returns 1 only when `value` is an attached
/// TypedArray (including subclasses such as Buffer) and `out` is non-NULL.
/// DataView is not accepted.
int bun_get_typed_array(BunContext* ctx, BunValue value, BunTypedArrayInfo* out);

// --------------------------------------------------------------------------
// Class API
// --------------------------------------------------------------------------

/// Register a host-defined class for this runtime.
///
/// The returned BunClass* is only valid for the current runtime. Pass a parent
/// returned by a prior bun_class_register() call on the same runtime to build a
/// prototype chain.
BunClass* bun_class_register(BunContext* ctx, const BunClassDescriptor* descriptor, BunClass* parent);

/// Create an instance of a registered BunClass.
///
/// The returned object is a normal BunValue object and can be used with the
/// existing generic APIs (bun_get, bun_set, bun_call, bun_protect, ...).
BunValue bun_class_new(BunContext* ctx, BunClass* klass, void* native_ptr,
    BunClassFinalizerFn finalizer, void* userdata);

/// Return the native pointer for a class instance, or NULL on mismatch.
///
/// `klass` may be any ancestor class handle returned by bun_class_register().
void* bun_class_unwrap(BunContext* ctx, BunValue value, BunClass* klass);

/// Return 1 when value is any BunClass instance, else 0.
int bun_is_class_instance(BunContext* ctx, BunValue value);

/// Return 1 when value is an instance of `klass` or one of its subclasses.
int bun_instanceof_class(BunContext* ctx, BunValue value, BunClass* klass);

/// Dispose an instance early and run its finalizer immediately if needed.
///
/// The finalizer runs at most once. Repeated calls return 0 after the first.
int bun_class_dispose(BunContext* ctx, BunValue value);

/// Return the runtime-local prototype object for a registered class.
BunValue bun_class_prototype(BunContext* ctx, BunClass* klass);

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

/// Call a JavaScript function synchronously.
///
/// @param ctx        JS context.
/// @param fn         Callable BunValue.
/// @param this_value The 'this' binding. Pass BUN_UNDEFINED for global 'this'.
/// @param argc       Argument count.
/// @param argv       Argument array (may be NULL when argc == 0).
/// @return           The JS return value on success, or BUN_EXCEPTION (0) if a
///                   JS exception was thrown. Call bun_last_error(ctx) to read
///                   the exception message after a BUN_EXCEPTION return.
BunValue bun_call(BunContext* ctx, BunValue fn, BunValue this_value, int argc, const BunValue* argv);

/// Return the error string from the most recent bun_call()/bun_eval*() that
/// failed on this context. Owned by the runtime; valid until the next
/// bun_call()/bun_eval*() call. Returns NULL if no error is pending.
const char* bun_last_error(BunContext* ctx);

int bun_call_async(BunRuntime* rt, BunValue fn, BunValue this_value, int argc, const BunValue* argv);

void bun_protect(BunContext* ctx, BunValue value);
void bun_unprotect(BunContext* ctx, BunValue value);

#ifdef __cplusplus
}
#endif

#endif // BUN_EMBED_H

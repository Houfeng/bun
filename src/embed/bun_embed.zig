///! bun_embed.zig — High-performance C API for embedding Bun's JS runtime.
///! See bun_embed.h for API documentation.
const std = @import("std");
const bun = @import("bun");
const jsc = bun.jsc;
const js_ast = bun.ast;
const Arena = bun.allocators.MimallocArena;
const VirtualMachine = jsc.VirtualMachine;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const JSModuleLoader = jsc.JSModuleLoader;
const Environment = bun.Environment;
const api = bun.schema.api;

const BunValue = u64;
const BunContext = opaque {};
const BunClass = opaque {};

const BunHostFn = *const fn (?*BunContext, c_int, ?[*]const BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunGetterFn = *const fn (?*BunContext, BunValue) callconv(.c) BunValue;
const BunSetterFn = *const fn (?*BunContext, BunValue, BunValue) callconv(.c) void;
const BunFinalizerFn = *const fn (?*anyopaque) callconv(.c) void;
const BunClassMethodFn = *const fn (?*BunContext, BunValue, ?*anyopaque, c_int, ?[*]const BunValue, ?*anyopaque) callconv(.c) BunValue;
const BunClassGetterFn = *const fn (?*BunContext, BunValue, ?*anyopaque, ?*anyopaque) callconv(.c) BunValue;
const BunClassSetterFn = *const fn (?*BunContext, BunValue, ?*anyopaque, BunValue, ?*anyopaque) callconv(.c) void;
const BunClassFinalizerFn = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;

const BunClassMethodDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    callback: ?BunClassMethodFn,
    userdata: ?*anyopaque,
    arg_count: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
};

const BunClassPropertyDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    getter: ?BunClassGetterFn,
    setter: ?BunClassSetterFn,
    userdata: ?*anyopaque,
    read_only: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
};

const BunClassDescriptor = extern struct {
    name: ?[*]const u8,
    name_len: usize,
    properties: ?[*]const BunClassPropertyDescriptor,
    property_count: usize,
    methods: ?[*]const BunClassMethodDescriptor,
    method_count: usize,
};

const BunArrayBufferInfo = extern struct {
    data: ?*anyopaque,
    byte_length: usize,
};

const BunTypedArrayInfo = extern struct {
    data: ?*anyopaque,
    byte_offset: usize,
    byte_length: usize,
    element_count: usize,
    kind: u32,
};

const HostFnData = struct {
    native_fn: BunHostFn,
    userdata: ?*anyopaque,
    /// Back-pointer so the GC finalizer can remove this entry from the registry.
    runtime: *BunRuntime,
    /// The JSValue key used in host_fn_registry (needed for removal in finalizer).
    js_fn: JSValue,
};

/// Per-object side-table entry for opaque pointer + finalizer-dedup guard.
const OpaqueEntry = struct {
    /// Native pointer stored by bun_set_opaque().
    opaque_ptr: ?*anyopaque = null,
    /// Whether a user finalizer has already been attached via bun_define_finalizer().
    finalizer_attached: bool = false,
};

const PendingCall = struct {
    fn_value: BunValue,
    this_value: BunValue,
    argv: []BunValue,
};

/// Opaque runtime handle exposed to C as `BunRuntime*`.
const BunRuntime = struct {
    vm: *VirtualMachine,
    arena: Arena,
    /// Scratch buffer for returning error strings to C callers. Valid until next eval call.
    last_error_buf: ?[*:0]u8 = null,
    pending_calls: std.ArrayListUnmanaged(PendingCall) = .{},
    pending_calls_mutex: std.Thread.Mutex = .{},
    /// Per-runtime registry of JSValue → HostFnData. Replaces the old global map.
    host_fn_registry: HostFnMap = .{},
    /// Per-runtime side-table for opaque pointers and finalizer-dedup guards.
    /// Keyed by JSValue identity; replaces the old string-property approach.
    opaque_map: OpaqueMap = .{},
    /// Runtime-local class handles allocated by BunEmbed.cpp.
    class_registry: std.ArrayListUnmanaged(*BunClass) = .{},

    fn captureException(runtime: *BunRuntime, global: *JSGlobalObject, value: JSValue) BunEvalResult {
        // Try to get a string representation via toString()
        const slice = value.toSlice(global, bun.default_allocator) catch {
            return .{ .success = 0, .@"error" = "exception (failed to capture message)" };
        };
        defer slice.deinit();

        const data = slice.ptr[0..slice.len];

        // Null-terminate and store
        const err_str = bun.default_allocator.allocSentinel(u8, data.len, 0) catch {
            return .{ .success = 0, .@"error" = "exception (failed to capture message)" };
        };
        @memcpy(err_str[0..data.len], data);

        runtime.freeLastError();
        runtime.last_error_buf = err_str;

        return .{ .success = 0, .@"error" = err_str };
    }

    fn freeLastError(runtime: *BunRuntime) void {
        if (runtime.last_error_buf) |buf| {
            bun.default_allocator.free(std.mem.span(buf));
            runtime.last_error_buf = null;
        }
    }

    fn freePendingCalls(runtime: *BunRuntime) void {
        runtime.pending_calls_mutex.lock();
        defer runtime.pending_calls_mutex.unlock();

        for (runtime.pending_calls.items) |call| {
            bun.default_allocator.free(call.argv);
        }
        runtime.pending_calls.deinit(bun.default_allocator);
    }

    fn freeHostFnRegistry(runtime: *BunRuntime) void {
        var it = runtime.host_fn_registry.valueIterator();
        while (it.next()) |item| {
            bun.default_allocator.destroy(item.*);
        }
        runtime.host_fn_registry.deinit(bun.default_allocator);
    }

    fn freeOpaqueMap(runtime: *BunRuntime) void {
        runtime.opaque_map.deinit(bun.default_allocator);
    }

    fn freeClassRegistry(runtime: *BunRuntime) void {
        for (runtime.class_registry.items) |class_handle| {
            BunEmbed__destroyClass(class_handle);
        }
        runtime.class_registry.deinit(bun.default_allocator);
    }
};

/// Result struct matching the C BunEvalResult layout.
const BunEvalResult = extern struct {
    success: c_int,
    @"error": ?[*:0]const u8,
};

const BunDebuggerMode = enum(c_int) {
    off = 0,
    attach = 1,
    wait = 2,
    @"break" = 3,
};

const BunInitializeOptions = extern struct {
    cwd: ?[*:0]const u8,
    debugger_mode: BunDebuggerMode,
    debugger_listen_url: ?[*:0]const u8,
};

const HostFnMap = std.AutoHashMapUnmanaged(JSValue, *HostFnData);
const OpaqueMap = std.AutoHashMapUnmanaged(JSValue, OpaqueEntry);

/// Global lookup table: VirtualMachine* → *BunRuntime.
/// Populated in bun_initialize(), removed in bun_destroy().
/// This is the only remaining global state, and it is necessary because the
/// hostFnTrampoline's JSC callback only receives the JSGlobalObject; we need
/// to recover the owning BunRuntime to access its per-runtime registries.
var vm_to_runtime_map: std.AutoHashMapUnmanaged(*VirtualMachine, *BunRuntime) = .{};
var vm_to_runtime_mutex: std.Thread.Mutex = .{};

fn vmToRuntime(vm: *VirtualMachine) ?*BunRuntime {
    vm_to_runtime_mutex.lock();
    defer vm_to_runtime_mutex.unlock();
    return vm_to_runtime_map.get(vm);
}

fn registerRuntime(runtime: *BunRuntime) void {
    vm_to_runtime_mutex.lock();
    defer vm_to_runtime_mutex.unlock();
    vm_to_runtime_map.put(bun.default_allocator, runtime.vm, runtime) catch {};
}

fn unregisterRuntime(runtime: *BunRuntime) void {
    vm_to_runtime_mutex.lock();
    defer vm_to_runtime_mutex.unlock();
    _ = vm_to_runtime_map.remove(runtime.vm);
}

fn toBunValue(value: JSValue) BunValue {
    return @as(BunValue, @bitCast(@as(i64, @intFromEnum(value))));
}

fn toJSValue(value: BunValue) JSValue {
    return @as(JSValue, @enumFromInt(@as(i64, @bitCast(value))));
}

fn toGlobal(ctx: ?*BunContext) ?*JSGlobalObject {
    return if (ctx) |ptr| @ptrCast(ptr) else null;
}

extern fn BunString__createUTF8ForJS(globalObject: *JSGlobalObject, ptr: [*]const u8, length: usize) JSValue;

extern fn JSFunction__createFromZig(
    global: *JSGlobalObject,
    fn_name: bun.String,
    implementation: *const jsc.JSHostFn,
    arg_count: u32,
    implementation_visibility: u8,
    intrinsic: u8,
    constructor: ?*const jsc.JSHostFn,
) JSValue;

extern fn BunEmbed__defineCustomAccessor(
    global: *JSGlobalObject,
    object: JSValue,
    key_ptr: [*]const u8,
    key_len: usize,
    getter: ?BunGetterFn,
    setter: ?BunSetterFn,
    flags: u32,
) bool;

extern fn BunEmbed__defineFinalizer(
    global: *JSGlobalObject,
    object: JSValue,
    finalizer: BunFinalizerFn,
    userdata: ?*anyopaque,
) bool;

extern fn BunEmbed__createArrayBuffer(
    global: *JSGlobalObject,
    data: ?*anyopaque,
    byte_len: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) JSValue;

extern fn BunEmbed__createTypedArray(
    global: *JSGlobalObject,
    kind: u32,
    data: ?*anyopaque,
    element_count: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) JSValue;

extern fn BunEmbed__getArrayBuffer(
    global: *JSGlobalObject,
    value: JSValue,
    out: *BunArrayBufferInfo,
) bool;

extern fn BunEmbed__getTypedArray(
    global: *JSGlobalObject,
    value: JSValue,
    out: *BunTypedArrayInfo,
) bool;

extern fn BunEmbed__registerClass(
    global: *JSGlobalObject,
    descriptor: *const BunClassDescriptor,
    parent: ?*BunClass,
) ?*BunClass;

extern fn BunEmbed__destroyClass(class_handle: *BunClass) void;

extern fn BunEmbed__createClassInstance(
    global: *JSGlobalObject,
    class_handle: *BunClass,
    native_ptr: ?*anyopaque,
    finalizer: ?BunClassFinalizerFn,
    userdata: ?*anyopaque,
) JSValue;

extern fn BunEmbed__unwrapClassInstance(
    global: *JSGlobalObject,
    value: JSValue,
    class_handle: ?*BunClass,
) ?*anyopaque;

extern fn BunEmbed__isClassInstance(
    global: *JSGlobalObject,
    value: JSValue,
) bool;

extern fn BunEmbed__instanceofClass(
    global: *JSGlobalObject,
    value: JSValue,
    class_handle: *BunClass,
) bool;

extern fn BunEmbed__disposeClassInstance(
    global: *JSGlobalObject,
    value: JSValue,
) bool;

extern fn BunEmbed__classPrototype(
    global: *JSGlobalObject,
    class_handle: *BunClass,
) JSValue;

const BUN_ACCESSOR_READ_ONLY: u32 = 1 << 0;
const BUN_ACCESSOR_DONT_ENUM: u32 = 1 << 1;
const BUN_ACCESSOR_DONT_DELETE: u32 = 1 << 2;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

pub export fn bun_initialize(options: ?*const BunInitializeOptions) callconv(.c) ?*BunRuntime {
    return initializeImpl(options) catch null;
}

fn debuggerFromOptions(options: ?*const BunInitializeOptions) bun.cli.Command.Debugger {
    const opts = options orelse return .{ .unspecified = {} };
    const path_or_port = if (opts.debugger_listen_url) |value| std.mem.span(value) else "";

    return switch (opts.debugger_mode) {
        .off => .{ .unspecified = {} },
        .attach => .{ .enable = .{ .path_or_port = path_or_port } },
        .wait => .{ .enable = .{ .path_or_port = path_or_port, .wait_for_connection = true } },
        .@"break" => .{ .enable = .{
            .path_or_port = path_or_port,
            .wait_for_connection = true,
            .set_breakpoint_on_first_line = true,
        } },
    };
}

fn initializeImpl(options: ?*const BunInitializeOptions) !?*BunRuntime {
    // Crash handler (idempotent)
    bun.crash_handler.init();

    if (Environment.isPosix) {
        var act: std.posix.Sigaction = .{
            .handler = .{ .handler = std.posix.SIG.IGN },
            .mask = std.posix.sigemptyset(),
            .flags = 0,
        };
        std.posix.sigaction(std.posix.SIG.PIPE, &act, null);
    }

    // Initialize JSC
    bun.jsc.initialize(false);

    // AST stores
    js_ast.Expr.Data.Store.create();
    js_ast.Stmt.Data.Store.create();

    // Arena
    const arena = Arena.init();
    const allocator = arena.allocator();

    // Build a minimal TransformOptions
    var args = api.TransformOptions{
        .entry_points = &.{},
        .inject = &.{},
        .external = &.{},
        .main_fields = &.{},
        .env_files = &.{},
        .extension_order = &.{},
        .conditions = &.{},
        .ignore_dce_annotations = false,
        .bunfig_path = "",
    };

    // Set the working directory if provided
    if (options) |opts| if (opts.cwd) |cwd| {
        args.absolute_working_dir = std.mem.span(cwd);
    };

    // Create the VM
    const vm = try VirtualMachine.init(.{
        .allocator = allocator,
        .args = args,
        .debugger = debuggerFromOptions(options),
        .is_main_thread = true,
    });

    var b = &vm.transpiler;
    b.options.env.behavior = .load_all_without_inlining;

    b.configureDefines() catch {
        return null;
    };

    vm.loadExtraEnvAndSourceCodePrinter();
    vm.is_main_thread = true;
    jsc.VirtualMachine.is_main_thread_vm = true;

    // Allocate the runtime handle
    const rt = bun.default_allocator.create(BunRuntime) catch return null;
    rt.* = .{
        .vm = vm,
        .arena = arena,
        .last_error_buf = null,
    };

    registerRuntime(rt);
    return rt;
}

pub export fn bun_destroy(rt: ?*BunRuntime) callconv(.c) void {
    const runtime = rt orelse return;
    unregisterRuntime(runtime);
    runtime.freeLastError();
    runtime.freePendingCalls();
    runtime.freeHostFnRegistry();
    runtime.freeOpaqueMap();
    runtime.freeClassRegistry();

    runtime.vm.onExit();
    bun.default_allocator.destroy(runtime);
}

pub export fn bun_context(rt: ?*BunRuntime) callconv(.c) ?*BunContext {
    const runtime = rt orelse return null;
    return @ptrCast(runtime.vm.global);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

pub export fn bun_eval_string(ctx: ?*BunContext, code_ptr: ?[*:0]const u8) callconv(.c) BunEvalResult {
    const global = toGlobal(ctx) orelse return .{ .success = 0, .@"error" = "null context" };
    const runtime = vmToRuntime(global.bunVM()) orelse return .{ .success = 0, .@"error" = "context has no runtime" };
    runtime.freeLastError();

    const code = if (code_ptr) |p| std.mem.span(p) else return .{ .success = 0, .@"error" = "null code" };

    var eval_ctx = EvalContext{
        .runtime = runtime,
        .global = global,
        .code = code,
        .result = .{ .success = 0, .@"error" = "eval did not complete" },
    };
    runtime.vm.runWithAPILock(EvalContext, &eval_ctx, EvalContext.run);
    return eval_ctx.result;
}

const EvalContext = struct {
    runtime: *BunRuntime,
    global: *JSGlobalObject,
    code: []const u8,
    result: BunEvalResult,

    pub fn run(this: *EvalContext) void {
        var exception: [1]JSValue = .{.js_undefined};
        const ret = JSModuleLoader.evaluate(
            this.global,
            this.code.ptr,
            this.code.len,
            "embed:eval",
            "embed:eval".len,
            "",
            0,
            .js_undefined,
            &exception,
        );

        if (exception[0] != .js_undefined and exception[0] != .zero) {
            this.result = this.runtime.captureException(this.global, exception[0]);
        } else if (ret == .zero) {
            this.result = .{ .success = 0, .@"error" = "evaluation returned null" };
        } else {
            this.result = .{ .success = 1, .@"error" = null };
        }
    }
};

pub export fn bun_eval_file(ctx: ?*BunContext, path_ptr: ?[*:0]const u8) callconv(.c) BunEvalResult {
    const global = toGlobal(ctx) orelse return .{ .success = 0, .@"error" = "null context" };
    const runtime = vmToRuntime(global.bunVM()) orelse return .{ .success = 0, .@"error" = "context has no runtime" };
    runtime.freeLastError();

    const path = if (path_ptr) |p| std.mem.span(p) else return .{ .success = 0, .@"error" = "null path" };

    var eval_ctx = EvalFileContext{
        .runtime = runtime,
        .global = global,
        .path = path,
        .result = .{ .success = 0, .@"error" = "eval_file did not complete" },
    };
    runtime.vm.runWithAPILock(EvalFileContext, &eval_ctx, EvalFileContext.run);
    return eval_ctx.result;
}

const EvalFileContext = struct {
    runtime: *BunRuntime,
    global: *JSGlobalObject,
    path: []const u8,
    result: BunEvalResult,

    pub fn run(this: *EvalFileContext) void {
        const vm = this.runtime.vm;

        const promise = vm.loadEntryPoint(this.path) catch {
            this.result = .{ .success = 0, .@"error" = "failed to load entry point" };
            return;
        };

        switch (promise.status()) {
            .rejected => {
                const rejection = promise.result();
                this.result = this.runtime.captureException(this.global, rejection);
            },
            else => {
                this.result = .{ .success = 1, .@"error" = null };
            },
        }
    }
};

// ---------------------------------------------------------------------------
// Event Loop Integration
// ---------------------------------------------------------------------------

pub export fn bun_run_pending_jobs(rt: ?*BunRuntime) callconv(.c) c_int {
    const runtime = rt orelse return 0;
    var tick_ctx = TickContext{ .runtime = runtime, .has_pending = false };
    runtime.vm.runWithAPILock(TickContext, &tick_ctx, TickContext.run);
    return if (tick_ctx.has_pending) 1 else 0;
}

const TickContext = struct {
    runtime: *BunRuntime,
    has_pending: bool,

    fn drainPendingCalls(runtime: *BunRuntime, global: *JSGlobalObject) void {
        runtime.pending_calls_mutex.lock();
        var local_calls = runtime.pending_calls;
        runtime.pending_calls = .{};
        runtime.pending_calls_mutex.unlock();

        defer {
            for (local_calls.items) |call| {
                bun.default_allocator.free(call.argv);
            }
            local_calls.deinit(bun.default_allocator);
        }

        for (local_calls.items) |call| {
            const fn_value = toJSValue(call.fn_value);
            const this_value = toJSValue(call.this_value);
            const args: []const JSValue = if (call.argv.len == 0)
                &.{}
            else
                @as([*]const JSValue, @ptrCast(call.argv.ptr))[0..call.argv.len];

            _ = fn_value.call(global, this_value, args) catch {
                // Clear any pending exception so the event loop stays healthy.
                global.clearException();
            };
        }
    }

    pub fn run(this: *TickContext) void {
        const vm = this.runtime.vm;

        drainPendingCalls(this.runtime, vm.global);

        // Non-blocking tick: process ready tasks + microtasks
        vm.eventLoop().tick();

        // Also do a non-blocking I/O poll (kqueue/epoll with zero timeout)
        if (vm.event_loop_handle) |loop| {
            loop.tickWithoutIdle();
        }

        // Process any tasks that became ready from I/O
        vm.eventLoop().tick();

        this.has_pending = vm.isEventLoopAlive();
    }
};

pub export fn bun_get_event_fd(rt: ?*BunRuntime) callconv(.c) c_int {
    const runtime = rt orelse return -1;
    if (comptime Environment.isPosix) {
        if (runtime.vm.event_loop_handle) |loop| {
            return loop.fd;
        }
    }
    return -1;
}

pub export fn bun_wakeup(rt: ?*BunRuntime) callconv(.c) void {
    const runtime = rt orelse return;
    if (runtime.vm.event_loop_handle) |loop| {
        loop.wakeup();
    }
}

// ---------------------------------------------------------------------------
// Value Creation
// ---------------------------------------------------------------------------

/// Must stay in sync with BunTypedArrayKind in bun_embed.h and BunEmbed.cpp.
const BunTypedArrayKind = enum(u32) {
    int8 = 0,
    uint8 = 1,
    uint8c = 2,
    int16 = 3,
    uint16 = 4,
    int32 = 5,
    uint32 = 6,
    float32 = 7,
    float64 = 8,
    bigint64 = 9,
    biguint64 = 10,
};

pub export fn bun_bool(value: c_int) callconv(.c) BunValue {
    return toBunValue(JSValue.jsBoolean(value != 0));
}

pub export fn bun_number(value: f64) callconv(.c) BunValue {
    return toBunValue(JSValue.jsDoubleNumber(value));
}

pub export fn bun_int32(value: i32) callconv(.c) BunValue {
    return toBunValue(JSValue.jsNumberFromInt32(value));
}

pub export fn bun_string(ctx: ?*BunContext, utf8: ?[*]const u8, len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const ptr = utf8 orelse "";
    return toBunValue(BunString__createUTF8ForJS(global, ptr, len));
}

pub export fn bun_object(ctx: ?*BunContext) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    return toBunValue(JSValue.createEmptyObject(global, 0));
}

pub export fn bun_array(ctx: ?*BunContext, len: usize) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const value = JSValue.createEmptyArray(global, len) catch return toBunValue(.js_undefined);
    return toBunValue(value);
}

pub export fn bun_global(ctx: ?*BunContext) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    return toBunValue(global.toJSValue());
}

fn hostFnTrampoline(global: *JSGlobalObject, callframe: *jsc.CallFrame) callconv(.c) JSValue {
    const callee = callframe.callee();
    // Look up in the runtime's per-instance registry via the VM pointer.
    // We find the runtime by casting the VM's globalObject back: the VM is
    // embedded in BunRuntime so we stored a pointer in HostFnData itself.
    // All we need is the registry entry from the callee JSValue.
    //
    // To locate the right runtime we stored a back-pointer in HostFnData.
    // We first peek into the global's VM to find our runtime.  Since
    // BunRuntime owns the VirtualMachine we can walk it via global.bunVM().
    const vm = global.bunVM();

    // Find the BunRuntime that owns this VM.  We stored the runtime pointer
    // in each HostFnData so we just need to look up by callee value.
    // The trampoline is shared across all bun_function() calls; the callee
    // JSValue uniquely identifies which HostFnData to use.
    //
    // We need the runtime pointer to access host_fn_registry.  We stored it
    // in HostFnData.runtime so we can recover it once we have the entry — but
    // we need the registry first.  Bootstrap: keep a small thread-local cache
    // mapping vm → *BunRuntime.
    const runtime = vmToRuntime(vm) orelse return .js_undefined;
    const cb_data = runtime.host_fn_registry.get(callee) orelse return .js_undefined;
    const args = callframe.arguments();

    const result = cb_data.native_fn(
        @ptrCast(global),
        @intCast(args.len),
        if (args.len == 0) null else @as([*]const BunValue, @ptrCast(args.ptr)),
        cb_data.userdata,
    );
    return toJSValue(result);
}

/// GC finalizer called when a bun_function() JS function is collected.
/// Removes HostFnData from the registry and frees the allocation.
fn hostFnFinalizer(userdata: ?*anyopaque) callconv(.c) void {
    const cb_data: *HostFnData = @ptrCast(@alignCast(userdata orelse return));
    const runtime = cb_data.runtime;
    _ = runtime.host_fn_registry.remove(cb_data.js_fn);
    bun.default_allocator.destroy(cb_data);
}

pub export fn bun_function(
    ctx: ?*BunContext,
    name_ptr: ?[*:0]const u8,
    native_fn: ?BunHostFn,
    userdata: ?*anyopaque,
    arg_count: c_int,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const name = if (name_ptr) |p| std.mem.span(p) else return toBunValue(.js_undefined);
    const fn_ptr = native_fn orelse return toBunValue(.js_undefined);
    const runtime = vmToRuntime(global.bunVM()) orelse return toBunValue(.js_undefined);

    const js_fn = JSFunction__createFromZig(
        global,
        bun.String.init(name),
        hostFnTrampoline,
        if (arg_count >= 0) @intCast(arg_count) else 0,
        0,
        0,
        null,
    );
    if (js_fn == .zero) return toBunValue(.js_undefined);

    const cb_data = bun.default_allocator.create(HostFnData) catch return toBunValue(.js_undefined);
    cb_data.* = .{
        .native_fn = fn_ptr,
        .userdata = userdata,
        .runtime = runtime,
        .js_fn = js_fn,
    };

    runtime.host_fn_registry.put(bun.default_allocator, js_fn, cb_data) catch {
        bun.default_allocator.destroy(cb_data);
        return toBunValue(.js_undefined);
    };

    // Attach a GC finalizer so HostFnData is freed and the registry entry is
    // removed when the JS function is garbage-collected.
    if (!BunEmbed__defineFinalizer(global, js_fn, hostFnFinalizer, cb_data)) {
        // Finalizer failed — still usable, but will leak on GC (only freed at bun_destroy).
        // This is acceptable degraded behaviour rather than failing the whole call.
    }

    return toBunValue(js_fn);
}

pub export fn bun_array_buffer(
    ctx: ?*BunContext,
    data: ?*anyopaque,
    len: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const result = BunEmbed__createArrayBuffer(global, data, len, finalizer, userdata);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_typed_array(
    ctx: ?*BunContext,
    kind: u32,
    data: ?*anyopaque,
    element_count: usize,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const result = BunEmbed__createTypedArray(global, kind, data, element_count, finalizer, userdata);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_get_array_buffer(
    ctx: ?*BunContext,
    value: BunValue,
    out: ?*BunArrayBufferInfo,
) callconv(.c) c_int {
    const info = out orelse return 0;
    info.* = .{
        .data = null,
        .byte_length = 0,
    };

    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__getArrayBuffer(global, toJSValue(value), info)) 1 else 0;
}

pub export fn bun_get_typed_array(
    ctx: ?*BunContext,
    value: BunValue,
    out: ?*BunTypedArrayInfo,
) callconv(.c) c_int {
    const info = out orelse return 0;
    info.* = .{
        .data = null,
        .byte_offset = 0,
        .byte_length = 0,
        .element_count = 0,
        .kind = 0,
    };

    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__getTypedArray(global, toJSValue(value), info)) 1 else 0;
}

pub export fn bun_class_register(
    ctx: ?*BunContext,
    descriptor: ?*const BunClassDescriptor,
    parent: ?*BunClass,
) callconv(.c) ?*BunClass {
    const global = toGlobal(ctx) orelse return null;
    const class_descriptor = descriptor orelse return null;
    const runtime = vmToRuntime(global.bunVM()) orelse return null;

    const class_handle = BunEmbed__registerClass(global, class_descriptor, parent) orelse return null;
    runtime.class_registry.append(bun.default_allocator, class_handle) catch {
        BunEmbed__destroyClass(class_handle);
        return null;
    };
    return class_handle;
}

pub export fn bun_class_new(
    ctx: ?*BunContext,
    class_handle: ?*BunClass,
    native_ptr: ?*anyopaque,
    finalizer: ?BunClassFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const klass = class_handle orelse return toBunValue(.js_undefined);
    const result = BunEmbed__createClassInstance(global, klass, native_ptr, finalizer, userdata);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

pub export fn bun_class_unwrap(
    ctx: ?*BunContext,
    value: BunValue,
    class_handle: ?*BunClass,
) callconv(.c) ?*anyopaque {
    const global = toGlobal(ctx) orelse return null;
    return BunEmbed__unwrapClassInstance(global, toJSValue(value), class_handle);
}

pub export fn bun_is_class_instance(ctx: ?*BunContext, value: BunValue) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__isClassInstance(global, toJSValue(value))) 1 else 0;
}

pub export fn bun_instanceof_class(
    ctx: ?*BunContext,
    value: BunValue,
    class_handle: ?*BunClass,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const klass = class_handle orelse return 0;
    return if (BunEmbed__instanceofClass(global, toJSValue(value), klass)) 1 else 0;
}

pub export fn bun_class_dispose(ctx: ?*BunContext, value: BunValue) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    return if (BunEmbed__disposeClassInstance(global, toJSValue(value))) 1 else 0;
}

pub export fn bun_class_prototype(ctx: ?*BunContext, class_handle: ?*BunClass) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const klass = class_handle orelse return toBunValue(.js_undefined);
    const result = BunEmbed__classPrototype(global, klass);
    return if (result == .zero) toBunValue(.js_undefined) else toBunValue(result);
}

// ---------------------------------------------------------------------------
// Value Introspection & Conversion
// ---------------------------------------------------------------------------

pub export fn bun_is_undefined(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isUndefined()) 1 else 0;
}

pub export fn bun_is_null(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isNull()) 1 else 0;
}

pub export fn bun_is_bool(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isBoolean()) 1 else 0;
}

pub export fn bun_is_number(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isNumber()) 1 else 0;
}

pub export fn bun_is_string(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isString()) 1 else 0;
}

pub export fn bun_is_object(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isObject()) 1 else 0;
}

pub export fn bun_is_callable(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).isCallable()) 1 else 0;
}

pub export fn bun_to_bool(value: BunValue) callconv(.c) c_int {
    return if (toJSValue(value).toBoolean()) 1 else 0;
}

pub export fn bun_to_number(ctx: ?*BunContext, value: BunValue) callconv(.c) f64 {
    const global = toGlobal(ctx) orelse return std.math.nan(f64);
    return toJSValue(value).toNumber(global) catch std.math.nan(f64);
}

pub export fn bun_to_int32(value: BunValue) callconv(.c) i32 {
    return toJSValue(value).toInt32();
}

pub export fn bun_to_utf8(ctx: ?*BunContext, value: BunValue, out_len: ?*usize) callconv(.c) ?[*:0]u8 {
    const global = toGlobal(ctx) orelse return null;
    const slice = toJSValue(value).toSlice(global, bun.default_allocator) catch return null;
    defer slice.deinit();

    const out_ptr = std.c.malloc(slice.len + 1) orelse return null;
    const out_buf: [*]u8 = @ptrCast(out_ptr);
    if (slice.len > 0) {
        @memcpy(out_buf[0..slice.len], slice.ptr[0..slice.len]);
    }
    out_buf[slice.len] = 0;

    if (out_len) |len_ptr| {
        len_ptr.* = slice.len;
    }

    return @ptrCast(out_buf);
}

// ---------------------------------------------------------------------------
// Object & Property Operations
// ---------------------------------------------------------------------------

pub export fn bun_set(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    value: BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const key = if (key_ptr) |p| p[0..key_len] else return 0;
    const obj = toJSValue(object);
    if (!obj.isObject()) return 0;

    obj.put(global, key, toJSValue(value));
    return 1;
}

pub export fn bun_get(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const key = if (key_ptr) |p| p[0..key_len] else return toBunValue(.js_undefined);
    const obj = toJSValue(object);
    if (!obj.isObject()) return toBunValue(.js_undefined);

    const value = obj.getPropertyValue(global, key) catch return toBunValue(.js_undefined);
    return if (value) |v| toBunValue(v) else toBunValue(.js_undefined);
}

pub export fn bun_set_index(
    ctx: ?*BunContext,
    object: BunValue,
    index: u32,
    value: BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const obj = toJSValue(object);
    if (!obj.isObject()) return 0;

    obj.putIndex(global, index, toJSValue(value)) catch return 0;
    return 1;
}

pub export fn bun_get_index(ctx: ?*BunContext, object: BunValue, index: u32) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return toBunValue(.js_undefined);
    const obj = toJSValue(object);
    if (!obj.isObject()) return toBunValue(.js_undefined);

    const value = obj.getIndex(global, index) catch return toBunValue(.js_undefined);
    return toBunValue(value);
}

pub export fn bun_define_getter(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    getter: ?BunGetterFn,
    dont_enum: c_int,
    dont_delete: c_int,
) callconv(.c) c_int {
    return bun_define_accessor(
        ctx,
        object,
        key_ptr,
        key_len,
        getter,
        null,
        1,
        dont_enum,
        dont_delete,
    );
}

pub export fn bun_define_setter(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    setter: ?BunSetterFn,
    dont_enum: c_int,
    dont_delete: c_int,
) callconv(.c) c_int {
    return bun_define_accessor(
        ctx,
        object,
        key_ptr,
        key_len,
        null,
        setter,
        0,
        dont_enum,
        dont_delete,
    );
}

pub export fn bun_define_accessor(
    ctx: ?*BunContext,
    object: BunValue,
    key_ptr: ?[*]const u8,
    key_len: usize,
    getter: ?BunGetterFn,
    setter: ?BunSetterFn,
    read_only: c_int,
    dont_enum: c_int,
    dont_delete: c_int,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const key = key_ptr orelse return 0;

    var flags: u32 = 0;
    if (read_only != 0) flags |= BUN_ACCESSOR_READ_ONLY;
    if (dont_enum != 0) flags |= BUN_ACCESSOR_DONT_ENUM;
    if (dont_delete != 0) flags |= BUN_ACCESSOR_DONT_DELETE;

    return if (BunEmbed__defineCustomAccessor(global, toJSValue(object), key, key_len, getter, setter, flags)) 1 else 0;
}

pub export fn bun_define_finalizer(
    ctx: ?*BunContext,
    object: BunValue,
    finalizer: ?BunFinalizerFn,
    userdata: ?*anyopaque,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const callback = finalizer orelse return 0;
    const obj = toJSValue(object);
    if (!obj.isObject()) return 0;
    const runtime = vmToRuntime(global.bunVM()) orelse return 0;

    // Use the opaque_map to guard against double-attachment.
    const result = runtime.opaque_map.getOrPut(bun.default_allocator, obj) catch return 0;
    if (result.found_existing and result.value_ptr.finalizer_attached) return 0;
    if (!result.found_existing) result.value_ptr.* = .{};
    result.value_ptr.finalizer_attached = true;

    return if (BunEmbed__defineFinalizer(global, obj, callback, userdata)) 1 else 0;
}

pub export fn bun_set_prototype(ctx: ?*BunContext, object: BunValue, proto: BunValue) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const obj = toJSValue(object);
    const prototype = toJSValue(proto);

    if (!obj.isObject()) return 0;
    if (!prototype.isObject() and !prototype.isNull()) return 0;

    obj.setPrototypeDirect(prototype, global) catch return 0;
    return 1;
}

pub export fn bun_set_opaque(ctx: ?*BunContext, object: BunValue, opaque_ptr: ?*anyopaque) callconv(.c) void {
    const global = toGlobal(ctx) orelse return;
    const obj = toJSValue(object);
    if (!obj.isObject()) return;
    const runtime = vmToRuntime(global.bunVM()) orelse return;

    const result = runtime.opaque_map.getOrPut(bun.default_allocator, obj) catch return;
    if (!result.found_existing) result.value_ptr.* = .{};
    result.value_ptr.opaque_ptr = opaque_ptr;
}

pub export fn bun_get_opaque(ctx: ?*BunContext, object: BunValue) callconv(.c) ?*anyopaque {
    const global = toGlobal(ctx) orelse return null;
    const obj = toJSValue(object);
    if (!obj.isObject()) return null;
    const runtime = vmToRuntime(global.bunVM()) orelse return null;

    const entry = runtime.opaque_map.get(obj) orelse return null;
    return entry.opaque_ptr;
}

// ---------------------------------------------------------------------------
// Function Call & GC Lifetime
// ---------------------------------------------------------------------------

pub export fn bun_call(
    ctx: ?*BunContext,
    fn_value: BunValue,
    this_value: BunValue,
    argc: c_int,
    argv: ?[*]const BunValue,
) callconv(.c) BunValue {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = vmToRuntime(global.bunVM());
    // Clear any stale error so bun_last_error() is NULL after a successful call.
    if (runtime) |rt| rt.freeLastError();

    const function = toJSValue(fn_value);
    if (!function.isCallable()) return 0;

    const argc_u: usize = if (argc > 0) @intCast(argc) else 0;
    const args: []const JSValue = if (argc_u == 0)
        &.{}
    else if (argv) |p|
        @as([*]const JSValue, @ptrCast(p))[0..argc_u]
    else
        return 0;

    const result = function.call(global, toJSValue(this_value), args) catch {
        // Capture the exception message into last_error_buf so the caller
        // can retrieve it with bun_last_error().
        if (runtime) |rt| {
            if (global.tryTakeException()) |exc| {
                _ = rt.captureException(global, exc);
            }
        } else {
            global.clearException();
        }
        return 0; // BUN_EXCEPTION sentinel
    };
    return toBunValue(result);
}

/// Return the latest error string stored by bun_call() or bun_eval*().
pub export fn bun_last_error(ctx: ?*BunContext) callconv(.c) ?[*:0]const u8 {
    const global = toGlobal(ctx) orelse return null;
    const runtime = vmToRuntime(global.bunVM()) orelse return null;
    return runtime.last_error_buf;
}

pub export fn bun_call_async(
    ctx: ?*BunContext,
    fn_value: BunValue,
    this_value: BunValue,
    argc: c_int,
    argv: ?[*]const BunValue,
) callconv(.c) c_int {
    const global = toGlobal(ctx) orelse return 0;
    const runtime = vmToRuntime(global.bunVM()) orelse return 0;
    const argc_u: usize = if (argc > 0) @intCast(argc) else 0;

    var arg_copy: []BunValue = &.{};
    if (argc_u > 0) {
        const src = argv orelse return 0;
        arg_copy = bun.default_allocator.alloc(BunValue, argc_u) catch return 0;
        @memcpy(arg_copy, src[0..argc_u]);
    }

    runtime.pending_calls_mutex.lock();
    defer runtime.pending_calls_mutex.unlock();

    runtime.pending_calls.append(bun.default_allocator, .{
        .fn_value = fn_value,
        .this_value = this_value,
        .argv = arg_copy,
    }) catch {
        if (arg_copy.len > 0) bun.default_allocator.free(arg_copy);
        return 0;
    };

    if (runtime.vm.event_loop_handle) |loop| {
        loop.wakeup();
    }

    return 1;
}

pub export fn bun_protect(_: ?*BunContext, value: BunValue) callconv(.c) void {
    toJSValue(value).protect();
}

pub export fn bun_unprotect(_: ?*BunContext, value: BunValue) callconv(.c) void {
    toJSValue(value).unprotect();
}
comptime {
    _ = &bun_initialize;
    _ = &bun_destroy;
    _ = &bun_context;
    _ = &bun_eval_string;
    _ = &bun_eval_file;
    _ = &bun_run_pending_jobs;
    _ = &bun_get_event_fd;
    _ = &bun_wakeup;
    _ = &bun_bool;
    _ = &bun_number;
    _ = &bun_int32;
    _ = &bun_string;
    _ = &bun_object;
    _ = &bun_array;
    _ = &bun_global;
    _ = &bun_function;
    _ = &bun_array_buffer;
    _ = &bun_typed_array;
    _ = &bun_get_array_buffer;
    _ = &bun_get_typed_array;
    _ = &bun_class_register;
    _ = &bun_class_new;
    _ = &bun_class_unwrap;
    _ = &bun_is_class_instance;
    _ = &bun_instanceof_class;
    _ = &bun_class_dispose;
    _ = &bun_class_prototype;
    _ = &bun_is_undefined;
    _ = &bun_is_null;
    _ = &bun_is_bool;
    _ = &bun_is_number;
    _ = &bun_is_string;
    _ = &bun_is_object;
    _ = &bun_is_callable;
    _ = &bun_to_bool;
    _ = &bun_to_number;
    _ = &bun_to_int32;
    _ = &bun_to_utf8;
    _ = &bun_set;
    _ = &bun_get;
    _ = &bun_set_index;
    _ = &bun_get_index;
    _ = &bun_define_getter;
    _ = &bun_define_setter;
    _ = &bun_define_accessor;
    _ = &bun_define_finalizer;
    _ = &bun_set_opaque;
    _ = &bun_get_opaque;
    _ = &bun_call;
    _ = &bun_last_error;
    _ = &bun_call_async;
    _ = &bun_protect;
    _ = &bun_unprotect;
}

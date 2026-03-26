///! bun_embed.zig — Minimal C API for embedding Bun's JS runtime into a host application.
///! See bun_embed.h for the C API documentation.
const std = @import("std");
const bun = @import("bun");
const jsc = bun.jsc;
const uws = bun.uws;
const js_ast = bun.ast;
const logger = bun.logger;
const Arena = bun.allocators.MimallocArena;
const VirtualMachine = jsc.VirtualMachine;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const JSModuleLoader = jsc.JSModuleLoader;
const EventLoop = jsc.EventLoop;
const Environment = bun.Environment;
const Output = bun.Output;
const Config = @import("../bun.js/config.zig");
const api = bun.schema.api;
const DNSResolver = bun.api.dns.Resolver;

/// Opaque runtime handle exposed to C as `BunRuntime*`.
const BunRuntime = struct {
    vm: *VirtualMachine,
    arena: Arena,
    /// Scratch buffer for returning error strings to C callers. Valid until next eval call.
    last_error_buf: ?[*:0]u8 = null,

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
};

/// Result struct matching the C BunEvalResult layout.
const BunEvalResult = extern struct {
    success: c_int,
    @"error": ?[*:0]const u8,
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

pub export fn bun_initialize(cwd_ptr: ?[*:0]const u8) callconv(.c) ?*BunRuntime {
    return initializeImpl(cwd_ptr) catch null;
}

fn initializeImpl(cwd_ptr: ?[*:0]const u8) !?*BunRuntime {
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
    if (cwd_ptr) |cwd| {
        args.absolute_working_dir = std.mem.span(cwd);
    }

    // Create the VM
    const vm = try VirtualMachine.init(.{
        .allocator = allocator,
        .args = args,
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

    return rt;
}

pub export fn bun_destroy(rt: ?*BunRuntime) callconv(.c) void {
    const runtime = rt orelse return;
    runtime.freeLastError();
    runtime.vm.onExit();
    bun.default_allocator.destroy(runtime);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

pub export fn bun_eval_string(rt: ?*BunRuntime, code_ptr: ?[*:0]const u8) callconv(.c) BunEvalResult {
    const runtime = rt orelse return .{ .success = 0, .@"error" = "null runtime" };
    runtime.freeLastError();

    const code = if (code_ptr) |p| std.mem.span(p) else return .{ .success = 0, .@"error" = "null code" };

    var eval_ctx = EvalContext{
        .runtime = runtime,
        .code = code,
        .result = .{ .success = 0, .@"error" = "eval did not complete" },
    };
    runtime.vm.runWithAPILock(EvalContext, &eval_ctx, EvalContext.run);
    return eval_ctx.result;
}

const EvalContext = struct {
    runtime: *BunRuntime,
    code: []const u8,
    result: BunEvalResult,

    pub fn run(this: *EvalContext) void {
        const vm = this.runtime.vm;
        const global = vm.global;

        var exception: [1]JSValue = .{.js_undefined};
        const ret = JSModuleLoader.evaluate(
            global,
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
            this.result = this.runtime.captureException(global, exception[0]);
        } else if (ret == .zero) {
            this.result = .{ .success = 0, .@"error" = "evaluation returned null" };
        } else {
            this.result = .{ .success = 1, .@"error" = null };
        }
    }
};

pub export fn bun_eval_file(rt: ?*BunRuntime, path_ptr: ?[*:0]const u8) callconv(.c) BunEvalResult {
    const runtime = rt orelse return .{ .success = 0, .@"error" = "null runtime" };
    runtime.freeLastError();

    const path = if (path_ptr) |p| std.mem.span(p) else return .{ .success = 0, .@"error" = "null path" };

    var eval_ctx = EvalFileContext{
        .runtime = runtime,
        .path = path,
        .result = .{ .success = 0, .@"error" = "eval_file did not complete" },
    };
    runtime.vm.runWithAPILock(EvalFileContext, &eval_ctx, EvalFileContext.run);
    return eval_ctx.result;
}

const EvalFileContext = struct {
    runtime: *BunRuntime,
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
                this.result = this.runtime.captureException(vm.global, rejection);
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

    pub fn run(this: *TickContext) void {
        const vm = this.runtime.vm;
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
// Native Function Injection
// ---------------------------------------------------------------------------

/// C callback type: char* (*)(int argc, const char** argv, void* userdata)
const BunNativeFunction = *const fn (c_int, ?[*]const ?[*:0]const u8, ?*anyopaque) callconv(.c) ?[*:0]u8;

pub export fn bun_register_native_function(
    rt: ?*BunRuntime,
    name_ptr: ?[*:0]const u8,
    native_fn: ?BunNativeFunction,
    userdata: ?*anyopaque,
) callconv(.c) c_int {
    const runtime = rt orelse return 0;
    const name = if (name_ptr) |p| std.mem.span(p) else return 0;
    const fn_ptr = native_fn orelse return 0;

    var ctx = InjectContext{
        .runtime = runtime,
        .name = name,
        .native_fn = fn_ptr,
        .userdata = userdata,
        .success = false,
    };
    runtime.vm.runWithAPILock(InjectContext, &ctx, InjectContext.run);
    return if (ctx.success) 1 else 0;
}

const InjectContext = struct {
    runtime: *BunRuntime,
    name: []const u8,
    native_fn: BunNativeFunction,
    userdata: ?*anyopaque,
    success: bool,

    pub fn run(this: *InjectContext) void {
        const vm = this.runtime.vm;
        const global = vm.global;

        // Store the context pointer in a NativeCallbackData allocated with default_allocator
        const cb_data = bun.default_allocator.create(NativeCallbackData) catch return;
        cb_data.* = .{
            .native_fn = this.native_fn,
            .userdata = this.userdata,
        };

        // Create a JS function that wraps our C callback
        const js_fn = jsc.JSFunction.create(
            global,
            this.name,
            nativeCallbackTrampoline,
            0, // variadic
            .{},
        );

        if (js_fn == .zero) return;

        // Store the callback data as internal field via a weak map or property
        // We use putDirect on the function object to store callback data
        // Actually, for simplicity, use a static registry keyed by the JSValue
        nativeCallbackRegistry.put(bun.default_allocator, js_fn, cb_data) catch return;

        // Put the function on globalThis
        const global_this = global.toJSValue();
        global_this.put(global, this.name, js_fn);

        this.success = true;
    }
};

/// Registry mapping JS function values to their native callback data.
var nativeCallbackRegistry: NativeCallbackMap = .{};
const NativeCallbackMap = std.AutoHashMapUnmanaged(jsc.JSValue, *NativeCallbackData);

const NativeCallbackData = struct {
    native_fn: BunNativeFunction,
    userdata: ?*anyopaque,
};

/// Trampoline called by JSC when the injected JS function is invoked.
fn nativeCallbackTrampoline(global: *JSGlobalObject, callframe: *jsc.CallFrame) bun.JSError!JSValue {
    // Look up the callback data
    const callee = callframe.callee();
    const cb_data = nativeCallbackRegistry.get(callee) orelse
        return .js_undefined;

    const args = callframe.arguments();
    const argc: usize = @min(args.len, 16);

    // Convert JS arguments to JSON strings for the C callback
    var c_argv_buf: [16]?[*:0]const u8 = @splat(null);
    var json_strs: [16]bun.String = @splat(bun.String.empty);
    var utf8_slices: [16]jsc.ZigString.Slice = @splat(.{});

    for (args[0..argc], 0..argc) |arg, i| {
        // Convert each argument to a JSON string
        try arg.jsonStringifyFast(global, &json_strs[i]);
        if (json_strs[i].isEmpty()) {
            c_argv_buf[i] = "null";
        } else {
            // Get a UTF8 slice and null-terminate it
            utf8_slices[i] = json_strs[i].toUTF8(bun.default_allocator);
            // Allocate a null-terminated copy for the C callback
            const slice_data = utf8_slices[i].ptr[0..utf8_slices[i].len];
            const z = bun.default_allocator.allocSentinel(u8, slice_data.len, 0) catch {
                c_argv_buf[i] = "null";
                continue;
            };
            @memcpy(z[0..slice_data.len], slice_data);
            c_argv_buf[i] = z;
        }
    }

    // Call the native function
    const result_ptr = cb_data.native_fn(
        @intCast(argc),
        if (argc > 0) &c_argv_buf else null,
        cb_data.userdata,
    );

    // Clean up JSON strings and allocated null-terminated copies
    for (0..argc) |i| {
        if (json_strs[i].isEmpty()) continue;
        json_strs[i].deref();
        utf8_slices[i].deinit();
        // Free the null-terminated copy if it was allocated
        if (c_argv_buf[i]) |p| {
            if (p != @as(?[*:0]const u8, "null")) {
                bun.default_allocator.free(std.mem.span(p));
            }
        }
    }

    // Parse the return value (JSON string from C)
    if (result_ptr) |ret| {
        const ret_span = std.mem.span(ret);
        defer std.c.free(@ptrCast(@constCast(ret)));

        // Create a bun.String from the returned UTF8 JSON, then parse it as JSON
        var json_str = bun.String.fromBytes(ret_span);
        return json_str.toJSByParseJSON(global) catch return .js_undefined;
    }

    return .js_undefined;
}

pub export fn bun_register_native_function_raw(
    rt: ?*BunRuntime,
    name_ptr: ?[*:0]const u8,
    fn_ptr: ?*anyopaque,
    arg_count: c_int,
) callconv(.c) c_int {
    const runtime = rt orelse return 0;
    const name = if (name_ptr) |p| std.mem.span(p) else return 0;
    const raw_fn = fn_ptr orelse return 0;

    var ctx = InjectRawContext{
        .runtime = runtime,
        .name = name,
        .raw_fn = @ptrCast(@alignCast(raw_fn)),
        .arg_count = if (arg_count >= 0) @intCast(arg_count) else 0,
        .success = false,
    };
    runtime.vm.runWithAPILock(InjectRawContext, &ctx, InjectRawContext.run);
    return if (ctx.success) 1 else 0;
}

/// Extern declaration for creating JSFunction with a runtime function pointer.
extern fn JSFunction__createFromZig(
    global: *JSGlobalObject,
    fn_name: bun.String,
    implementation: *const jsc.JSHostFn,
    arg_count: u32,
    implementation_visibility: u8,
    intrinsic: u8,
    constructor: ?*const jsc.JSHostFn,
) JSValue;

const InjectRawContext = struct {
    runtime: *BunRuntime,
    name: []const u8,
    raw_fn: *const jsc.JSHostFn,
    arg_count: u32,
    success: bool,

    pub fn run(this: *InjectRawContext) void {
        const vm = this.runtime.vm;
        const global = vm.global;

        const js_fn = JSFunction__createFromZig(
            global,
            bun.String.init(this.name),
            this.raw_fn,
            this.arg_count,
            0, // public
            0, // no intrinsic
            null,
        );

        if (js_fn == .zero) return;

        const global_this = global.toJSValue();
        global_this.put(global, this.name, js_fn);

        this.success = true;
    }
};

// Force export all symbols by referencing them
comptime {
    _ = &bun_initialize;
    _ = &bun_destroy;
    _ = &bun_eval_string;
    _ = &bun_eval_file;
    _ = &bun_run_pending_jobs;
    _ = &bun_get_event_fd;
    _ = &bun_wakeup;
    _ = &bun_register_native_function;
    _ = &bun_register_native_function_raw;
}

/// example.c — Demonstrates the zero-copy BunValue embedding API.
///
/// Build (assuming bun is built as a shared library):
///   cc -o example example.c -L<bun-lib-dir> -lbun -I.
///
/// This file is for illustration only and is NOT compiled as part of Bun.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bun_embed.h"

typedef struct {
    int value;
} Counter;

typedef struct {
    int x;
    int y;
} NativeView;

typedef struct {
    NativeView view;
    char text[64];
} NativeText;

static BunValue view_get_x(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    return view ? bun_int32(view->x) : BUN_UNDEFINED;
}

static void view_set_x(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    if (!view) return;
    view->x = bun_to_int32(value);
}

static BunValue view_get_y(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    return view ? bun_int32(view->y) : BUN_UNDEFINED;
}

static void view_set_y(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    if (!view) return;
    view->y = bun_to_int32(value);
}

static BunValue view_move_by(BunContext* ctx, BunValue this_value, void* native_ptr, int argc, const BunValue* argv, void* userdata)
{
    (void)this_value;
    (void)userdata;
    NativeView* view = (NativeView*)native_ptr;
    if (!view) return BUN_UNDEFINED;

    if (argc >= 1 && argv) view->x += bun_to_int32(argv[0]);
    if (argc >= 2 && argv) view->y += bun_to_int32(argv[1]);

    BunValue result = bun_object(ctx);
    bun_set(ctx, result, "x", 1, bun_int32(view->x));
    bun_set(ctx, result, "y", 1, bun_int32(view->y));
    return result;
}

static BunValue text_get_content(BunContext* ctx, BunValue this_value, void* native_ptr, void* userdata)
{
    (void)this_value;
    (void)userdata;
    NativeText* text = (NativeText*)native_ptr;
    if (!text) return BUN_UNDEFINED;
    return bun_string(ctx, text->text, strlen(text->text));
}

static void text_set_content(BunContext* ctx, BunValue this_value, void* native_ptr, BunValue value, void* userdata)
{
    (void)this_value;
    (void)userdata;
    NativeText* text = (NativeText*)native_ptr;
    if (!text) return;

    size_t len = 0;
    char* utf8 = bun_to_utf8(ctx, value, &len);
    if (!utf8) return;

    if (len >= sizeof(text->text)) len = sizeof(text->text) - 1;
    memcpy(text->text, utf8, len);
    text->text[len] = '\0';
    free(utf8);
}

static BunValue text_measure(BunContext* ctx, BunValue this_value, void* native_ptr, int argc, const BunValue* argv, void* userdata)
{
    (void)ctx;
    (void)this_value;
    (void)argc;
    (void)argv;
    (void)userdata;
    NativeText* text = (NativeText*)native_ptr;
    return text ? bun_int32((int32_t)strlen(text->text)) : BUN_UNDEFINED;
}

static void native_view_finalize(void* native_ptr, void* userdata)
{
    const char* class_name = (const char*)userdata;
    printf("  [class finalizer] freeing %s at %p\n", class_name ? class_name : "instance", native_ptr);
    free(native_ptr);
}

static const BunClassPropertyDescriptor VIEW_PROPERTIES[] = {
    { "x", 1, view_get_x, view_set_x, NULL, 0, 0, 0 },
    { "y", 1, view_get_y, view_set_y, NULL, 0, 0, 0 },
};

static const BunClassMethodDescriptor VIEW_METHODS[] = {
    { "moveBy", 6, view_move_by, NULL, 2, 0, 0 },
};

static const BunClassDescriptor VIEW_CLASS = {
    "View",
    4,
    VIEW_PROPERTIES,
    sizeof(VIEW_PROPERTIES) / sizeof(VIEW_PROPERTIES[0]),
    VIEW_METHODS,
    sizeof(VIEW_METHODS) / sizeof(VIEW_METHODS[0]),
};

static const BunClassPropertyDescriptor TEXT_PROPERTIES[] = {
    { "text", 4, text_get_content, text_set_content, NULL, 0, 0, 0 },
};

static const BunClassMethodDescriptor TEXT_METHODS[] = {
    { "measure", 7, text_measure, NULL, 0, 0, 0 },
};

static const BunClassDescriptor TEXT_CLASS = {
    "Text",
    4,
    TEXT_PROPERTIES,
    sizeof(TEXT_PROPERTIES) / sizeof(TEXT_PROPERTIES[0]),
    TEXT_METHODS,
    sizeof(TEXT_METHODS) / sizeof(TEXT_METHODS[0]),
};

static void counter_finalize(void* userdata)
{
    Counter* counter = (Counter*)userdata;
    free(counter);
}

static BunValue native_add(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)ctx;
    (void)userdata;
    if (argc < 2 || !argv) return BUN_UNDEFINED;

    double a = bun_to_number(ctx, argv[0]);
    double b = bun_to_number(ctx, argv[1]);
    return bun_number(a + b);
}

static BunValue counter_inc(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)argc;
    (void)argv;
    (void)userdata;

    BunValue self = bun_get(ctx, bun_global(ctx), "counter", 7);
    Counter* counter = (Counter*)bun_get_opaque(ctx, self);
    if (!counter) return BUN_UNDEFINED;

    counter->value += 1;
    return bun_int32(counter->value);
}

static BunValue counter_get(BunContext* ctx, BunValue this_value)
{
    Counter* counter = (Counter*)bun_get_opaque(ctx, this_value);
    if (!counter) return BUN_UNDEFINED;
    return bun_int32(counter->value);
}

static void counter_set(BunContext* ctx, BunValue this_value, BunValue value)
{
    Counter* counter = (Counter*)bun_get_opaque(ctx, this_value);
    if (!counter) return;
    counter->value = bun_to_int32(value);
}

static BunValue native_greet(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;

    const char* default_name = "World";
    const char* name = default_name;
    char* owned_name = NULL;
    size_t owned_len = 0;

    if (argc >= 1 && argv) {
        owned_name = bun_to_utf8(ctx, argv[0], &owned_len);
        if (owned_name && owned_len > 0) name = owned_name;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Hello, %s!", name);

    if (owned_name) free(owned_name);
    return bun_string(ctx, buf, strlen(buf));
}

static void float_buffer_finalize(void* userdata)
{
    float* buf = (float*)userdata;
    printf("  [finalizer] freeing float buffer at %p\n", (void*)buf);
    free(buf);
}

static BunValue native_sum_typed(BunContext* ctx, int argc, const BunValue* argv, void* userdata)
{
    (void)userdata;
    /* Expects one Float32Array argument. Returns the sum of all elements. */
    if (argc < 1 || !argv) return bun_number(0.0);
    /* For the demo we just return the buffer length as a proxy for success */
    (void)ctx;
    return BUN_UNDEFINED; /* simplified — real code would read the typed array */
}

int main(void)
{
    printf("Initializing Bun runtime...\n");

    BunRuntime* rt = bun_initialize(NULL);
    if (!rt) {
        fprintf(stderr, "Failed to initialize Bun runtime\n");
        return 1;
    }

    BunContext* ctx = bun_context(rt);
    BunValue global = bun_global(ctx);

    BunValue add_fn = bun_function(ctx, "nativeAdd", native_add, NULL, 2);
    BunValue greet_fn = bun_function(ctx, "nativeGreet", native_greet, NULL, 1);

    bun_set(ctx, global, "nativeAdd", 9, add_fn);
    bun_set(ctx, global, "nativeGreet", 11, greet_fn);

    Counter* counter = malloc(sizeof(*counter));
    if (!counter) {
        fprintf(stderr, "Failed to allocate counter\n");
        bun_destroy(rt);
        return 1;
    }
    counter->value = 10;

    BunValue counter_obj = bun_object(ctx);
    BunValue inc_fn = bun_function(ctx, "inc", counter_inc, NULL, 0);
    bun_set_opaque(ctx, counter_obj, counter);
    bun_define_finalizer(ctx, counter_obj, counter_finalize, counter);
    bun_set(ctx, counter_obj, "inc", 3, inc_fn);
    bun_define_accessor(ctx, counter_obj, "value", 5, counter_get, counter_set, 0, 0, 0);
    bun_set(ctx, global, "counter", 7, counter_obj);

    BunClass* view_class = bun_class_register(ctx, &VIEW_CLASS, NULL);
    BunClass* text_class = bun_class_register(ctx, &TEXT_CLASS, view_class);
    if (!view_class || !text_class) {
        fprintf(stderr, "Failed to register BunClass descriptors\n");
        bun_destroy(rt);
        return 1;
    }

    NativeText* label = calloc(1, sizeof(*label));
    if (!label) {
        fprintf(stderr, "Failed to allocate label\n");
        bun_destroy(rt);
        return 1;
    }
    label->view.x = 12;
    label->view.y = 18;
    strcpy(label->text, "embed label");

    BunValue label_obj = bun_class_new(ctx, text_class, label, native_view_finalize, "Text");
    BunValue view_proto = bun_class_prototype(ctx, view_class);
    BunValue text_proto = bun_class_prototype(ctx, text_class);
    bun_set(ctx, global, "label", 5, label_obj);
    bun_set(ctx, global, "ViewProto", 9, view_proto);
    bun_set(ctx, global, "TextProto", 9, text_proto);

    printf("class instance? %d\n", bun_is_class_instance(ctx, label_obj));
    printf("instanceof View? %d\n", bun_instanceof_class(ctx, label_obj, view_class));
    printf("instanceof Text? %d\n", bun_instanceof_class(ctx, label_obj, text_class));

    NativeView* unwrapped_view = (NativeView*)bun_class_unwrap(ctx, label_obj, view_class);
    NativeText* unwrapped_text = (NativeText*)bun_class_unwrap(ctx, label_obj, text_class);
    printf("unwrap(view)=%p unwrap(text)=%p\n", (void*)unwrapped_view, (void*)unwrapped_text);

    printf("\n--- Evaluating JS ---\n");
    BunEvalResult r;

    r = bun_eval_string(rt, "console.log('Hello from embedded Bun!')");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    r = bun_eval_string(rt, "console.log('nativeAdd(3, 4) =', nativeAdd(3, 4))");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    r = bun_eval_string(rt, "console.log(nativeGreet('Bun'))");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    r = bun_eval_string(rt,
        "console.log('counter.value =', counter.value);"
        "counter.value = 42;"
        "console.log('counter.inc() =', counter.inc());"
        "console.log('counter.value =', counter.value);");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    r = bun_eval_string(rt,
        "console.log('label.text =', label.text);"
        "console.log('label.measure() =', label.measure());"
        "console.log('moveBy ->', label.moveBy(3, 4));"
        "console.log('label.x,label.y =', label.x, label.y);"
        "console.log('text proto === Object.getPrototypeOf(label):', TextProto === Object.getPrototypeOf(label));"
        "console.log('view proto === Object.getPrototypeOf(TextProto):', ViewProto === Object.getPrototypeOf(TextProto));");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    // Demonstrate bun_call with error detection.
    r = bun_eval_string(rt, "globalThis.throwingFn = () => { throw new Error('boom'); };");
    if (r.success) {
        BunValue throwing_fn = bun_get(ctx, global, "throwingFn", 10);
        BunValue result = bun_call(ctx, throwing_fn, BUN_UNDEFINED, 0, NULL);
        if (result == BUN_EXCEPTION) {
            const char* err = bun_last_error(ctx);
            printf("bun_call caught exception: %s\n", err ? err : "(no message)");
        }
    }

    // Schedule a timer to demonstrate event loop integration
    r = bun_eval_string(rt,
        "let count = 0;"
        "const timer = setInterval(() => {"
        "  count++;"
        "  console.log('Timer tick', count);"
        "  if (count >= 3) clearInterval(timer);"
        "}, 100);");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    // ------------------------------------------------------------------
    // Demonstrate bun_array_buffer and bun_typed_array (zero-copy)
    // ------------------------------------------------------------------
    printf("\n--- ArrayBuffer / TypedArray demo ---\n");

    // Heap-allocate a float buffer; ownership passes to the JS finalizer.
    const size_t num_floats = 4;
    float* floats = (float*)malloc(num_floats * sizeof(float));
    if (floats) {
        floats[0] = 1.5f;
        floats[1] = 2.5f;
        floats[2] = 3.0f;
        floats[3] = 4.0f;

        // Wrap as Float32Array — zero-copy, finalizer frees `floats` on GC.
        BunValue f32 = bun_typed_array(ctx, BUN_FLOAT32_ARRAY, floats,
            num_floats, float_buffer_finalize, floats);
        bun_set(ctx, global, "nativeFloats", 12, f32);

        r = bun_eval_string(rt,
            "const a = nativeFloats;"
            "console.log('Float32Array length:', a.length);"
            "let sum = 0; for (const x of a) sum += x;"
            "console.log('Float32Array sum:', sum);");
        if (!r.success) fprintf(stderr, "Error: %s\n", r.error);
    }

    // Wrap a static byte buffer as ArrayBuffer (no finalizer needed).
    static const uint8_t magic[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    BunValue ab = bun_array_buffer(ctx, (void*)(uintptr_t)magic, sizeof(magic), NULL, NULL);
    bun_set(ctx, global, "nativeBuf", 9, ab);

    r = bun_eval_string(rt,
        "const v = new DataView(nativeBuf);"
        "console.log('ArrayBuffer[0]:', v.getUint8(0).toString(16));"
        "console.log('ArrayBuffer length:', nativeBuf.byteLength);");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    printf("\n--- Running event loop ---\n");
    for (int i = 0; i < 100; i++) {
        int has_pending = bun_run_pending_jobs(rt);
        if (!has_pending) {
            printf("Event loop idle, stopping.\n");
            break;
        }
        usleep(50000); // 50ms - simulate frame rate
    }

    printf("\nDestroying Bun runtime...\n");
    printf("manual dispose(label) -> %d\n", bun_class_dispose(ctx, label_obj));
    printf("dispose(label) again -> %d\n", bun_class_dispose(ctx, label_obj));
    bun_destroy(rt);

    printf("Done.\n");
    return 0;
}

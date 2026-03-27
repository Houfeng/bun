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

    // Schedule a timer to demonstrate event loop integration
    r = bun_eval_string(rt,
        "let count = 0;"
        "const timer = setInterval(() => {"
        "  count++;"
        "  console.log('Timer tick', count);"
        "  if (count >= 3) clearInterval(timer);"
        "}, 100);");
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
    bun_destroy(rt);

    printf("Done.\n");
    return 0;
}

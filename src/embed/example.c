/// example.c — Demonstrates using the Bun embedding API.
///
/// This example shows how to:
///   1. Initialize a Bun runtime
///   2. Evaluate JavaScript code
///   3. Inject native C functions callable from JS
///   4. Drive the event loop manually (simulating a GUI main loop)
///   5. Clean up
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

// ---------------------------------------------------------------------------
// Example native function: adds two numbers
// ---------------------------------------------------------------------------

static char *native_add(int argc, const char **argv, void *userdata) {
    (void)userdata;
    if (argc < 2 || !argv[0] || !argv[1]) return NULL;

    double a = atof(argv[0]);  // argv[i] are JSON values, numbers are valid JSON
    double b = atof(argv[1]);

    // Return a JSON number string (caller takes ownership via malloc)
    char *result = malloc(64);
    if (!result) return NULL;
    snprintf(result, 64, "%g", a + b);
    return result;
}

// ---------------------------------------------------------------------------
// Example native function: returns a greeting string
// ---------------------------------------------------------------------------

static char *native_greet(int argc, const char **argv, void *userdata) {
    (void)userdata;

    const char *name = "World";
    // argv[0] is a JSON string like "\"Alice\"", so skip the quotes
    char namebuf[128] = {0};
    if (argc >= 1 && argv[0]) {
        size_t len = strlen(argv[0]);
        if (len >= 2 && argv[0][0] == '"' && argv[0][len - 1] == '"') {
            size_t copylen = len - 2;
            if (copylen >= sizeof(namebuf)) copylen = sizeof(namebuf) - 1;
            memcpy(namebuf, argv[0] + 1, copylen);
            namebuf[copylen] = '\0';
            name = namebuf;
        }
    }

    // Return a JSON string (must be a valid JSON value)
    char *result = malloc(256);
    if (!result) return NULL;
    snprintf(result, 256, "\"Hello, %s!\"", name);
    return result;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void) {
    printf("Initializing Bun runtime...\n");

    BunRuntime *rt = bun_initialize(NULL);
    if (!rt) {
        fprintf(stderr, "Failed to initialize Bun runtime\n");
        return 1;
    }

    // Inject native functions
    if (!bun_inject_native_function(rt, "nativeAdd", native_add, NULL)) {
        fprintf(stderr, "Failed to inject nativeAdd\n");
    }
    if (!bun_inject_native_function(rt, "nativeGreet", native_greet, NULL)) {
        fprintf(stderr, "Failed to inject nativeGreet\n");
    }

    // Evaluate some JavaScript
    printf("\n--- Evaluating JS ---\n");
    BunEvalResult r;

    r = bun_eval(rt, "console.log('Hello from embedded Bun!')");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    r = bun_eval(rt, "console.log('nativeAdd(3, 4) =', nativeAdd(3, 4))");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    r = bun_eval(rt, "console.log(nativeGreet('Bun'))");
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    // Schedule a timer to demonstrate event loop integration
    r = bun_eval(rt,
        "let count = 0;"
        "const timer = setInterval(() => {"
        "  count++;"
        "  console.log('Timer tick', count);"
        "  if (count >= 3) clearInterval(timer);"
        "}, 100);"
    );
    if (!r.success) fprintf(stderr, "Error: %s\n", r.error);

    // Simulate a GUI main loop: poll the event loop until idle
    printf("\n--- Running event loop ---\n");
    for (int i = 0; i < 100; i++) {
        int has_pending = bun_eval_pending_jobs(rt);
        if (!has_pending) {
            printf("Event loop idle, stopping.\n");
            break;
        }
        usleep(50000);  // 50ms - simulate frame rate
    }

    // Clean up
    printf("\nDestroying Bun runtime...\n");
    bun_destroy(rt);

    printf("Done.\n");
    return 0;
}

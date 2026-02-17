# nanocron

Small C23 cron scheduler library with nanosecond precision.

## Features

- Public API header at `include/nanocron/nanocron.h`.
- Implementation in `src/nanocron.c`.
- Full 7-field cron format with nanosecond field.
- Supports wildcard, exact value, ranges, lists, and step expressions.
- Implements vixie-cron day-of-month/day-of-week semantics:
  - If both DOM and DOW are restricted, they are OR-ed.
  - If either DOM or DOW is `*`, they are AND-ed.
- `cron_get_next_trigger` computes the next matching instant strictly after a given time.
- C23-first implementation with strict warning flags in project build scripts.

## Schedule Format

Every schedule must contain exactly 7 whitespace-separated fields:

`nanosecond second minute hour day_of_month month day_of_week`

| Field | Range |
| --- | --- |
| nanosecond | `0-999999999` |
| second | `0-59` |
| minute | `0-59` |
| hour | `0-23` |
| day_of_month | `1-31` |
| month | `1-12` |
| day_of_week | `0-6` (`0 = Sunday`) |

Supported syntax per field:

- `*` for any value
- `42` for exact value
- `10-20` for range
- `1,3,5` for list
- `*/15` for step from field minimum
- `10-50/5` for step inside a range

Examples:

- `0 * * * * * *` -> every second on the nanosecond boundary
- `*/100000000 * * * * * *` -> every 100ms
- `0 0 30 9 * * 1-5` -> weekdays at `09:30:00.000000000` UTC
- `0 0 0 0 1 * 5` -> midnight on day `1` of the month OR Friday

## Quick Start

```c
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <time.h>
#include "nanocron/nanocron.h"

static void on_fire([[maybe_unused]] void *user_data,
                    const struct timespec *ts) {
  printf("fired at %lld.%09ld\n", (long long)ts->tv_sec, ts->tv_nsec);
}

int main() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return 1;
  }

  if (cron_add(ctx, "0 */5 * * * * *", on_fire, nullptr) == nullptr) {
    cron_destroy(ctx);
    return 1;
  }

  while (true) {
    cron_tick(ctx);

    struct timespec sleep_for = {.tv_sec = 0, .tv_nsec = 1'000'000}; /* 1ms */
    nanosleep(&sleep_for, nullptr);
  }

  cron_destroy(ctx);
  return 0;
}
```

Compile manually:

```bash
clang -std=c23 -Wall -Wextra -Wpedantic -Werror -Iinclude examples/simple.c src/nanocron.c -o examples/simple
```

## API

- `cron_ctx_t *cron_create()`
  - Allocates and returns a scheduler context, or `nullptr` on failure.
- `void cron_destroy(cron_ctx_t *ctx)`
  - Frees all jobs and the context.
- `cron_job_t *cron_add(cron_ctx_t *ctx, const char *schedule, cron_callback_t cb, void *user_data)`
  - Parses and registers a job. Returns a job handle or `nullptr`.
- `bool cron_remove(cron_ctx_t *ctx, cron_job_t *job)`
  - Removes a previously-added job handle.
- `void cron_execute_due(cron_ctx_t *ctx, const struct timespec *now)`
  - Executes callbacks due at `now` (UTC).
- `void cron_tick(cron_ctx_t *ctx)`
  - Convenience wrapper using current UTC time via `timespec_get`.
- `bool cron_get_next_trigger(const cron_ctx_t *ctx, const struct timespec *after, struct timespec *next_out)`
  - Finds the next trigger strictly after `after` (search horizon: 366 days).

## Build And Test

Using `just`:

```bash
just all           # build examples + tests
just test          # build and run tests
just test-debug    # build with sanitizers and run tests
just examples-build
just examples-debug
just clean
```

Using Zig build:

```bash
zig build          # default: all
zig build examples
zig build tests
zig build test     # build and run tests
```

The project builds with:

- `-std=c23`
- `-Wall -Wextra -Wpedantic -Werror`
- hardening: `-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE`
- linker hardening: `-Wl,-z,relro,-z,now -pie`

## Notes

- Time handling is UTC-based (`gmtime_r`, `timespec_get`).
- Callback execution is synchronous in the calling thread.
- The scheduler context is not thread-safe; synchronize externally if shared.

## License

MIT. See `LICENSE`.

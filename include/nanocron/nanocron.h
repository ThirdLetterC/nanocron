#pragma once

/*
 * nanocron.h - C23 CRON library with full nanosecond precision
 *
 * Schedule format (exactly 7 whitespace-separated fields):
 *   nanosecond (0-999999999)  second (0-59)  minute (0-59)  hour (0-23)
 *   day-of-month (1-31)  month (1-12)  day-of-week (0-6, 0=Sunday)
 *
 * Standard vixie-cron DOM/DOW rule is implemented (when both fields are
 * restricted they are OR-ed, otherwise AND).
 */

#include <stdint.h>
#include <time.h>

typedef void (*cron_callback_t)(void *user_data,
                                const struct timespec *trigger_time);

typedef struct cron_job cron_job_t;
typedef struct cron_ctx cron_ctx_t;

[[nodiscard]]
cron_ctx_t *cron_create();

void cron_destroy(cron_ctx_t *ctx);

/* Returns opaque job handle for later removal, nullptr on parse error. */
[[nodiscard]]
cron_job_t *cron_add(cron_ctx_t *ctx, const char *schedule, cron_callback_t cb,
                     void *user_data);

[[nodiscard]]
bool cron_remove(cron_ctx_t *ctx, cron_job_t *job);

/* Call with current time (UTC). Fires every matching job exactly once per
 * instant. */
void cron_execute_due(cron_ctx_t *ctx, const struct timespec *now);

/* Convenience: use current UTC time via standard timespec_get */
void cron_tick(cron_ctx_t *ctx);

/* Compute next trigger strictly after `after` (search horizon: 366 days). */
[[nodiscard]]
bool cron_get_next_trigger(const cron_ctx_t *ctx, const struct timespec *after,
                           struct timespec *next_out);

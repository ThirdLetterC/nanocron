#pragma once

/*
 * nanocron.h - Single-header C23 CRON library with full nanosecond precision
 *
 * Schedule format (exactly 7 whitespace-separated fields):
 *   nanosecond (0-999999999)  second (0-59)  minute (0-59)  hour (0-23)
 *   day-of-month (1-31)  month (1-12)  day-of-week (0-6, 0=Sunday)
 *
 * Per-field syntax (all fields support this):
 *   *          → any value
 *   42         → exact
 *   10-20      → range
 *   1,3,5      → list
 *   *\/15      → every 15 (from field minimum)
 *   10-50/5    → every 5 inside range
 *
 * Examples:
 *   "0 * * * * * *"                    → every second on the dot
 *   "*\/100000000 * * * * * *"         → 10× per second
 *   "0 30 9 * * 1-5 *"                 → weekdays 09:30:00.000000000 UTC
 *   "0 0 0 1 * *"                      → 1st of every month at midnight
 *
 * Standard vixie-cron DOM/DOW rule is implemented (when both fields are
 * restricted they are OR-ed, otherwise AND).
 *
 * C23 only, header-only, no external dependencies beyond <time.h>.
 * All allocations are explicit; user owns the context.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*cron_callback_t)(void *user_data,
                                const struct timespec *trigger_time);

typedef struct cron_job cron_job_t;
typedef struct cron_ctx cron_ctx_t;

static constexpr size_t CRON_FIELD_COUNT = 7;
static constexpr size_t CRON_MAX_ATOMS = 12;
static constexpr size_t CRON_DOM_FIELD = 4;
static constexpr size_t CRON_DOW_FIELD = 6;
static constexpr size_t CRON_SECONDS_PER_DAY = 86'400;
static constexpr size_t CRON_LOOKAHEAD_DAYS = 366;
static constexpr size_t CRON_LOOKAHEAD_SECONDS =
    CRON_LOOKAHEAD_DAYS * CRON_SECONDS_PER_DAY;

typedef struct {
  uint64_t start;
  uint64_t end;
  uint32_t step;
} cron_atom;

typedef struct {
  cron_atom atoms[CRON_MAX_ATOMS];
  size_t num_atoms;
  bool is_wildcard; /* true only if the field was exactly "*" */
} cron_field;

static inline int timespec_cmp(const struct timespec *a,
                               const struct timespec *b);

struct cron_job {
  cron_job_t *next;
  cron_field fields[CRON_FIELD_COUNT];
  cron_callback_t callback;
  void *user_data;
  struct timespec last_fired; /* de-duplication */
  bool is_removed;            /* deferred removal marker */
};

struct cron_ctx {
  cron_job_t *jobs;
  size_t execution_depth; /* >0 while inside cron_execute_due */
};

static constexpr uint64_t CRON_FIELD_MIN[CRON_FIELD_COUNT] = {0, 0, 0, 0,
                                                              1, 1, 0};
static constexpr uint64_t CRON_FIELD_MAX[CRON_FIELD_COUNT] = {
    999'999'999ULL, 59, 59, 23, 31, 12, 6};

[[nodiscard]]
static bool parse_u64(const char **cursor, uint64_t minv, uint64_t maxv,
                      uint64_t *out) {
  if (!cursor || !(*cursor) || !out) {
    return false;
  }

  const char *p = *cursor;
  if (!isdigit((unsigned char)*p)) {
    return false;
  }

  uint64_t value = 0;
  while (isdigit((unsigned char)*p)) {
    const uint64_t digit = (uint64_t)(*p - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    p++;
  }

  if (value < minv || value > maxv) {
    return false;
  }

  *out = value;
  *cursor = p;
  return true;
}

/* Hard part: parse one field. Supports lists, ranges, steps. */
static bool parse_cron_field(const char *str, uint64_t minv, uint64_t maxv,
                             cron_field *f) {
  f->num_atoms = 0;
  f->is_wildcard = false;

  if (!str || !*str)
    return false;

  if (strcmp(str, "*") == 0) {
    f->is_wildcard = true;
    f->atoms[0].start = minv;
    f->atoms[0].end = maxv;
    f->atoms[0].step = 1;
    f->num_atoms = 1;
    return true;
  }

  char *copy = strdup(str);
  if (!copy)
    return false;

  char *part = copy;
  while (part && *part) {
    if (f->num_atoms >= CRON_MAX_ATOMS) {
      free(copy);
      return false;
    }

    char *next = part;
    while (*next && *next != ',') {
      next++;
    }

    if (*next == ',') {
      *next = '\0';
      next++;
      if (*next == '\0') {
        goto fail;
      }
    } else {
      next = nullptr;
    }

    if (*part == '\0') {
      goto fail;
    }

    const char *p = part;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t step_u64 = 1;
    bool had_range = false;

    /* start value */
    if (*p == '*') {
      start = minv;
      end = maxv;
      had_range = true;
      p++;
    } else {
      if (!parse_u64(&p, minv, maxv, &start))
        goto fail;
      end = start;
    }

    /* range */
    if (*p == '-') {
      had_range = true;
      p++;
      if (!parse_u64(&p, minv, maxv, &end) || end < start)
        goto fail;
    }

    /* step */
    if (*p == '/') {
      p++;
      if (!parse_u64(&p, 1, UINT64_MAX, &step_u64))
        goto fail;
    }

    if (*p != '\0')
      goto fail;

    /* "10/5" → start=10, end=max (standard cron semantics) */
    if (step_u64 > 1 && !had_range) {
      end = maxv;
    }
    if (step_u64 > UINT32_MAX)
      goto fail;

    f->atoms[f->num_atoms].start = start;
    f->atoms[f->num_atoms].end = end;
    f->atoms[f->num_atoms].step = (uint32_t)step_u64;
    f->num_atoms++;

    part = next;
  }

  free(copy);
  return f->num_atoms > 0;

fail:
  free(copy);
  return false;
}

static bool parse_cron_expression(const char *expr,
                                  cron_field fields[CRON_FIELD_COUNT]) {
  if (!expr)
    return false;

  char *copy = strdup(expr);
  if (!copy)
    return false;

  size_t idx = 0;
  char *cursor = copy;

  while (*cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (!*cursor) {
      break;
    }

    char *token = cursor;
    while (*cursor && !isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor) {
      *cursor = '\0';
      cursor++;
    }

    if (idx >= CRON_FIELD_COUNT) {
      free(copy);
      return false;
    }
    if (!parse_cron_field(token, CRON_FIELD_MIN[idx], CRON_FIELD_MAX[idx],
                          &fields[idx])) {
      free(copy);
      return false;
    }
    idx++;
  }

  free(copy);
  return idx == CRON_FIELD_COUNT; /* exactly 7 fields required */
}

static bool field_matches(const cron_field *f, uint64_t val) {
  for (size_t i = 0; i < f->num_atoms; i++) {
    const cron_atom *a = &f->atoms[i];
    if (val >= a->start && val <= a->end) {
      if (a->step == 1 || (val - a->start) % a->step == 0)
        return true;
    }
  }
  return false;
}

static bool day_fields_match(const cron_field *dom_field,
                             const cron_field *dow_field, uint64_t dom_value,
                             uint64_t dow_value) {
  const bool dom_match = field_matches(dom_field, dom_value);
  const bool dow_match = field_matches(dow_field, dow_value);

  if (dom_field->is_wildcard || dow_field->is_wildcard) {
    return dom_match && dow_match;
  }
  return dom_match || dow_match;
}

static bool non_day_fields_match(const cron_field fields[CRON_FIELD_COUNT],
                                 const uint64_t values[CRON_FIELD_COUNT],
                                 bool include_nanoseconds) {
  for (size_t i = 0; i < CRON_FIELD_COUNT; i++) {
    if (i == CRON_DOM_FIELD || i == CRON_DOW_FIELD) {
      continue;
    }
    if (!include_nanoseconds && i == 0) {
      continue;
    }
    if (!field_matches(&fields[i], values[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]]
static bool field_next_match(const cron_field *f, uint64_t min_candidate,
                             uint64_t maxv, uint64_t *out) {
  if (!f || !out || min_candidate > maxv) {
    return false;
  }

  bool found = false;
  uint64_t best = 0;

  for (size_t i = 0; i < f->num_atoms; i++) {
    const cron_atom *atom = &f->atoms[i];
    if (atom->start > maxv) {
      continue;
    }

    const uint64_t atom_end = (atom->end < maxv) ? atom->end : maxv;
    if (min_candidate > atom_end) {
      continue;
    }

    uint64_t candidate = atom->start;
    if (candidate < min_candidate) {
      const uint64_t step = (uint64_t)atom->step;
      const uint64_t delta = min_candidate - atom->start;
      const uint64_t rem = delta % step;
      candidate = (rem == 0) ? min_candidate : (min_candidate + (step - rem));
    }

    if (candidate > atom_end) {
      continue;
    }

    if (!found || candidate < best) {
      best = candidate;
      found = true;
    }
  }

  if (!found) {
    return false;
  }
  *out = best;
  return true;
}

[[nodiscard]]
static cron_job_t **find_job_link(cron_ctx_t *ctx, const cron_job_t *job) {
  if (!ctx || !job) {
    return nullptr;
  }

  cron_job_t **link = &ctx->jobs;
  while (*link && *link != job) {
    link = &(*link)->next;
  }
  return (*link == job) ? link : nullptr;
}

static void sweep_removed_jobs(cron_ctx_t *ctx) {
  if (!ctx) {
    return;
  }

  cron_job_t **link = &ctx->jobs;
  while (*link) {
    cron_job_t *job = *link;
    if (job->is_removed) {
      *link = job->next;
      free(job);
      continue;
    }
    link = &job->next;
  }
}

/* === Public API === */

[[nodiscard]]
cron_ctx_t *cron_create() {
  return calloc(1, sizeof(cron_ctx_t));
}

void cron_destroy(cron_ctx_t *ctx) {
  if (!ctx)
    return;
  cron_job_t *j = ctx->jobs;
  while (j) {
    cron_job_t *nxt = j->next;
    free(j);
    j = nxt;
  }
  free(ctx);
}

/* Returns opaque job handle for later removal, nullptr on parse error. */
[[nodiscard]]
cron_job_t *cron_add(cron_ctx_t *ctx, const char *schedule, cron_callback_t cb,
                     void *user_data) {
  if (!ctx || !schedule || !cb)
    return nullptr;

  cron_field fields[CRON_FIELD_COUNT] = {0};
  if (!parse_cron_expression(schedule, fields))
    return nullptr;

  cron_job_t *job = calloc(1, sizeof(cron_job_t));
  if (!job)
    return nullptr;

  memcpy(job->fields, fields, sizeof(fields));
  job->callback = cb;
  job->user_data = user_data;
  job->last_fired.tv_sec = (time_t)-1; /* sentinel */

  job->next = ctx->jobs;
  ctx->jobs = job;
  return job;
}

[[nodiscard]]
bool cron_remove(cron_ctx_t *ctx, cron_job_t *job) {
  cron_job_t **link = find_job_link(ctx, job);
  if (!link) {
    return false;
  }

  if (ctx->execution_depth > 0) {
    (*link)->is_removed = true;
    return true;
  }

  cron_job_t *victim = *link;
  *link = victim->next;
  free(victim);
  return true;
}

/* Call with current time (UTC). Fires every matching job exactly once per
 * instant. */
void cron_execute_due(cron_ctx_t *ctx, const struct timespec *now) {
  if (!ctx || !now) {
    return;
  }
  if (now->tv_nsec < 0 || now->tv_nsec > 999'999'999L) {
    return;
  }

  struct tm tm;
  if (!gmtime_r(&now->tv_sec, &tm)) {
    return;
  }

  const uint64_t values[CRON_FIELD_COUNT] = {
      (uint64_t)now->tv_nsec, (uint64_t)tm.tm_sec,  (uint64_t)tm.tm_min,
      (uint64_t)tm.tm_hour,   (uint64_t)tm.tm_mday, (uint64_t)tm.tm_mon + 1,
      (uint64_t)tm.tm_wday};

  ctx->execution_depth++;
  cron_job_t *job = ctx->jobs;
  while (job) {
    cron_job_t *next = job->next;

    if (job->is_removed) {
      job = next;
      continue;
    }

    if (!non_day_fields_match(job->fields, values, true)) {
      job = next;
      continue;
    }

    const bool day_ok = day_fields_match(
        &job->fields[CRON_DOM_FIELD], &job->fields[CRON_DOW_FIELD],
        values[CRON_DOM_FIELD], values[CRON_DOW_FIELD]);

    if (day_ok) {
      /* Fire only once per distinct nanosecond instant */
      if (now->tv_sec > job->last_fired.tv_sec ||
          (now->tv_sec == job->last_fired.tv_sec &&
           now->tv_nsec > job->last_fired.tv_nsec)) {

        job->last_fired = *now; /* set before callback for reentrant safety */
        job->callback(job->user_data, now);
      }
    }
    job = next;
  }

  ctx->execution_depth--;
  if (ctx->execution_depth == 0) {
    sweep_removed_jobs(ctx);
  }
}

/* Convenience: use current UTC time via C11 timespec_get */
void cron_tick(cron_ctx_t *ctx) {
  struct timespec now;
  if (timespec_get(&now, TIME_UTC) == 0)
    return;
  cron_execute_due(ctx, &now);
}

/* Compute next trigger strictly after `after` (search horizon: 366 days). */
[[nodiscard]]
bool cron_get_next_trigger(const cron_ctx_t *ctx, const struct timespec *after,
                           struct timespec *next_out) {
  if (!ctx || !after || !next_out)
    return false;
  if (after->tv_nsec < 0 || after->tv_nsec > 999'999'999L)
    return false;

  for (time_t sec_off = 0; sec_off < (time_t)CRON_LOOKAHEAD_SECONDS;
       sec_off++) {
    const time_t sec = after->tv_sec + sec_off;

    struct tm tm;
    if (!gmtime_r(&sec, &tm))
      break;

    const uint64_t values[CRON_FIELD_COUNT] = {
        (uint64_t)0,          (uint64_t)tm.tm_sec,  (uint64_t)tm.tm_min,
        (uint64_t)tm.tm_hour, (uint64_t)tm.tm_mday, (uint64_t)tm.tm_mon + 1,
        (uint64_t)tm.tm_wday};

    bool found_in_second = false;
    uint64_t best_ns = 0;

    const cron_job_t *job = ctx->jobs;
    while (job) {
      if (job->is_removed) {
        job = job->next;
        continue;
      }

      if (!non_day_fields_match(job->fields, values, false)) {
        job = job->next;
        continue;
      }
      if (!day_fields_match(&job->fields[CRON_DOM_FIELD],
                            &job->fields[CRON_DOW_FIELD],
                            values[CRON_DOM_FIELD], values[CRON_DOW_FIELD])) {
        job = job->next;
        continue;
      }

      uint64_t min_ns = 0;
      if (sec_off == 0) {
        if (after->tv_nsec >= 999'999'999L) {
          job = job->next;
          continue;
        }
        min_ns = (uint64_t)after->tv_nsec + 1;
      }

      uint64_t matched_ns = 0;
      if (field_next_match(&job->fields[0], min_ns, CRON_FIELD_MAX[0],
                           &matched_ns)) {
        if (!found_in_second || matched_ns < best_ns) {
          best_ns = matched_ns;
          found_in_second = true;
        }
      }

      job = job->next;
    }

    if (found_in_second) {
      next_out->tv_sec = sec;
      next_out->tv_nsec = (long)best_ns;
      return true;
    }
  }

  return false;
}

static inline int timespec_cmp(const struct timespec *a,
                               const struct timespec *b) {
  if (a->tv_sec != b->tv_sec)
    return (a->tv_sec > b->tv_sec) ? 1 : -1;
  if (a->tv_nsec != b->tv_nsec)
    return (a->tv_nsec > b->tv_nsec) ? 1 : -1;
  return 0;
}

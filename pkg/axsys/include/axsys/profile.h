#ifndef AX_PROFILE_H
#define AX_PROFILE_H

#if defined(TRACY_ENABLE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

typedef TracyCZoneCtx ax_profile_zone_ctx;

static inline double ax_now_s(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    abort();
  return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static inline void ax_profile_zone_end(ax_profile_zone_ctx* ctx) {
  TracyCZoneEnd(*ctx);
}

static void ax_wait_for_tracy(double tim_df) {
  double deadline_s = ax_now_s() + tim_df;
  const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = 10000000L};
  while (!TracyCIsConnected && ax_now_s() < deadline_s) {
    nanosleep(&sleep_ts, NULL);
  }
}

#define AX_PROFILE_JOIN2(a, b) a##b
#define AX_PROFILE_JOIN(a, b)  AX_PROFILE_JOIN2(a, b)
#define AX_PROFILE_ZONE_IMPL(name, line)                                       \
  TracyCZoneN(AX_PROFILE_JOIN(ax_tracy_zone_, line), name, 1);                 \
  __attribute__((cleanup(ax_profile_zone_end))) ax_profile_zone_ctx            \
  AX_PROFILE_JOIN(ax_tracy_zone_cleanup_, line) =                              \
      AX_PROFILE_JOIN(ax_tracy_zone_, line)

#define AX_PROFILE_ZONE(name)            AX_PROFILE_ZONE_IMPL(name, __LINE__)
#define AX_PROFILE_ZONE_BEGIN(ctx, name) TracyCZoneN(ctx, name, 1)
#define AX_PROFILE_ZONE_BEGIN_ASSIGN(ctx, name)                                \
  do {                                                                         \
    static const struct ___tracy_source_location_data AX_PROFILE_JOIN(         \
        ax_tracy_source_, __LINE__) = {name, __func__, TracyFile,              \
                                       (uint32_t)TracyLine, 0};                \
    (ctx) = ___tracy_emit_zone_begin_callstack(                                \
        &AX_PROFILE_JOIN(ax_tracy_source_, __LINE__), TRACY_CALLSTACK, 1);     \
  } while (0)
#define AX_PROFILE_ZONE_BEGIN_ALLOC_NAME(ctx, name, size)                      \
  do {                                                                         \
    uint64_t AX_PROFILE_JOIN(ax_tracy_srcloc_, __LINE__) =                     \
        ___tracy_alloc_srcloc_name((uint32_t)TracyLine, TracyFile,             \
                                   strlen(TracyFile), __func__,                \
                                   strlen(__func__), name, size, 0);           \
    (ctx) = ___tracy_emit_zone_begin_alloc_callstack(                          \
        AX_PROFILE_JOIN(ax_tracy_srcloc_, __LINE__), TRACY_CALLSTACK, 1);      \
  } while (0)
#define AX_PROFILE_ZONE_END(ctx)              TracyCZoneEnd(ctx)
#define AX_PROFILE_ZONE_NAME(ctx, name, size) TracyCZoneName(ctx, name, size)
#define AX_PROFILE_THREAD(name)               TracyCSetThreadName(name)
#define AX_PROFILE_FRAME(name)                TracyCFrameMarkNamed(name)
#define AX_PROFILE_PLOT_I(name, value)        TracyCPlotI(name, value)

#else

typedef const void* ax_profile_zone_ctx;

#define AX_PROFILE_ZONE(name)                             ((void)0)
#define AX_PROFILE_ZONE_BEGIN(ctx, name)                  ((void)0)
#define AX_PROFILE_ZONE_BEGIN_ASSIGN(ctx, name)           ((void)0)
#define AX_PROFILE_ZONE_BEGIN_ALLOC_NAME(ctx, name, size) ((void)0)
#define AX_PROFILE_ZONE_END(ctx)                          ((void)0)
#define AX_PROFILE_ZONE_NAME(ctx, name, size)             ((void)0)
#define AX_PROFILE_THREAD(name)                           ((void)0)
#define AX_PROFILE_FRAME(name)                            ((void)0)
#define AX_PROFILE_PLOT_I(name, value)                    ((void)0)

static void ax_wait_for_tracy(double tim_df) {
  (void)tim_df;
}

#endif
#endif

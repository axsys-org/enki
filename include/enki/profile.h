#pragma once

#if defined(TRACY_ENABLE)
#include <tracy/TracyC.h>

typedef TracyCZoneCtx enki_profile_zone_ctx;

static inline void enki_profile_zone_end(enki_profile_zone_ctx* ctx)
{
    TracyCZoneEnd(*ctx);
}

#define ENKI_PROFILE_JOIN2(a, b) a##b
#define ENKI_PROFILE_JOIN(a, b) ENKI_PROFILE_JOIN2(a, b)
#define ENKI_PROFILE_ZONE_IMPL(name, line)                                      \
    TracyCZoneN(ENKI_PROFILE_JOIN(enki_tracy_zone_, line), name, 1);            \
    __attribute__((cleanup(enki_profile_zone_end))) enki_profile_zone_ctx       \
        ENKI_PROFILE_JOIN(enki_tracy_zone_cleanup_, line) =                    \
            ENKI_PROFILE_JOIN(enki_tracy_zone_, line)

#define ENKI_PROFILE_ZONE(name) ENKI_PROFILE_ZONE_IMPL(name, __LINE__)
#define ENKI_PROFILE_ZONE_BEGIN(ctx, name) TracyCZoneN(ctx, name, 1)
#define ENKI_PROFILE_ZONE_END(ctx) TracyCZoneEnd(ctx)
#define ENKI_PROFILE_THREAD(name) TracyCSetThreadName(name)
#define ENKI_PROFILE_FRAME(name) TracyCFrameMarkNamed(name)
#define ENKI_PROFILE_PLOT_I(name, value) TracyCPlotI(name, value)

#else

#define ENKI_PROFILE_ZONE(name) ((void)0)
#define ENKI_PROFILE_ZONE_BEGIN(ctx, name) ((void)0)
#define ENKI_PROFILE_ZONE_END(ctx) ((void)0)
#define ENKI_PROFILE_THREAD(name) ((void)0)
#define ENKI_PROFILE_FRAME(name) ((void)0)
#define ENKI_PROFILE_PLOT_I(name, value) ((void)0)

#endif

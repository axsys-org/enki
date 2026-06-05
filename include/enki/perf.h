#pragma once

#define ep_likely(x)       __builtin_expect(!!(x), 1)
#define ep_unlikely(x)     __builtin_expect(!!(x), 0)

#ifdef DEBUG
#define ep_debug_if(cond, ret) if ( cond ) ret
#else
#define ep_debug_if(cond, ret) 0;
#endif



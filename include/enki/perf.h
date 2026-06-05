#pragma once

#define ep_likely(x)       __builtin_expect(!!(x), 1)
#define ep_unlikely(x)     __builtin_expect(!!(x), 0)

#ifdef DEBUG
#define ep_debug_if(cond, ret) do { if (cond) ret } while (0)
#else
#define ep_debug_if(cond, ret) do { (void)sizeof(cond); } while (0)
#endif


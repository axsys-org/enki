#ifndef ENKI_OS_TIME_H
#define ENKI_OS_TIME_H

#include <stdint.h>

typedef long time_t;
struct timespec { time_t tv_sec; long tv_nsec; };

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clock_id, struct timespec* ts);
int nanosleep(const struct timespec* req, struct timespec* rem);

#endif

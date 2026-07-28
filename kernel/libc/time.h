/* freestanding time.h for the LarzOS kernel (returns 0 - no clock yet) */
#ifndef _LARZOS_TIME_H
#define _LARZOS_TIME_H
typedef long time_t;
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  0
time_t time(time_t *t);
int clock_gettime(int clk_id, struct timespec *tp);
#endif

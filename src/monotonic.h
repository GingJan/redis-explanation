#ifndef __MONOTONIC_H
#define __MONOTONIC_H
/* The monotonic clock is an always increasing clock source.  It is unrelated to
 * the actual time of day and should only be used for relative timings.  The
 * monotonic clock is also not guaranteed to be chronologically precise; there
 * may be slight skew/shift from a precise clock.
 *
 * Depending on system architecture, the monotonic time may be able to be
 * retrieved much faster than a normal clock source by using an instruction
 * counter on the CPU.  On x86 architectures (for example), the RDTSC
 * instruction is a very fast clock source for this purpose.
 */

#include "fmacros.h"
#include <stdint.h>
#include <unistd.h>

/* A counter in micro-seconds.  The 'monotime' type is provided for variables
 * holding a monotonic time.  This will help distinguish & document that the
 * variable is associated with the monotonic clock and should not be confused
 * with other types of time.*/
typedef uint64_t monotime;

/* Retrieve counter of micro-seconds relative to an arbitrary point in time.  */
extern monotime (*getMonotonicUs)(void);

typedef enum monotonic_clock_type {
    MONOTONIC_CLOCK_POSIX,
    MONOTONIC_CLOCK_HW,
} monotonic_clock_type;

/* 在启动时调用一次以初始化monotonic时钟，尽管它只被调用一次，但多次调用也不会造成影响。
 * 返回一个可打印的字符串，该字符串表式时钟的类型
 * (返回的字符串是static的，无需释放内存空间。)  */
const char *monotonicInit(void);

/* 返回一个字符串，该字符串表式正被使用的monotonic时钟的类型。 */
const char *monotonicInfoString(void);

/* 返回正被使用的monotonic时钟的类型。 */
monotonic_clock_type monotonicGetType(void);

/* 测量经过的时间，例如:
 *     monotime myTimer;
 *     elapsedStart(&myTimer);
 *     while (elapsedMs(myTimer) < 10) {} // 循环10ms
 */
static inline void elapsedStart(monotime *start_time) {
    *start_time = getMonotonicUs();
}

static inline uint64_t elapsedUs(monotime start_time) {
    return getMonotonicUs() - start_time;
}

static inline uint64_t elapsedMs(monotime start_time) {
    return elapsedUs(start_time) / 1000;
}

#endif

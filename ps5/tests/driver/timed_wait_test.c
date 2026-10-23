/* Crux2 - the PS5 driver's timed waits (ps5/patches/mesa/0004), on the PC.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0+
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 * glibc, like FreeBSD, gives cnd_init a realtime clock: a monotonic deadline (the old way) comes
 * back at once, and a loop around it spins; util/ps5_timed_wait.h finds the clock and waits. */
#include "util/ps5_timed_wait.h"
#include <stdio.h>
static cnd_t c; static mtx_t m; static int failed;
static int waker(void *a) { (void)a; struct timespec t = {0, 3000000}; thrd_sleep(&t, NULL); mtx_lock(&m); cnd_broadcast(&c); mtx_unlock(&m); return 0; }
int main(void) {
   cnd_init(&c); mtx_init(&m, mtx_plain);
   /* the old way: a monotonic deadline 20 ms ahead */
   mtx_lock(&m); uint64_t s = os_time_get_nano(); struct timespec d = ps5_timed_wait_deadline(s, 20000000);
   int spins = 0; while (os_time_get_nano() - s < 20000000) { cnd_timedwait(&c, &m, &d); spins++; } mtx_unlock(&m);
   printf("old: 20 ms of waiting took %d waits\n", spins); if (spins < 10) failed++;
   mtx_lock(&m); s = os_time_get_nano(); ps5_cnd_wait_ns(&c, &m, 20000000); uint64_t took = os_time_get_nano() - s; mtx_unlock(&m);
   printf("new: one wait of 20 ms took %.3f ms\n", took / 1e6); if (took < 19000000 || took > 40000000) failed++;
   thrd_t t; thrd_create(&t, waker, NULL);
   mtx_lock(&m); s = os_time_get_nano(); ps5_cnd_wait_ns(&c, &m, 200000000); took = os_time_get_nano() - s; mtx_unlock(&m); thrd_join(t, NULL);
   printf("new: a broadcast 3 ms in ended a 200 ms wait after %.3f ms\n", took / 1e6); if (took > 50000000) failed++;
   if (ps5_timed_wait_clock() != PS5_WAIT_CLOCK_REALTIME) failed++;
   printf("timed wait: %s\n", failed ? "FAILED" : "passed"); return failed;
}

/* F-23: a slow peer must not hold a handshake thread past the timeout.
 *
 * The PQ link handshake receives a multi-cell value (ML-KEM public key, 1184
 * bytes over three cells). Both loops polled with a fresh MOOR_HANDSHAKE_TIMEOUT
 * every time a partial read left the cell incomplete, so a peer that sent one
 * byte just inside the timeout reset it forever and the thread never returned.
 *
 * The two cases below drive the real helper the fix introduced. They differ only
 * in where the deadline is computed -- which is exactly what the defect was:
 *
 *   "per wait"  recomputes the budget on every wait   <- the pre-fix logic
 *   "absolute"  computes it once before the loop      <- the fix
 *
 * A writer dribbles a byte every DRIBBLE_MS for DRIBBLE_TOTAL_MS without ever
 * completing anything, while the reader consumes one byte per wakeup, as a
 * partial cell read does. The per-wait loop is expected to run for as long as
 * the dribbling continues; the absolute loop is expected to stop at its budget.
 */
#include "moor/moor.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

void moor_hs_event_invalidate_circuit(moor_circuit_t *circ) { (void)circ; }
void moor_hs_event_nullify_conn(moor_connection_t *conn)    { (void)conn; }
void moor_handle_sighup(void)     { }
void moor_graceful_shutdown(void) { }
moor_hs_config_t *g_hs_configs = NULL;
int g_num_hs_configs = 0;
int g_use_bridges = 0;
void moor_request_consensus_refresh(void) { }

#define BUDGET_MS         200
#define DRIBBLE_MS         40
#define DRIBBLE_TOTAL_MS 1200

static int checks = 0, failures = 0;
#define OK(cond, fmt, ...) do { checks++; if (cond) printf("  ok    " fmt "\n", __VA_ARGS__); \
    else { failures++; printf("  FAIL  " fmt "\n", __VA_ARGS__); } } while (0)

static void *dribbler(void *arg) {
    int fd = *(int *)arg;
    for (int sent = 0; sent * DRIBBLE_MS < DRIBBLE_TOTAL_MS; sent++) {
        if (write(fd, "x", 1) != 1) break;
        usleep(DRIBBLE_MS * 1000);
    }
    return NULL;
}

/* Consume one byte per wakeup, the way a partial cell read does, and wait for
 * more. `absolute` selects where the deadline comes from. */
static uint64_t run_loop(int read_fd, int absolute) {
    uint64_t start = moor_time_ms();
    uint64_t deadline = start + BUDGET_MS;
    for (;;) {
        char b;
        ssize_t n = recv(read_fd, &b, 1, MSG_DONTWAIT);
        (void)n;                                  /* incomplete: keep waiting */
        int pr = moor_conn_wait_readable_until(
                     read_fd, absolute ? deadline : moor_time_ms() + BUDGET_MS);
        if (pr <= 0) break;
    }
    return moor_time_ms() - start;
}

static uint64_t timed(int absolute) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    pthread_t t;
    pthread_create(&t, NULL, dribbler, &sv[1]);
    uint64_t el = run_loop(sv[0], absolute);
    pthread_join(t, NULL);
    close(sv[0]); close(sv[1]);
    return el;
}

int main(void) {
    moor_log_set_level(MOOR_LOG_FATAL);
    if (sodium_init() < 0) return 1;

    printf("== F-23: a dribbling peer against a %d ms budget ==\n", BUDGET_MS);

    uint64_t per_wait = timed(0);
    OK(per_wait > (uint64_t)BUDGET_MS * 3,
       "deadline recomputed per wait (pre-fix): held %llu ms, far past %d ms",
       (unsigned long long)per_wait, BUDGET_MS);

    uint64_t absolute = timed(1);
    OK(absolute < (uint64_t)BUDGET_MS * 2,
       "one absolute deadline (fixed): returned after %llu ms",
       (unsigned long long)absolute);

    OK(absolute * 2 < per_wait,
       "the fix bounds it: %llu ms vs %llu ms",
       (unsigned long long)absolute, (unsigned long long)per_wait);

    printf("\n== the deadline itself ==\n");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        OK(moor_conn_wait_readable_until(sv[0], moor_time_ms() - 1) == 0,
           "%s", "a deadline already passed returns 0 without polling");
        if (write(sv[1], "x", 1) == 1)
            OK(moor_conn_wait_readable_until(sv[0], moor_time_ms() + 1000) > 0,
               "%s", "readable data returns > 0 before the deadline");
        uint64_t t0 = moor_time_ms();
        char b; while (recv(sv[0], &b, 1, MSG_DONTWAIT) == 1) { }
        int pr = moor_conn_wait_readable_until(sv[0], moor_time_ms() + 120);
        uint64_t waited = moor_time_ms() - t0;
        OK(pr == 0 && waited >= 100 && waited < 400,
           "a silent peer returns 0 at the deadline, after %llu ms",
           (unsigned long long)waited);
        close(sv[0]); close(sv[1]);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

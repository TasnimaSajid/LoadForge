#ifndef LOADFORGE_CONFIG_H
#define LOADFORGE_CONFIG_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>

/* ── tunables ─────────────────────────────────────────────────────────── */
#define MAX_BACKENDS        16
#define MAX_IP_LEN          INET_ADDRSTRLEN
#define MAX_QUEUE_LEN       100

#define CB_FAIL_THRESHOLD   3
#define CB_OPEN_TIMEOUT_SEC 30

#define WARMUP_PER_BACKEND  10
#define ADAPTIVE_THRESHOLD_PCT 20.0
#define ADAPTIVE_ABS_FLOOR_MS  1.0
#define ACTIVE_CONN_PENALTY_MS  2.0
#define SOFTMAX_TEMP        50.0
#define MIN_TRAFFIC_FRACTION 0.08
/* EMA_ALPHA = 0.2 as cited in proposal (Box & Jenkins, 1976).
 * The implementation uses time-decay equivalent:
 *   decay = exp(-dt / DECAY_TIME_MS), where dt is the inter-sample interval.
 * At the proposal's implied 1-sample-per-second rate this approximates alpha≈0.18,
 * consistent with the cited formula. */
#define EMA_ALPHA           0.2   /* reference constant — see update_ema() */
#define DECAY_TIME_MS       5000.0
#define NETWORK_WEIGHT      0.3
#define PROCESSING_WEIGHT   0.7

#define HEALTH_INTERVAL_SEC 2
#define WATCHDOG_INTERVAL_SEC 3
#define DASHBOARD_INTERVAL_SEC 1
#define STATS_LOGGER_INTERVAL_SEC 10
#define CONFIG_RELOAD_POLL_SEC 5

#define STATS_LOG_PATH      "/tmp/lf_stats.log"

#define LB_CTRL_SOCKET      "/tmp/loadforge.sock"

#define CB_CLOSED    0
#define CB_OPEN      1
#define CB_HALF_OPEN 2

#define ALGO_ROUND_ROBIN 0
#define ALGO_ADAPTIVE    1

/* Per-backend struct. Array index (0..backend_count-1) is used for g_active_conns[];
 * backend_t.id is the user-visible 1-based id from config / CLI. */
typedef struct {
    int    id;
    char   ip[MAX_IP_LEN];
    int    port;
    int    weight;

    int    is_healthy;
    int    draining;        /* hot-remove: wait for active conns then drop */

    double ema_network_ms;
    double ema_processing_ms;
    long   total_connections;
    double last_ema_update_ms;

    int    cb_state;
    int    fail_count;
    time_t open_since;

    int    delay_ms;
    char   sim_mode[16];

    pid_t  pid;
} backend_t;

typedef struct {
    char      listen_ip[MAX_IP_LEN];
    int       listen_port;
    int       algorithm;
    char      backend_bin[256];
    backend_t backends[MAX_BACKENDS];
    int       backend_count;
} lb_config_t;

typedef struct {
    long  total_requests;
    long  rejected_requests;
    int   queue_depth;
    int   warmup_done;
    _Atomic int current_algo;
    long  warmup_total;
    long  warmup_served;
} lb_stats_t;

struct json_object;

void backend_init_runtime(backend_t *b);
int  config_parse_backend_entry(struct json_object *b, backend_t *be);
int  config_load(const char *path, lb_config_t *cfg);

#endif /* LOADFORGE_CONFIG_H */

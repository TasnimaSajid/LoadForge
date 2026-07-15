#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define RST  "\x1b[0m"
#define B    "\x1b[1m"
#define DIM  "\x1b[2m"
#define R    "\x1b[31m"
#define G    "\x1b[32m"
#define Y    "\x1b[33m"
#define BL   "\x1b[34m"
#define C    "\x1b[36m"

#define BACKLOG             64
#define BUF_SIZE            4096
#define CONNECT_TIMEOUT_SEC 2
#define N_WORKERS           8

static int g_active_conns[MAX_BACKENDS];
static int g_half_open_probing[MAX_BACKENDS];
static lb_config_t g_cfg;
static lb_stats_t g_stats;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_stop = 0;
static int g_lfd = -1;
static char g_config_path[512] = "config/config.json";
static int g_forced_algo = -1;
static int g_rr = 0;
static int g_rr_blend = 0;

typedef struct { int fd; struct sockaddr_in peer; } queued_conn_t;
static queued_conn_t g_queue[MAX_QUEUE_LEN];
static int g_q_head = 0, g_q_tail = 0, g_q_count = 0;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_lfd >= 0) close(g_lfd);
    pthread_cond_broadcast(&g_q_cond);
}

static __thread unsigned int tl_rng_seed = 0;

static double thread_local_rand(void) {
    if (tl_rng_seed == 0)
        tl_rng_seed = (unsigned int)((uintptr_t)pthread_self() ^ (unsigned int)time(NULL) ^ 0x9e3779b9u);
    return (double)rand_r(&tl_rng_seed) / ((double)RAND_MAX + 1.0);
}

static int backend_match(const backend_t *a, const backend_t *b) {
    return a->id == b->id && a->port == b->port && strcmp(a->ip, b->ip) == 0;
}

static int find_backend_by_id(int id) {
    for (int i = 0; i < g_cfg.backend_count; i++)
        if (g_cfg.backends[i].id == id) return i;
    return -1;
}

static int cb_allow_locked(int idx) {
    backend_t *b = &g_cfg.backends[idx];

    if (b->cb_state == CB_CLOSED)
        return 1;

    if (b->cb_state == CB_OPEN) {
        if (difftime(time(NULL), b->open_since) >= CB_OPEN_TIMEOUT_SEC) {
            b->cb_state = CB_HALF_OPEN;
            g_half_open_probing[idx] = 0;
            printf(Y "[CB] Backend %d  OPEN → HALF_OPEN  (timeout elapsed)\n" RST, b->id);
            fflush(stdout);
        } else {
            return 0;
        }
    }

    if (b->cb_state == CB_HALF_OPEN) {
        if (!g_half_open_probing[idx]) {
            g_half_open_probing[idx] = 1;
            return 1;
        }
        return 0;
    }
    return 0;
}

static void seed_ema_from_peers(int idx) {
    backend_t *b = &g_cfg.backends[idx];
    double avg_net = 0.0, avg_proc = 0.0;
    int cnt = 0;

    for (int i = 0; i < g_cfg.backend_count; i++) {
        if (i == idx) continue;
        backend_t *o = &g_cfg.backends[i];
        if (o->is_healthy && o->cb_state == CB_CLOSED &&
            (o->ema_network_ms > 0.0 || o->ema_processing_ms > 0.0)) {
            avg_net += o->ema_network_ms;
            avg_proc += o->ema_processing_ms;
            cnt++;
        }
    }
    if (cnt > 0) {
        b->ema_network_ms = avg_net / (double)cnt;
        b->ema_processing_ms = avg_proc / (double)cnt;
    }
}

static void cb_on_success_locked(int idx) {
    backend_t *b = &g_cfg.backends[idx];
    b->fail_count = 0;

    if (b->cb_state == CB_HALF_OPEN) {
        b->cb_state = CB_CLOSED;
        b->is_healthy = 1;
        g_half_open_probing[idx] = 0;
        seed_ema_from_peers(idx);
        printf(G "[CB] Backend %d  HALF_OPEN → CLOSED  (probe succeeded)\n" RST, b->id);
        fflush(stdout);
    }
}

static void cb_on_success(int idx) {
    pthread_mutex_lock(&g_lock);
    cb_on_success_locked(idx);
    pthread_mutex_unlock(&g_lock);
}

static void cb_on_failure_locked(int idx) {
    backend_t *b = &g_cfg.backends[idx];
    b->fail_count++;

    if (b->cb_state == CB_HALF_OPEN) {
        b->cb_state = CB_OPEN;
        b->open_since = time(NULL);
        g_half_open_probing[idx] = 0;
        printf(R "[CB] Backend %d  HALF_OPEN → OPEN  (probe failed)\n" RST, b->id);
    } else if (b->cb_state == CB_CLOSED && b->fail_count >= CB_FAIL_THRESHOLD) {
        b->cb_state = CB_OPEN;
        b->open_since = time(NULL);
        b->is_healthy = 0;
        printf(R "[CB] Backend %d  CLOSED → OPEN  (%d consecutive failures)\n" RST,
               b->id, CB_FAIL_THRESHOLD);
    }
    fflush(stdout);
}

static void cb_on_failure(int idx) {
    pthread_mutex_lock(&g_lock);
    cb_on_failure_locked(idx);
    pthread_mutex_unlock(&g_lock);
}

static void release_active_conn(int idx) {
    pthread_mutex_lock(&g_lock);
    if (g_active_conns[idx] > 0) g_active_conns[idx]--;
    pthread_mutex_unlock(&g_lock);
}

static void update_ema(int idx, double net_ms, double proc_ms) {
    pthread_mutex_lock(&g_lock);
    backend_t *b = &g_cfg.backends[idx];
    double now = now_ms();

    if (b->ema_processing_ms == 0.0 && b->ema_network_ms == 0.0) {
        b->ema_network_ms = net_ms;
        b->ema_processing_ms = proc_ms;
        b->last_ema_update_ms = now;
    } else {
        double dt = now - b->last_ema_update_ms;
        if (dt < 0.0) dt = 0.0;
        double decay = exp(-dt / DECAY_TIME_MS);
        b->ema_network_ms = decay * b->ema_network_ms + (1.0 - decay) * net_ms;
        b->ema_processing_ms = decay * b->ema_processing_ms + (1.0 - decay) * proc_ms;
        b->last_ema_update_ms = now;
    }

    b->total_connections++;
    if (g_active_conns[idx] > 0) g_active_conns[idx]--;
    g_stats.total_requests++;

    if (!g_stats.warmup_done) {
        g_stats.warmup_served++;
        if (g_stats.warmup_served >= g_stats.warmup_total) {
            g_stats.warmup_done = 1;
        }
    }

    pthread_mutex_unlock(&g_lock);
}

static void compact_draining_backends(void) {
    int w = 0;
    for (int r = 0; r < g_cfg.backend_count; r++) {
        backend_t *b = &g_cfg.backends[r];
        if (b->draining && g_active_conns[r] == 0) {
            continue;
        }
        if (w != r) {
            g_cfg.backends[w] = g_cfg.backends[r];
            g_active_conns[w] = g_active_conns[r];
            g_half_open_probing[w] = g_half_open_probing[r];
        }
        w++;
    }
    g_cfg.backend_count = w;
}

static int select_backend(uint32_t skip_mask) {
    pthread_mutex_lock(&g_lock);

    if (g_forced_algo == ALGO_ROUND_ROBIN) {
        int wrr[MAX_BACKENDS * 8];
        int wrr_n = 0;
        for (int i = 0; i < g_cfg.backend_count && wrr_n < (int)(sizeof(wrr) / sizeof(wrr[0])); i++) {
            if (skip_mask & (1u << i)) continue;
            backend_t *b = &g_cfg.backends[i];
            if (b->draining || !b->is_healthy || b->cb_state == CB_OPEN) continue;
            if (!cb_allow_locked(i)) continue;
            int w = b->weight > 0 ? b->weight : 1;
            if (w > 8) w = 8;
            for (int j = 0; j < w && wrr_n < (int)(sizeof(wrr) / sizeof(wrr[0])); j++)
                wrr[wrr_n++] = i;
        }
        if (wrr_n == 0) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        int idx = wrr[g_rr % wrr_n];
        g_rr = (g_rr + 1) % wrr_n;
        atomic_store(&g_stats.current_algo, ALGO_ROUND_ROBIN);
        pthread_mutex_unlock(&g_lock);
        return idx;
    }

    double adaptive_weight = g_stats.warmup_done
        ? 1.0
        : (g_stats.warmup_total > 0
           ? (double)g_stats.warmup_served / (double)g_stats.warmup_total
           : 1.0);

    int eligible[MAX_BACKENDS], n = 0;
    double raw[MAX_BACKENDS];
    double sum_raw = 0.0;
    memset(raw, 0, sizeof(raw));

    for (int i = 0; i < g_cfg.backend_count; i++) {
        if (skip_mask & (1u << i)) continue;
        backend_t *b = &g_cfg.backends[i];
        if (b->draining || !b->is_healthy || b->cb_state == CB_OPEN) continue;
        if (!cb_allow_locked(i)) continue;

        eligible[n++] = i;

        double lat = NETWORK_WEIGHT * b->ema_network_ms
                   + PROCESSING_WEIGHT * b->ema_processing_ms
                   + (double)g_active_conns[i] * ACTIVE_CONN_PENALTY_MS;
        if (lat <= 0.0) lat = 1.0;

        raw[i] = exp(-lat / SOFTMAX_TEMP);
        sum_raw += raw[i];
    }

    if (n == 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    double weights[MAX_BACKENDS];
    memset(weights, 0, sizeof(weights));
    double floor_per = MIN_TRAFFIC_FRACTION;
    double adaptive_share = 1.0 - (double)n * floor_per;
    if (adaptive_share < 0.0) adaptive_share = 0.0;

    double total_weight = 0.0;
    for (int k = 0; k < n; k++) {
        int i = eligible[k];
        double norm = (sum_raw > 0.0) ? raw[i] / sum_raw : 1.0 / (double)n;
        weights[i] = floor_per + adaptive_share * norm;
        total_weight += weights[i];
    }

    int idx;
    if (adaptive_weight < 0.05) {
        idx = eligible[g_rr % n];
        g_rr = (g_rr + 1) % n;
        atomic_store(&g_stats.current_algo, ALGO_ROUND_ROBIN);
    } else {
        int use_rr = 0;
        if (g_forced_algo != ALGO_ADAPTIVE && adaptive_weight < 1.0 &&
            thread_local_rand() > adaptive_weight)
            use_rr = 1;

        if (use_rr) {
            idx = eligible[g_rr_blend % n];
            g_rr_blend = (g_rr_blend + 1) % n;
            atomic_store(&g_stats.current_algo, ALGO_ROUND_ROBIN);
        } else {
            double r = thread_local_rand() * total_weight;
            double cum = 0.0;
            idx = eligible[0];
            for (int k = 0; k < n; k++) {
                cum += weights[eligible[k]];
                if (r <= cum) {
                    idx = eligible[k];
                    break;
                }
            }
            atomic_store(&g_stats.current_algo, ALGO_ADAPTIVE);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return idx;
}

static int pick_backend(uint32_t skip) {
    int saved = g_forced_algo;
    g_forced_algo = ALGO_ROUND_ROBIN;
    int r = select_backend(skip);
    g_forced_algo = saved;
    return r;
}

static int pick_backend_adaptive(uint32_t skip) {
    int saved = g_forced_algo;
    g_forced_algo = ALGO_ADAPTIVE;
    int r = select_backend(skip);
    g_forced_algo = saved;
    return r;
}

static int tcp_connect_nb(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { close(fd); return -1; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }

    if (rc < 0) {
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_SEC, .tv_usec = 0 };
        if (select(fd + 1, NULL, &ws, NULL, &tv) <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) { close(fd); return -1; }
    }

    fcntl(fd, F_SETFL, flags);
    return fd;
}

static double parse_process_ms(const char *buf) {
    const char *p = strstr(buf, "PROCESS_TIME_MS=");
    if (!p) return -1.0;
    return atof(p + 16);
}

static void handle_connection(int client_fd, struct sockaddr_in peer) {
    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));

    int bfd = -1, idx = -1;
    int conn_charged = 0;
    double t_connect_start, t_connect_done;
    uint32_t tried = 0;

    for (int attempt = 0; attempt < g_cfg.backend_count; attempt++) {
        idx = (g_forced_algo == ALGO_ROUND_ROBIN) ? pick_backend(tried)
                                                  : pick_backend_adaptive(tried);
        if (idx < 0) break;
        tried |= (1u << idx);

        t_connect_start = now_ms();
        bfd = tcp_connect_nb(g_cfg.backends[idx].ip, g_cfg.backends[idx].port);
        t_connect_done = now_ms();

        if (bfd >= 0) {
            pthread_mutex_lock(&g_lock);
            g_active_conns[idx]++;
            conn_charged = 1;
            int bid = g_cfg.backends[idx].id;
            pthread_mutex_unlock(&g_lock);

            double net_ms = t_connect_done - t_connect_start;

            char req_buf[BUF_SIZE] = {0};
            ssize_t req_n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
            int ok = 0;
            double proc_ms = 0.0;

            if (req_n > 0) {
                ssize_t sent = 0;
                while (sent < req_n) {
                    ssize_t w = send(bfd, req_buf + sent, (size_t)(req_n - sent), 0);
                    if (w <= 0) break;
                    sent += w;
                }
                if (sent == req_n) {
                    char resp_buf[BUF_SIZE] = {0};
                    ssize_t resp_n = recv(bfd, resp_buf, sizeof(resp_buf) - 1, 0);
                    if (resp_n > 0) {
                        resp_buf[resp_n] = '\0';
                        double parsed = parse_process_ms(resp_buf);
                        if (parsed >= 0) proc_ms = parsed;

                        sent = 0;
                        while (sent < resp_n) {
                            ssize_t w = send(client_fd, resp_buf + sent, (size_t)(resp_n - sent), 0);
                            if (w <= 0) break;
                            sent += w;
                        }
                        if (sent == resp_n) {
                            update_ema(idx, net_ms, proc_ms);
                            conn_charged = 0;
                            cb_on_success(idx);
                            ok = 1;
                        }
                    }
                }
            }

            if (!ok) {
                if (conn_charged) {
                    release_active_conn(idx);
                    conn_charged = 0;
                }
                close(bfd);
                bfd = -1;
                cb_on_failure(idx);
                pthread_mutex_lock(&g_lock);
                int fc = g_cfg.backends[idx].fail_count;
                pthread_mutex_unlock(&g_lock);
                printf("  " R "[✗]" RST " Backend %d: request failed (fail %d/%d)\n",
                       bid, fc, CB_FAIL_THRESHOLD);
                fflush(stdout);
                continue;
            }
            break;
        } else {
            cb_on_failure(idx);
            pthread_mutex_lock(&g_lock);
            int bid = g_cfg.backends[idx].id;
            int fc = g_cfg.backends[idx].fail_count;
            pthread_mutex_unlock(&g_lock);
            printf("  " R "[✗]" RST " Backend %d: connect failed (fail %d/%d)\n",
                   bid, fc, CB_FAIL_THRESHOLD);
            fflush(stdout);
        }
    }

    if (conn_charged)
        release_active_conn(idx);

    if (bfd < 0) {
        const char *msg = "LoadForge 503 Service Unavailable: no healthy backends\n";
        send(client_fd, msg, strlen(msg), 0);
        printf("  " R "[503]" RST " No healthy backends for %s\n", peer_ip);
        fflush(stdout);
    }

    close(client_fd);
    if (bfd >= 0) close(bfd);

    pthread_mutex_lock(&g_lock);
    compact_draining_backends();
    pthread_mutex_unlock(&g_lock);
}

static void *queue_worker(void *arg) {
    (void)arg;
    while (!g_stop) {
        pthread_mutex_lock(&g_q_lock);
        while (g_q_count == 0 && !g_stop)
            pthread_cond_wait(&g_q_cond, &g_q_lock);

        if (g_stop && g_q_count == 0) {
            pthread_mutex_unlock(&g_q_lock);
            break;
        }

        queued_conn_t conn = g_queue[g_q_head];
        g_q_head = (g_q_head + 1) % MAX_QUEUE_LEN;
        g_q_count--;

        pthread_mutex_lock(&g_lock);
        g_stats.queue_depth = g_q_count;
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&g_q_lock);

        handle_connection(conn.fd, conn.peer);
    }
    return NULL;
}

static void *health_thread(void *arg) {
    (void)arg;
    while (!g_stop) {
        for (int i = 0; i < g_cfg.backend_count && !g_stop; i++) {
            pthread_mutex_lock(&g_lock);
            if (g_cfg.backends[i].draining) {
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            char ip[MAX_IP_LEN];
            int port = g_cfg.backends[i].port;
            int id = g_cfg.backends[i].id;
            int cb = g_cfg.backends[i].cb_state;
            snprintf(ip, sizeof(ip), "%s", g_cfg.backends[i].ip);
            pthread_mutex_unlock(&g_lock);

            if (cb == CB_OPEN) continue;

            int fd = tcp_connect_nb(ip, port);
            int ok = (fd >= 0);

            if (ok) {
                const char *ping = "HEALTHCHECK\n";
                send(fd, ping, strlen(ping), 0);
                char buf[128];
                recv(fd, buf, sizeof(buf) - 1, 0);
                close(fd);
            }

            pthread_mutex_lock(&g_lock);
            backend_t *b = &g_cfg.backends[i];
            if (ok) {
                if (!b->is_healthy) {
                    b->is_healthy = 1;
                }
                if (b->cb_state == CB_HALF_OPEN)
                    cb_on_success_locked(i);
            } else if (b->is_healthy && b->cb_state != CB_OPEN) {
                printf(R "[Health] Backend %d is UNREACHABLE\n" RST, id);
                b->is_healthy = 0;
            }
            fflush(stdout);
            pthread_mutex_unlock(&g_lock);
        }

        for (int t = 0; t < HEALTH_INTERVAL_SEC && !g_stop; t++) sleep(1);
    }
    return NULL;
}

static pid_t launch_backend(int idx) {
    if (g_cfg.backend_bin[0] == '\0') return -1;

    backend_t *b = &g_cfg.backends[idx];
    char port_str[16], id_str[16];
    snprintf(port_str, sizeof(port_str), "%d", b->port);
    snprintf(id_str, sizeof(id_str), "%d", b->id);

    pid_t pid = fork();
    if (pid < 0) { perror("watchdog: fork"); return -1; }

    if (pid == 0) {
        char logfile[64];
        snprintf(logfile, sizeof(logfile), "/tmp/lf_b%d.log", b->id);
        freopen(logfile, "a", stdout);
        freopen(logfile, "a", stderr);

        if (b->sim_mode[0] != '\0')
            execl(g_cfg.backend_bin, g_cfg.backend_bin, b->ip, port_str, id_str, b->sim_mode, NULL);
        else if (b->delay_ms > 0) {
            char delay_str[16];
            snprintf(delay_str, sizeof(delay_str), "%d", b->delay_ms);
            execl(g_cfg.backend_bin, g_cfg.backend_bin, b->ip, port_str, id_str, delay_str, NULL);
        } else
            execl(g_cfg.backend_bin, g_cfg.backend_bin, b->ip, port_str, id_str, NULL);
        perror("watchdog: execl");
        _exit(1);
    }
    return pid;
}

static int tcp_probe(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    connect(fd, (struct sockaddr *)&a, sizeof(a));

    fd_set ws;
    FD_ZERO(&ws);
    FD_SET(fd, &ws);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int rc = select(fd + 1, NULL, &ws, NULL, &tv);
    int ok = 0;
    if (rc > 0) {
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        ok = (err == 0);
    }
    close(fd);
    return ok;
}

static void *watchdog_thread(void *arg) {
    (void)arg;
    sleep(2);

    while (!g_stop) {
        waitpid(-1, NULL, WNOHANG);

        for (int i = 0; i < g_cfg.backend_count && !g_stop; i++) {
            pthread_mutex_lock(&g_lock);
            if (g_cfg.backends[i].draining) {
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            pid_t pid = g_cfg.backends[i].pid;
            int port = g_cfg.backends[i].port;
            int id = g_cfg.backends[i].id;
            char ip[MAX_IP_LEN];
            snprintf(ip, sizeof(ip), "%s", g_cfg.backends[i].ip);
            pthread_mutex_unlock(&g_lock);

            int alive = (pid > 0) && (kill(pid, 0) == 0);
            if (alive) continue;

            if (tcp_probe(ip, port)) {
                pthread_mutex_lock(&g_lock);
                backend_t *b = &g_cfg.backends[i];
                if (!b->is_healthy && b->cb_state == CB_OPEN) {
                    b->is_healthy = 1;
                    b->fail_count = 0;
                    b->cb_state = CB_HALF_OPEN;
                    g_half_open_probing[i] = 0;
                    printf(G "[WD] Backend %d (%s:%d)  listening → HALF_OPEN\n" RST, id, ip, port);
                    fflush(stdout);
                }
                pthread_mutex_unlock(&g_lock);
            } else {
                printf(Y "[WD] Backend %d (%s:%d)  DOWN → restarting\n" RST, id, ip, port);
                fflush(stdout);

                pid_t npid = launch_backend(i);
                int up = 0;
                for (int w = 0; w < 6 && !g_stop; w++) {
                    usleep(500000);
                    if (tcp_probe(ip, port)) { up = 1; break; }
                }

                pthread_mutex_lock(&g_lock);
                g_cfg.backends[i].pid = npid;
                if (up) {
                    g_cfg.backends[i].cb_state = CB_HALF_OPEN;
                    g_cfg.backends[i].is_healthy = 1;
                    g_cfg.backends[i].fail_count = 0;
                    g_half_open_probing[i] = 0;
                    printf(G "[WD] Backend %d (%s:%d)  restarted (PID %d) → HALF_OPEN\n" RST,
                           id, ip, port, (int)npid);
                } else {
                    printf(R "[WD] Backend %d (%s:%d)  restarted but not yet listening\n" RST, id, ip, port);
                }
                fflush(stdout);
                pthread_mutex_unlock(&g_lock);
            }
        }

        for (int t = 0; t < WATCHDOG_INTERVAL_SEC && !g_stop; t++) sleep(1);
    }
    return NULL;
}

static const char *cb_str(int s) {
    switch (s) {
        case CB_CLOSED: return "CLOSED";
        case CB_OPEN: return "OPEN";
        case CB_HALF_OPEN: return "HALF_OPEN";
        default: return "?";
    }
}

static const char *cb_col(int s) {
    switch (s) {
        case CB_CLOSED: return G;
        case CB_OPEN: return R;
        default: return Y;
    }
}

static void format_status_json(char *out, size_t outsz) {
    size_t off = 0;
    off += (size_t)snprintf(out + off, outsz - off, "{\"backends\":[");
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_cfg.backend_count && off < outsz - 64; i++) {
        backend_t *b = &g_cfg.backends[i];
        if (i) off += (size_t)snprintf(out + off, outsz - off, ",");
        off += (size_t)snprintf(out + off, outsz - off,
            "{\"id\":%d,\"ip\":\"%s\",\"port\":%d,\"healthy\":%d,\"cb\":\"%s\","
            "\"net_ms\":%.2f,\"proc_ms\":%.2f,\"active\":%d,\"draining\":%d}",
            b->id, b->ip, b->port, b->is_healthy, cb_str(b->cb_state),
            b->ema_network_ms, b->ema_processing_ms, g_active_conns[i], b->draining);
    }
    int algo = atomic_load(&g_stats.current_algo);
    pthread_mutex_unlock(&g_lock);
    snprintf(out + off, outsz - off, "],\"algo\":\"%s\",\"warmup_done\":%d}",
             algo == ALGO_ADAPTIVE ? "adaptive" : "round_robin", g_stats.warmup_done);
}

static void persist_backend_to_config(const char *ip, int port, const char *sim) {
    int n;
    if (sim && sim[0])
        n = snprintf(NULL, 0, "scripts/add_backend.sh %s %d %s %s 2>/dev/null",
                     ip, port, sim, g_config_path);
    else
        n = snprintf(NULL, 0, "scripts/add_backend.sh %s %d '' %s 2>/dev/null",
                     ip, port, g_config_path);
    if (n < 0)
        return;
    char *cmd = malloc((size_t)n + 1);
    if (!cmd)
        return;
    if (sim && sim[0])
        snprintf(cmd, (size_t)n + 1, "scripts/add_backend.sh %s %d %s %s 2>/dev/null",
                 ip, port, sim, g_config_path);
    else
        snprintf(cmd, (size_t)n + 1, "scripts/add_backend.sh %s %d '' %s 2>/dev/null",
                 ip, port, g_config_path);
    int rc = system(cmd);
    free(cmd);
    if (rc != 0)
        fprintf(stderr, R "[LB] Config update failed\n" RST);
}

static int sim_mode_valid(const char *sim) {
    if (!sim || sim[0] == '\0') return 1;
    return strcmp(sim, "cpu") == 0 || strcmp(sim, "memory") == 0 || strcmp(sim, "io") == 0;
}

static int add_backend_locked(const char *ip, int port, const char *sim_mode) {
    if (g_cfg.backend_count >= MAX_BACKENDS) return -1;
    if (sim_mode && sim_mode[0] && !sim_mode_valid(sim_mode)) return -1;

    int max_id = 0;
    for (int i = 0; i < g_cfg.backend_count; i++)
        if (g_cfg.backends[i].id > max_id) max_id = g_cfg.backends[i].id;

    backend_t *b = &g_cfg.backends[g_cfg.backend_count];
    memset(b, 0, sizeof(*b));
    b->id = max_id + 1;
    snprintf(b->ip, sizeof(b->ip), "%s", ip);
    b->port = port;
    b->weight = 1;
    if (sim_mode && sim_mode[0])
        snprintf(b->sim_mode, sizeof(b->sim_mode), "%s", sim_mode);
    backend_init_runtime(b);
    g_active_conns[g_cfg.backend_count] = 0;
    g_half_open_probing[g_cfg.backend_count] = 0;
    g_cfg.backend_count++;
    g_stats.warmup_total = (long)g_cfg.backend_count * WARMUP_PER_BACKEND;
    return b->id;
}

static int remove_backend_by_id_locked(int id) {
    int idx = find_backend_by_id(id);
    if (idx < 0) return -1;
    g_cfg.backends[idx].draining = 1;
    g_cfg.backends[idx].is_healthy = 0;
    return 0;
}

static void merge_config_reload(const lb_config_t *fresh) {
    pthread_mutex_lock(&g_lock);

    for (int ni = 0; ni < fresh->backend_count; ni++) {
        const backend_t *nb = &fresh->backends[ni];
        int found = -1;
        for (int oi = 0; oi < g_cfg.backend_count; oi++) {
            if (backend_match(nb, &g_cfg.backends[oi])) {
                found = oi;
                g_cfg.backends[oi].weight = nb->weight;
                g_cfg.backends[oi].delay_ms = nb->delay_ms;
                snprintf(g_cfg.backends[oi].sim_mode, sizeof(g_cfg.backends[oi].sim_mode),
                         "%s", nb->sim_mode);
                break;
            }
        }
        if (found < 0 && g_cfg.backend_count < MAX_BACKENDS) {
            g_cfg.backends[g_cfg.backend_count] = *nb;
            g_active_conns[g_cfg.backend_count] = 0;
            g_half_open_probing[g_cfg.backend_count] = 0;
            g_cfg.backend_count++;
        }
    }

    for (int oi = 0; oi < g_cfg.backend_count; oi++) {
        int still = 0;
        for (int ni = 0; ni < fresh->backend_count; ni++) {
            if (backend_match(&fresh->backends[ni], &g_cfg.backends[oi])) {
                still = 1;
                break;
            }
        }
        if (!still && !g_cfg.backends[oi].draining) {
            g_cfg.backends[oi].draining = 1;
            g_cfg.backends[oi].is_healthy = 0;
        }
    }

    if (fresh->backend_bin[0])
        snprintf(g_cfg.backend_bin, sizeof(g_cfg.backend_bin), "%s", fresh->backend_bin);

    compact_draining_backends();
    g_stats.warmup_total = (long)g_cfg.backend_count * WARMUP_PER_BACKEND;
    pthread_mutex_unlock(&g_lock);
}

static void handle_control_line(const char *line, int cfd) {
    char resp[4096];
    resp[0] = '\0';

    if (strncmp(line, "STATUS", 6) == 0) {
        format_status_json(resp, sizeof(resp));
    } else if (strncmp(line, "ADD ", 4) == 0) {
        char ip[MAX_IP_LEN], sim[16] = "";
        int port = 0;
        int n = sscanf(line + 4, "%63s %d %15s", ip, &port, sim);
        if (n < 2) {
            snprintf(resp, sizeof(resp), "ERR usage: ADD <ip> <port> [sim_mode]\n");
        } else {
            pthread_mutex_lock(&g_lock);
            int nid = add_backend_locked(ip, port, n >= 3 ? sim : "");
            pthread_mutex_unlock(&g_lock);
            if (nid < 0)
                snprintf(resp, sizeof(resp), "ERR backend table full\n");
            else {
                snprintf(resp, sizeof(resp), "OK added backend %d at %s:%d\n", nid, ip, port);
                persist_backend_to_config(ip, port, n >= 3 ? sim : "");
            }
        }
    } else if (strncmp(line, "REMOVE ", 7) == 0) {
        int id = atoi(line + 7);
        pthread_mutex_lock(&g_lock);
        int rc = remove_backend_by_id_locked(id);
        compact_draining_backends();
        pthread_mutex_unlock(&g_lock);
        if (rc < 0)
            snprintf(resp, sizeof(resp), "ERR backend %d not found\n", id);
        else
            snprintf(resp, sizeof(resp), "OK draining backend %d\n", id);
    } else if (strncmp(line, "SET_ALGO ", 9) == 0) {
        const char *mode = line + 9;
        if (strncmp(mode, "round_robin", 11) == 0) {
            g_forced_algo = ALGO_ROUND_ROBIN;
            snprintf(resp, sizeof(resp), "OK algo=round_robin\n");
        } else if (strncmp(mode, "adaptive", 8) == 0) {
            g_forced_algo = ALGO_ADAPTIVE;
            snprintf(resp, sizeof(resp), "OK algo=adaptive\n");
        } else if (strncmp(mode, "auto", 4) == 0) {
            g_forced_algo = -1;
            snprintf(resp, sizeof(resp), "OK algo=auto\n");
        } else {
            snprintf(resp, sizeof(resp), "ERR use round_robin|adaptive|auto\n");
        }
    } else {
        snprintf(resp, sizeof(resp),
                 "ERR unknown command. Use: STATUS | ADD ip port [sim] | REMOVE id | SET_ALGO mode\n");
    }

    send(cfd, resp, strlen(resp), 0);
}

static void *control_thread(void *arg) {
    (void)arg;
    unlink(LB_CTRL_SOCKET);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { perror("control socket"); return NULL; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LB_CTRL_SOCKET);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("control bind");
        close(sfd);
        return NULL;
    }
    listen(sfd, 8);

    while (!g_stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sfd, &rf);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(sfd + 1, &rf, NULL, NULL, &tv) <= 0) continue;

        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;

        char buf[512];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            handle_control_line(buf, cfd);
        }
        close(cfd);
    }

    close(sfd);
    unlink(LB_CTRL_SOCKET);
    return NULL;
}

static void *config_reload_thread(void *arg) {
    (void)arg;
    struct stat st;
    time_t last_mtime = 0;

    if (stat(g_config_path, &st) == 0)
        last_mtime = st.st_mtime;

    while (!g_stop) {
        for (int t = 0; t < CONFIG_RELOAD_POLL_SEC && !g_stop; t++) sleep(1);
        if (g_stop) break;

        if (stat(g_config_path, &st) != 0) continue;
        if (st.st_mtime == last_mtime) continue;
        last_mtime = st.st_mtime;

        lb_config_t fresh;
        if (config_load(g_config_path, &fresh) != 0) {
            continue;
        }
        merge_config_reload(&fresh);
    }
    return NULL;
}

static void *stats_logger_thread(void *arg) {
    (void)arg;
    while (!g_stop) {
        for (int t = 0; t < STATS_LOGGER_INTERVAL_SEC && !g_stop; t++)
            sleep(1);
        if (g_stop) break;

        char buf[4096];
        format_status_json(buf, sizeof(buf));
        FILE *f = fopen(STATS_LOG_PATH, "a");
        if (f) {
            time_t now = time(NULL);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));
            fprintf(f, "%s %s\n", ts, buf);
            fclose(f);
        }
    }
    return NULL;
}

static void *dashboard_thread(void *arg) {
    (void)arg;
    static int first_frame = 1;
    static int last_frame_lines = 0;

    while (!g_stop) {
        for (int t = 0; t < DASHBOARD_INTERVAL_SEC && !g_stop; t++) sleep(1);
        if (g_stop) break;

        if (!first_frame)
            printf("\x1b[%dA\x1b[J", last_frame_lines);
        first_frame = 0;
        printf(B C "LOADFORGE MONITOR\n" RST);

        pthread_mutex_lock(&g_lock);
        last_frame_lines = g_cfg.backend_count + 6;
        const char *algo = atomic_load(&g_stats.current_algo) == ALGO_ADAPTIVE
                           ? "adaptive"
                           : "round-robin";
        printf("Requests: " B "%ld" RST "  Algorithm: " B "%s" RST "\n",
               g_stats.total_requests, algo);
        printf(DIM "----------------------------------------------------------------\n" RST);
        printf(B "%-4s  %-20s  %-7s  %-9s  %8s  %s\n" RST,
               "ID", "Address", "Health", "Circuit", "EMA(ms)", "Conns");
        printf(DIM "----------------------------------------------------------------\n" RST);

        for (int i = 0; i < g_cfg.backend_count; i++) {
            backend_t *b = &g_cfg.backends[i];
            char addr[32];
            snprintf(addr, sizeof(addr), "%s:%d", b->ip, b->port);

            const char *hcol, *hs;
            if (b->draining)           { hcol = Y; hs = "DRAIN"; }
            else if (b->is_healthy)    { hcol = G; hs = "OK";    }
            else                       { hcol = R; hs = "DOWN";  }

            double ema_ms = b->ema_network_ms + b->ema_processing_ms;

            printf(B "%-4d" RST "  %-20s  %s%-7s" RST "  %s%-9s" RST
                   "  %8.2f  %ld\n",
                   b->id, addr, hcol, hs,
                   cb_col(b->cb_state), cb_str(b->cb_state),
                   ema_ms, b->total_connections);
        }
        printf(DIM "----------------------------------------------------------------\n" RST);
        fflush(stdout);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static int make_listener(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void show_splash(void) {
    printf("\n");
    printf(B C "  ██╗      ██████╗  █████╗ ██████╗ ███████╗ ██████╗ ██████╗  ██████╗ ███████╗\n");
    printf("  ██║     ██╔═══██╗██╔══██╗██╔══██╗██╔════╝██╔═══██╗██╔══██╗██╔════╝██╔════╝\n");
    printf("  ██║     ██║   ██║███████║██║  ██║█████╗  ██║   ██║██████╔╝██║  ███╗█████╗  \n");
    printf("  ██║     ██║   ██║██╔══██║██║  ██║██╔══╝  ██║   ██║██╔══██╗██║   ██║██╔══╝  \n");
    printf("  ███████╗╚██████╔╝██║  ██║██████╔╝██║     ╚██████╔╝██║  ██║╚██████╔╝███████╗\n");
    printf("  ╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚═════╝ ╚═╝      ╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚══════╝\n" RST);
    printf("\n");
    printf(B "  LoadForge — Adaptive TCP Load Balancer\n\n" RST);
    printf(DIM "  Round-robin · Adaptive EMA · Circuit Breaker · Statistics Monitor\n\n" RST);

    if (isatty(STDIN_FILENO)) {
        printf(B "  Press Enter to start..." RST "\n\n");
        fflush(stdout);
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
    }
}

int main(int argc, char **argv) {
    const char *cfg_path = (argc >= 2) ? argv[1] : "config/config.json";
    snprintf(g_config_path, sizeof(g_config_path), "%s", cfg_path);

    if (config_load(cfg_path, &g_cfg) != 0) {
        fprintf(stderr, "Failed to load config\n");
        return 1;
    }

    memset(g_active_conns, 0, sizeof(g_active_conns));
    memset(g_half_open_probing, 0, sizeof(g_half_open_probing));

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.warmup_total = (long)g_cfg.backend_count * WARMUP_PER_BACKEND;
    g_stats.warmup_done = 0;
    atomic_store(&g_stats.current_algo, ALGO_ROUND_ROBIN);

    if (g_cfg.algorithm == ALGO_ADAPTIVE)
        g_forced_algo = ALGO_ADAPTIVE;

    show_splash();

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGCHLD, SIG_DFL);

    int lfd = make_listener(g_cfg.listen_ip, g_cfg.listen_port);
    if (lfd < 0) return 1;
    g_lfd = lfd;

    printf(G "[LB] Listening on %s:%d\n" RST, g_cfg.listen_ip, g_cfg.listen_port);
    printf("[LB] Backends: %d  Algorithm: %s\n\n",
           g_cfg.backend_count,
           g_forced_algo == ALGO_ADAPTIVE ? "adaptive" : "round-robin");
    fflush(stdout);

    pthread_t health_tid, watchdog_tid, dashboard_tid, control_tid, reload_tid, stats_logger_tid;
    pthread_create(&health_tid, NULL, health_thread, NULL);
    pthread_create(&watchdog_tid, NULL, watchdog_thread, NULL);
    pthread_create(&dashboard_tid, NULL, dashboard_thread, NULL);
    pthread_create(&control_tid, NULL, control_thread, NULL);
    pthread_create(&reload_tid, NULL, config_reload_thread, NULL);
    pthread_create(&stats_logger_tid, NULL, stats_logger_thread, NULL);

    pthread_t workers[N_WORKERS];
    for (int i = 0; i < N_WORKERS; i++)
        pthread_create(&workers[i], NULL, queue_worker, NULL);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);

        if (cfd < 0) {
            if (g_stop) break;
            continue;
        }

        pthread_mutex_lock(&g_q_lock);
        if (g_q_count >= MAX_QUEUE_LEN) {
            pthread_mutex_unlock(&g_q_lock);
            const char *msg = "LoadForge 503 Service Unavailable: queue full\n";
            send(cfd, msg, strlen(msg), 0);
            close(cfd);

            pthread_mutex_lock(&g_lock);
            g_stats.rejected_requests++;
            pthread_mutex_unlock(&g_lock);

            printf(R "[LB] Queue full — request rejected\n" RST);
            fflush(stdout);
            continue;
        }

        g_queue[g_q_tail].fd = cfd;
        g_queue[g_q_tail].peer = peer;
        g_q_tail = (g_q_tail + 1) % MAX_QUEUE_LEN;
        g_q_count++;

        pthread_mutex_lock(&g_lock);
        g_stats.queue_depth = g_q_count;
        pthread_mutex_unlock(&g_lock);

        pthread_cond_signal(&g_q_cond);
        pthread_mutex_unlock(&g_q_lock);
    }

    printf("\n" Y "[LB] Shutting down...\n" RST "\n");
    g_stop = 1;
    pthread_cond_broadcast(&g_q_cond);

    for (int i = 0; i < N_WORKERS; i++) pthread_join(workers[i], NULL);
    pthread_join(health_tid, NULL);
    pthread_join(watchdog_tid, NULL);
    pthread_join(dashboard_tid, NULL);
    pthread_join(control_tid, NULL);
    pthread_join(reload_tid, NULL);
    pthread_join(stats_logger_tid, NULL);

    if (g_lfd >= 0) close(g_lfd);
    return 0;
}

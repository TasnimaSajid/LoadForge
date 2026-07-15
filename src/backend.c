#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BACKLOG    32
#define BUF_SIZE   2048

#define SIM_NONE    0
#define SIM_DELAY   1
#define SIM_CPU     2
#define SIM_MEMORY  3
#define SIM_IO      4

static char g_ip[64];
static int  g_port;
static int  g_id;
static int  g_sim_mode  = SIM_NONE;
static int  g_delay_ms  = 0;

static volatile sig_atomic_t g_stop    = 0;
static int                   g_lfd     = -1;
static long                  g_served  = 0;
static pthread_mutex_t       g_lock    = PTHREAD_MUTEX_INITIALIZER;

static void on_signal(int s) { (void)s; g_stop = 1; if (g_lfd >= 0) close(g_lfd); }

static void sim_cpu(void) {
    const int N      = 2000000;
    const int PASSES = 4;
    char *sieve = malloc((size_t)(N + 1));
    if (!sieve) return;
    volatile long count = 0;
    for (int p = 0; p < PASSES; p++) {
        memset(sieve, 1, (size_t)(N + 1));
        for (int i = 2; (long)i * i <= N; i++)
            if (sieve[i])
                for (int j = i * i; j <= N; j += i)
                    sieve[j] = 0;
        for (int i = 2; i <= N; i++)
            if (sieve[i]) count++;
    }
    free(sieve);
    (void)count;
}

static void sim_memory(void) {
    const size_t SZ = 4 * 1024 * 1024;
    char *buf = malloc(SZ);
    if (!buf) return;
    memset(buf, 0xAB, SZ);
    volatile char x = buf[SZ / 2];
    (void)x;
    free(buf);
}

static void sim_io(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/lf_io_%d_%ld.tmp", g_id, (long)getpid());
    FILE *f = fopen(path, "w+");
    if (!f) return;
    char block[4096];
    memset(block, 'X', sizeof(block));
    for (int i = 0; i < 256; i++) fwrite(block, 1, sizeof(block), f);
    rewind(f);
    size_t tot = 0;
    while (!feof(f)) tot += fread(block, 1, sizeof(block), f);
    fclose(f);
    remove(path);
    (void)tot;
}

static double run_simulation(void) {
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    switch (g_sim_mode) {
        case SIM_DELAY:  usleep((useconds_t)g_delay_ms * 1000); break;
        case SIM_CPU:    sim_cpu();    break;
        case SIM_MEMORY: sim_memory(); break;
        case SIM_IO:     sim_io();     break;
        default: break;
    }

    gettimeofday(&t1, NULL);
    return (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
}

typedef struct { int fd; struct sockaddr_in peer; } client_arg_t;

static void *handle_client(void *arg) {
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ca->peer.sin_addr, peer_ip, sizeof(peer_ip));
    int peer_port = ntohs(ca->peer.sin_port);
    free(ca);

    char buf[BUF_SIZE];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) buf[n] = '\0';

    if (strcmp(buf, "HEALTHCHECK\n") == 0) {
        send(fd, "OK\n", 3, 0);
        close(fd);
        return NULL;
    }

    double proc_ms = run_simulation();

    char reply[BUF_SIZE];
    int len = snprintf(reply, sizeof(reply),
        "BACKEND_ID=%d PROCESS_TIME_MS=%.2f MSG=Hello from Backend %d (%s:%d)\n",
        g_id, proc_ms, g_id, g_ip, g_port);
    send(fd, reply, (size_t)len, 0);

    pthread_mutex_lock(&g_lock);
    g_served++;
    long sv = g_served;
    pthread_mutex_unlock(&g_lock);

    printf("[B%d] %s:%d  %6.1fms  #%ld\n",
           g_id, peer_ip, peer_port, proc_ms, sv);
    fflush(stdout);

    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <ip> <port> <id> [delay_ms|cpu|memory|io]\n", argv[0]);
        return 1;
    }
    snprintf(g_ip, sizeof(g_ip), "%s", argv[1]);
    g_port = atoi(argv[2]);
    g_id   = atoi(argv[3]);

    if (argc >= 5) {
        const char *mode = argv[4];
        if (strcmp(mode, "cpu")    == 0) g_sim_mode = SIM_CPU;
        else if (strcmp(mode, "memory") == 0) g_sim_mode = SIM_MEMORY;
        else if (strcmp(mode, "io")     == 0) g_sim_mode = SIM_IO;
        else {
            g_delay_ms = atoi(mode);
            if (g_delay_ms < 0) g_delay_ms = 0;
            if (g_delay_ms > 30000) g_delay_ms = 30000;
            g_sim_mode = g_delay_ms > 0 ? SIM_DELAY : SIM_NONE;
        }
    }

    const char *sim_name = "none";
    switch (g_sim_mode) {
        case SIM_CPU:    sim_name = "cpu-intensive";    break;
        case SIM_MEMORY: sim_name = "memory-intensive"; break;
        case SIM_IO:     sim_name = "io-intensive";     break;
        case SIM_DELAY:  sim_name = "sleep-delay";      break;
        default: break;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid ip '%s'\n", g_ip); close(lfd); return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(lfd); return 1;
    }
    if (listen(lfd, BACKLOG) < 0) {
        perror("listen"); close(lfd); return 1;
    }

    g_lfd = lfd;
    struct sigaction sa = {0}; sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    if (g_sim_mode == SIM_DELAY)
        printf("[Backend %d] %s:%d  mode=delay(%dms)\n", g_id, g_ip, g_port, g_delay_ms);
    else if (g_sim_mode != SIM_NONE)
        printf("[Backend %d] %s:%d  mode=%s\n", g_id, g_ip, g_port, sim_name);
    else
        printf("[Backend %d] %s:%d  mode=passthrough\n", g_id, g_ip, g_port);
    fflush(stdout);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) { if (g_stop) break; perror("accept"); continue; }

        client_arg_t *ca = malloc(sizeof(*ca));
        if (!ca) { close(cfd); continue; }
        ca->fd   = cfd;
        ca->peer = peer;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ca) != 0) {
            free(ca); close(cfd); continue;
        }
        pthread_detach(tid);
    }

    printf("[Backend %d] shutting down (served %ld requests)\n", g_id, g_served);
    close(g_lfd);
    return 0;
}

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define BUF_SIZE 2048
#define MAX_BE   16

#define RST "\x1b[0m"
#define B   "\x1b[1m"
#define D   "\x1b[2m"
#define R   "\x1b[31m"
#define G   "\x1b[32m"
#define Y   "\x1b[33m"
#define BL  "\x1b[34m"
#define M   "\x1b[35m"
#define C   "\x1b[36m"

static const char *bcol(int id) {
    switch (id) {
        case 1: return G; case 2: return Y; case 3: return M;
        case 4: return BL; case 5: return C; default: return G;
    }
}

typedef struct { int id; char ip[64]; int port; } be_info_t;
static be_info_t g_be[MAX_BE];
static int g_be_count = 0;

static int lb_ctl(const char *cmd, char *reply, size_t rsz) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LB_CTRL_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char line[256];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (send(fd, line, strlen(line), 0) < 0) {
        close(fd);
        return -1;
    }

    ssize_t n = recv(fd, reply, rsz - 1, 0);
    close(fd);
    if (n < 0) return -1;
    reply[n] = '\0';
    return 0;
}

static void load_config(void) {
    g_be_count = 0;
    char status[8192];
    if (lb_ctl("STATUS", status, sizeof(status)) == 0) {
        const char *p = status;
        while ((p = strstr(p, "\"id\":")) != NULL) {
            if (g_be_count >= MAX_BE) break;
            int id = atoi(p + 5);
            const char *ip_p = strstr(p, "\"ip\":\"");
            const char *port_p = strstr(p, "\"port\":");
            if (!ip_p || !port_p) break;
            ip_p += 6;
            const char *ip_end = strchr(ip_p, '"');
            if (!ip_end) break;
            int l = (int)(ip_end - ip_p);
            if (l <= 0 || l >= 63) break;
            strncpy(g_be[g_be_count].ip, ip_p, (size_t)l);
            g_be[g_be_count].ip[l] = '\0';
            g_be[g_be_count].port = atoi(port_p + 7);
            g_be[g_be_count].id = id;
            g_be_count++;
            p = ip_end;
        }
        if (g_be_count > 0) return;
    }

    lb_config_t cfg;
    const char *paths[] = {"config/config.json", "../config/config.json", NULL};
    for (int i = 0; paths[i]; i++) {
        if (config_load(paths[i], &cfg) == 0) {
            g_be_count = cfg.backend_count;
            for (int j = 0; j < g_be_count && j < MAX_BE; j++) {
                g_be[j].id = cfg.backends[j].id;
                g_be[j].port = cfg.backends[j].port;
                strncpy(g_be[j].ip, cfg.backends[j].ip, sizeof(g_be[j].ip) - 1);
                g_be[j].ip[sizeof(g_be[j].ip) - 1] = '\0';
            }
            return;
        }
    }
}

static int parse_host_port(const char *hp, char *ip, size_t ipsz, int *port) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", hp);
    char *colon = strrchr(buf, ':');
    if (!colon) return -1;
    *colon = '\0';
    snprintf(ip, ipsz, "%s", buf);
    *port = atoi(colon + 1);
    return (*port > 0 && *port <= 65535) ? 0 : -1;
}

static int do_request(const char *ip, int port, const char *msg,
                      char *reply, size_t rsz, double *ms) {
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (send(fd, msg, strlen(msg), 0) < 0) { close(fd); return -1; }

    ssize_t n = recv(fd, reply, rsz - 1, 0);
    if (n < 0) { close(fd); return -1; }
    reply[n] = '\0';
    if (n > 0 && reply[n - 1] == '\n') reply[n - 1] = '\0';

    gettimeofday(&t1, NULL);
    *ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
    close(fd);
    return 0;
}

static int parse_id(const char *r) {
    const char *p = strstr(r, "Backend ");
    if (p) return atoi(p + 8);
    p = strstr(r, "BACKEND_ID=");
    return p ? atoi(p + 11) : 0;
}

static void burst(const char *ip, int port, int n) {
    int tally[MAX_BE] = {0};
    double mss[MAX_BE] = {0};
    int fails = 0, max_id = 0;
    double total = 0;
    char reply[BUF_SIZE];

    printf("\n  " B C "[*]" RST " Sending " B "%d" RST " requests to " B "%s:%d" RST "\n\n", n, ip, port);
    for (int i = 1; i <= n; i++) {
        double ms;
        if (do_request(ip, port, "ping", reply, sizeof(reply), &ms) != 0) {
            printf("  " R "  [!] #%-3d  failed" RST "\n", i);
            fails++;
            fflush(stdout);
            continue;
        }
        int id = parse_id(reply);
        if (id >= 1 && id < MAX_BE) {
            tally[id]++;
            mss[id] += ms;
            if (id > max_id) max_id = id;
        }
        total += ms;
        printf("  " D "  #%-3d" RST "  %s" B "Backend %d" RST "  " D "%.1f ms" RST "\n",
               i, bcol(id), id, ms);
        fflush(stdout);
    }

    int disp = g_be_count > max_id ? g_be_count : max_id;
    int mt = 0;
    for (int i = 1; i <= disp; i++)
        if (tally[i] > mt) mt = tally[i];

    printf("\n" D "  ------------------------------------------------------------------\n" RST);
    printf("  " B C "[*]" RST " Distribution across " B "%d" RST " backends\n", disp);
    printf(D "  ------------------------------------------------------------------\n" RST);
    for (int i = 1; i <= disp; i++) {
        const char *col = bcol(i);
        if (!tally[i]) {
            printf("      %s" B "Backend %d" RST "  " R "no traffic" RST "\n", col, i);
            continue;
        }
        int pct = n > 0 ? (tally[i] * 100 + n / 2) / n : 0;
        int bar = mt > 0 ? tally[i] * 30 / mt : 0;
        double avg = tally[i] ? mss[i] / tally[i] : 0;
        printf("      %s" B "Backend %d" RST "  ", col, i);
        for (int j = 0; j < bar; j++) printf("%s\xe2\x96\x88" RST, col);
        for (int j = bar; j < 30; j++) printf(" ");
        printf("  %3d%%  %3d req  " D "%.1f ms" RST "\n", pct, tally[i], avg);
    }
    printf(D "  ------------------------------------------------------------------\n" RST);
    if (fails) printf("  " R "[!] Failed: %d" RST "\n", fails);
    int ok = n - fails;
    if (ok > 0) printf("  " D "[i] Total: %d  |  Avg: %.2f ms" RST "\n", ok, total / ok);
    printf("\n");
}

static int read_key(void) {
    struct termios o, n;
    tcgetattr(STDIN_FILENO, &o);
    n = o;
    n.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &n);
    int ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &o);
    return ch;
}

static void cls(void) { printf("\x1b[2J\x1b[H"); fflush(stdout); }

static void dashboard(const char *ip, int port) {
    cls();
    printf("\n  " B C "LoadForge Client Dashboard" RST "\n");
    printf("  " D "Target LB: %s:%d" RST "\n\n", ip, port);
    load_config();
    if (g_be_count > 0) {
        printf("  " B C "[*]" RST " Backends: " B "%d" RST "\n", g_be_count);
        printf(D "  ------------------------------------------------------------------\n" RST);
        for (int i = 0; i < g_be_count; i++)
            printf("      %s" B "%d" RST "  %s:%d\n",
                   bcol(g_be[i].id), g_be[i].id, g_be[i].ip, g_be[i].port);
        printf(D "  ------------------------------------------------------------------\n" RST "\n");
    }
    printf("  " B C "[*]" RST " Send requests\n");
    printf(D "  ------------------------------------------------------------------\n" RST);
    printf("  " B "[1]" RST " 1 req   " B "[5]" RST " 5 req   "
           B "[2]" RST " 20 req   " B "[3]" RST " 30 req   "
           B "[6]" RST " 60 req   " B "[9]" RST " 100 req\n");
    printf(D "  ------------------------------------------------------------------\n" RST);
    printf("  " B "[A]" RST " Custom target   " B "[+]" RST " Add backend   "
           B "[-]" RST " Remove backend\n");
    printf("  " D "[H] Menu   [C] Clear   [Q] Quit" RST "\n\n");
    printf("  " C "loadforge> " RST);
    fflush(stdout);
}

static void prompt_line(const char *label, char *out, size_t outsz) {
    printf("\n  " B "%s" RST ": ", label);
    fflush(stdout);
    if (!fgets(out, (int)outsz, stdin)) out[0] = '\0';
    size_t len = strlen(out);
    if (len && out[len - 1] == '\n') out[len - 1] = '\0';
}

static void interactive(const char *ip, int port) {
    char target_ip[64];
    int target_port = port;
    snprintf(target_ip, sizeof(target_ip), "%s", ip);

    dashboard(target_ip, target_port);
    int seq = 0;
    char reply[BUF_SIZE];

    for (;;) {
        int ch = read_key();
        printf("%c\n", (ch >= 32 && ch < 127) ? ch : ' ');
        if (ch == 'q' || ch == 'Q' || ch == EOF) {
            printf(D "\n  Done.\n\n" RST);
            break;
        }
        if (ch == 'h' || ch == 'H' || ch == 'c' || ch == 'C') {
            dashboard(target_ip, target_port);
            continue;
        }
        if (ch == 'a' || ch == 'A') {
            char hp[128];
            prompt_line("Enter ip:port", hp, sizeof(hp));
            if (parse_host_port(hp, target_ip, sizeof(target_ip), &target_port) == 0)
                printf("  " G "Target set to %s:%d" RST "\n", target_ip, target_port);
            else
                printf("  " R "Invalid format (use 127.0.0.1:8080)" RST "\n");
            printf("\n  " C "loadforge> " RST);
            fflush(stdout);
            continue;
        }
        if (ch == '+') {
            char hip[64], sim[16] = "";
            int p = 0;
            prompt_line("Backend IP", hip, sizeof(hip));
            char ps[16];
            prompt_line("Port", ps, sizeof(ps));
            p = atoi(ps);
            prompt_line("Sim mode (cpu|memory|io or empty)", sim, sizeof(sim));
            char cmd[256], resp[512];
            if (sim[0])
                snprintf(cmd, sizeof(cmd), "ADD %s %d %s", hip, p, sim);
            else
                snprintf(cmd, sizeof(cmd), "ADD %s %d", hip, p);
            if (lb_ctl(cmd, resp, sizeof(resp)) == 0)
                printf("  %s", resp);
            else
                printf("  " R "Control socket unavailable (is lb running?)" RST "\n");
            load_config();
            printf("\n  " C "loadforge> " RST);
            fflush(stdout);
            continue;
        }
        if (ch == '-') {
            char ids[16];
            prompt_line("Backend ID to remove", ids, sizeof(ids));
            char cmd[64], resp[512];
            snprintf(cmd, sizeof(cmd), "REMOVE %s", ids);
            if (lb_ctl(cmd, resp, sizeof(resp)) == 0)
                printf("  %s", resp);
            else
                printf("  " R "Control socket unavailable" RST "\n");
            load_config();
            printf("\n  " C "loadforge> " RST);
            fflush(stdout);
            continue;
        }
        if (ch == '1') {
            double ms;
            char msg[32];
            snprintf(msg, sizeof(msg), "r%d", ++seq);
            if (do_request(target_ip, target_port, msg, reply, sizeof(reply), &ms) != 0)
                printf("\n  " R "[!] Failed" RST "\n");
            else {
                int id = parse_id(reply);
                printf("\n  " G "[+]" RST " %s" B "Backend %d" RST "  " D "%.1f ms" RST "\n",
                       bcol(id), id, ms);
            }
            printf("\n  " C "loadforge> " RST);
            fflush(stdout);
            continue;
        }
        if (ch == '5') { burst(target_ip, target_port, 5); printf("  " C "loadforge> " RST); fflush(stdout); continue; }
        if (ch == '2') { burst(target_ip, target_port, 20); printf("  " C "loadforge> " RST); fflush(stdout); continue; }
        if (ch == '3') { burst(target_ip, target_port, 30); printf("  " C "loadforge> " RST); fflush(stdout); continue; }
        if (ch == '6') { burst(target_ip, target_port, 60); printf("  " C "loadforge> " RST); fflush(stdout); continue; }
        if (ch == '9') { burst(target_ip, target_port, 100); printf("  " C "loadforge> " RST); fflush(stdout); continue; }
        printf("  " D "(press H)" RST "\n  " C "loadforge> " RST);
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    char ip_buf[64] = "127.0.0.1";
    const char *ip = ip_buf;
    int port = 8080;
    int burst_n = -1;
    const char *one_shot = NULL;
    int pos = 1;

    while (pos < argc) {
        if (strcmp(argv[pos], "--lb") == 0 && pos + 1 < argc) {
            if (parse_host_port(argv[pos + 1], ip_buf, sizeof(ip_buf), &port) != 0) {
                fprintf(stderr, "Invalid --lb address (use ip:port)\n");
                return 1;
            }
            pos += 2;
        } else if (strcmp(argv[pos], "--burst") == 0 && pos + 1 < argc) {
            burst_n = atoi(argv[pos + 1]);
            pos += 2;
        } else if (argv[pos][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[pos]);
            return 1;
        } else {
            break;
        }
    }

    if (pos < argc) {
        snprintf(ip_buf, sizeof(ip_buf), "%s", argv[pos++]);
        ip = ip_buf;
        if (pos < argc && argv[pos][0] != '-')
            port = atoi(argv[pos++]);
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port.\nUsage: %s [--lb ip:port] [ip] [port] [--burst N] [msg]\n", argv[0]);
        return 1;
    }

    if (pos < argc && strcmp(argv[pos], "--burst") == 0 && pos + 1 < argc) {
        burst_n = atoi(argv[pos + 1]);
        pos += 2;
    }
    if (pos < argc)
        one_shot = argv[pos];

    load_config();

    if (burst_n > 0) {
        burst(ip, port, burst_n);
        return 0;
    }

    if (one_shot) {
        char reply[BUF_SIZE];
        double ms;
        if (do_request(ip, port, one_shot, reply, sizeof(reply), &ms) != 0) return 1;
        printf("%s\n", reply);
        return 0;
    }

    interactive(ip, port);
    return 0;
}

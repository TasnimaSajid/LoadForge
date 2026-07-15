/*
 * lf_ctl.c — LoadForge control CLI (Unix domain socket)
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int send_cmd(const char *cmd, char *reply, size_t rsz) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LB_CTRL_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    char line[512];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (send(fd, line, strlen(line), 0) < 0) {
        perror("send");
        close(fd);
        return -1;
    }

    ssize_t n = recv(fd, reply, rsz - 1, 0);
    close(fd);
    if (n < 0) { perror("recv"); return -1; }
    reply[n] = '\0';
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s status\n"
        "  %s add <ip> <port> [sim_mode]\n"
        "  %s remove <id>\n"
        "  %s set-algo <round_robin|adaptive|auto>\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    char cmd[512];
    const char *sub = argv[1];

    if (strcmp(sub, "status") == 0) {
        snprintf(cmd, sizeof(cmd), "STATUS");
    } else if (strcmp(sub, "add") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        if (argc >= 5)
            snprintf(cmd, sizeof(cmd), "ADD %s %s %s", argv[2], argv[3], argv[4]);
        else
            snprintf(cmd, sizeof(cmd), "ADD %s %s", argv[2], argv[3]);
    } else if (strcmp(sub, "remove") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        snprintf(cmd, sizeof(cmd), "REMOVE %s", argv[2]);
    } else if (strcmp(sub, "set-algo") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        snprintf(cmd, sizeof(cmd), "SET_ALGO %s", argv[2]);
    } else {
        usage(argv[0]);
        return 1;
    }

    char reply[8192];
    if (send_cmd(cmd, reply, sizeof(reply)) != 0)
        return 1;

    fputs(reply, stdout);
    if (strncmp(reply, "ERR", 3) == 0) return 1;
    return 0;
}

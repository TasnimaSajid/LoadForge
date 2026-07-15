/*
 * config.c — JSON configuration loader for LoadForge v3.
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <json-c/json.h>

static int valid_sim_mode(const char *mode)
{
    if (!mode || mode[0] == '\0') return 1;
    return strcmp(mode, "cpu") == 0
        || strcmp(mode, "memory") == 0
        || strcmp(mode, "io") == 0;
}

void backend_init_runtime(backend_t *b)
{
    b->is_healthy        = 1;
    b->draining          = 0;
    b->ema_network_ms    = 0.0;
    b->ema_processing_ms = 0.0;
    b->total_connections = 0;
    b->last_ema_update_ms = 0.0;
    b->cb_state          = CB_CLOSED;
    b->fail_count        = 0;
    b->open_since        = 0;
    b->pid               = -1;
}

int config_parse_backend_entry(struct json_object *b, backend_t *be)
{
    struct json_object *tmp;

    memset(be, 0, sizeof(*be));
    be->id = json_object_object_get_ex(b, "id", &tmp)
           ? json_object_get_int(tmp) : 0;

    if (!json_object_object_get_ex(b, "ip", &tmp)) return -1;
    snprintf(be->ip, MAX_IP_LEN, "%s", json_object_get_string(tmp));

    if (!json_object_object_get_ex(b, "port", &tmp)) return -1;
    be->port = json_object_get_int(tmp);

    be->weight = json_object_object_get_ex(b, "weight", &tmp)
               ? json_object_get_int(tmp) : 1;
    if (be->weight < 1) be->weight = 1;

    be->delay_ms = json_object_object_get_ex(b, "delay_ms", &tmp)
                 ? json_object_get_int(tmp) : 0;
    if (be->delay_ms < 0) be->delay_ms = 0;
    if (be->delay_ms > 30000) be->delay_ms = 30000;

    be->sim_mode[0] = '\0';
    if (json_object_object_get_ex(b, "sim_mode", &tmp)) {
        const char *sm = json_object_get_string(tmp);
        if (!valid_sim_mode(sm)) {
            fprintf(stderr, "config: invalid sim_mode '%s' (use cpu|memory|io)\n", sm);
            return -1;
        }
        snprintf(be->sim_mode, sizeof(be->sim_mode), "%s", sm);
    }

    backend_init_runtime(be);
    return 0;
}

int config_load(const char *path, lb_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->algorithm = ALGO_ROUND_ROBIN;

    struct json_object *root = json_object_from_file(path);
    if (!root) {
        fprintf(stderr, "config: cannot open/parse '%s'\n", path);
        return -1;
    }

    int rc = -1;
    struct json_object *j;

    if (!json_object_object_get_ex(root, "listen_ip", &j)) {
        fprintf(stderr, "config: missing listen_ip\n");
        goto done;
    }
    snprintf(cfg->listen_ip, sizeof(cfg->listen_ip), "%s", json_object_get_string(j));

    if (!json_object_object_get_ex(root, "listen_port", &j)) {
        fprintf(stderr, "config: missing listen_port\n");
        goto done;
    }
    cfg->listen_port = json_object_get_int(j);

    if (json_object_object_get_ex(root, "algorithm", &j)) {
        const char *alg = json_object_get_string(j);
        if (strcmp(alg, "adaptive") == 0)
            cfg->algorithm = ALGO_ADAPTIVE;
        else if (strcmp(alg, "round_robin") == 0)
            cfg->algorithm = ALGO_ROUND_ROBIN;
        else
            cfg->algorithm = ALGO_ROUND_ROBIN;
    } else {
        cfg->algorithm = ALGO_ROUND_ROBIN;
    }

    if (json_object_object_get_ex(root, "backend_bin", &j))
        snprintf(cfg->backend_bin, sizeof(cfg->backend_bin), "%s", json_object_get_string(j));
    else
        snprintf(cfg->backend_bin, sizeof(cfg->backend_bin), "bin/backend");

    struct json_object *jbe;
    if (!json_object_object_get_ex(root, "backends", &jbe) ||
        !json_object_is_type(jbe, json_type_array)) {
        fprintf(stderr, "config: missing/non-array 'backends'\n");
        goto done;
    }

    int n = (int)json_object_array_length(jbe);
    if (n <= 0 || n > MAX_BACKENDS) {
        fprintf(stderr, "config: bad backend count %d\n", n);
        goto done;
    }

    for (int i = 0; i < n; i++) {
        struct json_object *b = json_object_array_get_idx(jbe, i);
        if (config_parse_backend_entry(b, &cfg->backends[i]) != 0) {
            fprintf(stderr, "config: bad backend entry %d\n", i);
            goto done;
        }
        if (cfg->backends[i].id == 0)
            cfg->backends[i].id = i + 1;
    }
    cfg->backend_count = n;
    rc = 0;

done:
    json_object_put(root);
    return rc;
}

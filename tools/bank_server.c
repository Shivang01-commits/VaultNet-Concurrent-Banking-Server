/*
 * bank_server - starts the banking server.
 *
 *   ./bin/bank_server [--port 9090] [--data ./data] [--loops 2] [--workers 8]
 *                     [--idle 300] [--worker-timeout 10] [--max-clients N] [-v]
 *
 * First start creates the user  admin / admin123  (change the password!).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

static void usage(void)
{
    fprintf(stderr,
        "usage: bank_server [--port N] [--data DIR] [--loops N] [--workers N]\n"
        "                   [--idle SECS] [--worker-timeout SECS] [--max-clients N] [-v]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    ServerConfig cfg = { .port = 9090, .datadir = "./data", .nloops = 2, .nworkers = 8,
                         .idle_secs = 300, .worker_timeout_secs = 10 };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) { cfg.verbose = 1; continue; }
        if (i + 1 >= argc) usage();
        const char *v = argv[++i];
        if      (!strcmp(a, "--port"))           cfg.port = atoi(v);
        else if (!strcmp(a, "--data"))           cfg.datadir = v;
        else if (!strcmp(a, "--loops"))          cfg.nloops = atoi(v);
        else if (!strcmp(a, "--workers"))        cfg.nworkers = atoi(v);
        else if (!strcmp(a, "--idle"))           cfg.idle_secs = atoi(v);
        else if (!strcmp(a, "--worker-timeout")) cfg.worker_timeout_secs = atoi(v);
        else if (!strcmp(a, "--max-clients"))    cfg.max_clients_per_loop = atoi(v);
        else usage();
    }
    if (cfg.port <= 0 || cfg.nloops < 1 || cfg.nworkers < 1 || cfg.worker_timeout_secs < 1) usage();
    supervisor_main(&cfg);
    return 0;
}

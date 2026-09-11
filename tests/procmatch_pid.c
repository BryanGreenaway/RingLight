/*
 * Report whether a live process matches a watched name, using the same
 * /proc reading and matching the monitor uses.
 *
 *   procmatch_pid <pid> <name>
 *
 * Exits 0 on a match, 1 on no match, 2 when the process could not be read.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/procmatch.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <pid> <name>\n", argv[0]);
        return 2;
    }

    struct proc_snapshot snap;
    if (!procmatch_snapshot((pid_t)atoi(argv[1]), &snap)) {
        fprintf(stderr, "could not read /proc for pid %s\n", argv[1]);
        return 2;
    }

    bool matched = proc_matches_name(&snap.info, argv[2]);
    printf("%-9s pid=%s comm=%s exe=%s argc=%d\n",
           matched ? "MATCH" : "NO-MATCH", argv[1],
           snap.info.comm ? snap.info.comm : "?",
           snap.info.exe ? snap.info.exe : "?",
           snap.info.argc);
    return matched ? 0 : 1;
}

/*
 * RingLight - process identity matching
 *
 * Decides whether a process *is* a watched program, as opposed to merely
 * mentioning its name somewhere in its arguments.
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RINGLIGHT_PROCMATCH_H
#define RINGLIGHT_PROCMATCH_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define PROCMATCH_MAX_ARGV 64

/* Identifying details of a process. Any field may be NULL or empty when /proc
 * did not provide it. */
struct proc_info {
    const char *comm;                    /* /proc/PID/comm (truncated at 15 chars) */
    const char *exe;                     /* readlink of /proc/PID/exe */
    const char *argv[PROCMATCH_MAX_ARGV];
    int argc;
};

/* A proc_info together with the storage its fields point at. */
struct proc_snapshot {
    struct proc_info info;
    char comm[256];
    char exe[PATH_MAX];
    char cmdline[4096];
};

/* Split a raw /proc/PID/cmdline buffer (NUL-separated) into argv pointers.
 * buf must stay alive for as long as info is used. */
void procmatch_parse_cmdline(struct proc_info *info, const char *buf, size_t len);

/* Read a process's identity out of /proc. False when nothing could be read,
 * which is the normal outcome for a process that has already exited. */
bool procmatch_snapshot(pid_t pid, struct proc_snapshot *snap);

/* True when the process is an instance of `name`. */
bool proc_matches_name(const struct proc_info *info, const char *name);

#endif

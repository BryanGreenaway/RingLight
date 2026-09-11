/*
 * RingLight - process identity matching
 *
 * A watched name has to identify the program being run, not just appear
 * somewhere in its arguments. Package managers name their packages on the
 * command line, so a plain substring search over /proc/PID/cmdline treats
 * `pacman -Qs howdy` as a howdy run and flashes the ring light during a
 * system update.
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "procmatch.h"

/* Interpreters run their program as an argument, so for these the script path
 * has to be inspected as well as argv[0]. Versioned names (python3,
 * python3.13) are accepted. */
static const char *const interpreters[] = {
    "sh", "bash", "dash", "zsh", "ksh", "fish",
    "python", "perl", "ruby", "node", "lua", "php", NULL
};

/* Extensions a script may carry, so /usr/lib/howdy/howdy.py matches "howdy". */
static const char *const script_exts[] = {
    ".py", ".sh", ".pl", ".rb", ".js", ".lua", ".php", NULL
};

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool component_equals(const char *comp, size_t len, const char *name) {
    size_t nlen = strlen(name);
    return len == nlen && strncasecmp(comp, name, nlen) == 0;
}

/* Like component_equals, but also accepts a script extension: the basename
 * "howdy.py" identifies "howdy", while "howdy-git" and "howdy.tar.gz" do not. */
static bool basename_identifies(const char *bname, const char *name) {
    size_t nlen = strlen(name);
    if (component_equals(bname, strlen(bname), name)) return true;
    if (strncasecmp(bname, name, nlen) != 0) return false;
    for (int i = 0; script_exts[i]; i++)
        if (strcasecmp(bname + nlen, script_exts[i]) == 0) return true;
    return false;
}

/* True when `name` is one of the directory components of `path`, or names the
 * file itself. Only meaningful for arguments that really are paths. */
static bool path_identifies(const char *path, const char *name) {
    const char *p = path;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        if (p > start) {
            bool last = (*p == '\0');
            if (last ? basename_identifies(start, name)
                     : component_equals(start, (size_t)(p - start), name))
                return true;
        }
        while (*p == '/') p++;
    }
    return false;
}

static bool is_interpreter(const char *path) {
    if (!path || !*path) return false;
    const char *bname = base_name(path);

    for (int i = 0; interpreters[i]; i++) {
        size_t len = strlen(interpreters[i]);
        if (strncasecmp(bname, interpreters[i], len) != 0) continue;

        /* Accept an exact name, or a version suffix such as python3.13. */
        const char *rest = bname + len;
        if (!*rest) return true;
        bool versioned = true;
        for (const char *r = rest; *r; r++) {
            if (!isdigit((unsigned char)*r) && *r != '.') { versioned = false; break; }
        }
        if (versioned) return true;
    }
    return false;
}

/* The program an interpreter was asked to run: the first argument that is not
 * an option and looks like a path. A bare word such as the "howdy" in
 * `pacman -Qs howdy` is deliberately not a candidate. */
static const char *script_argument(const struct proc_info *info) {
    for (int i = 1; i < info->argc; i++) {
        const char *arg = info->argv[i];
        if (!arg || !*arg) continue;
        if (arg[0] == '-') continue;
        if (!strchr(arg, '/')) continue;
        return arg;
    }
    return NULL;
}

void procmatch_parse_cmdline(struct proc_info *info, const char *buf, size_t len) {
    info->argc = 0;
    if (!buf || len == 0) return;

    size_t i = 0;
    while (i < len && info->argc < PROCMATCH_MAX_ARGV) {
        info->argv[info->argc++] = buf + i;
        while (i < len && buf[i] != '\0') i++;
        i++; /* step over the separator */
    }
}

static bool read_proc_file(pid_t pid, const char *what, char *buf, size_t len,
                           ssize_t *out_len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/%s", pid, what);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return false;
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return true;
}

bool procmatch_snapshot(pid_t pid, struct proc_snapshot *snap) {
    memset(snap, 0, sizeof(*snap));

    bool got_comm = read_proc_file(pid, "comm", snap->comm, sizeof(snap->comm), NULL);
    if (got_comm) {
        char *nl = strchr(snap->comm, '\n');
        if (nl) *nl = '\0';
    }

    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/exe", pid);
    ssize_t elen = readlink(link, snap->exe, sizeof(snap->exe) - 1);
    bool got_exe = elen >= 0;
    if (got_exe) snap->exe[elen] = '\0';

    /* The command line keeps its NUL separators so arguments stay distinct. */
    ssize_t clen = 0;
    bool got_cmdline = read_proc_file(pid, "cmdline", snap->cmdline,
                                      sizeof(snap->cmdline), &clen);

    if (!got_comm && !got_exe && clen <= 0) return false;

    snap->info.comm = got_comm ? snap->comm : NULL;
    snap->info.exe = got_exe ? snap->exe : NULL;
    if (got_cmdline && clen > 0)
        procmatch_parse_cmdline(&snap->info, snap->cmdline, (size_t)clen);

    return true;
}

bool proc_matches_name(const struct proc_info *info, const char *name) {
    if (!info || !name || !*name) return false;

    /* The kernel's name for the running executable. */
    if (info->comm && *info->comm && strcasecmp(info->comm, name) == 0)
        return true;

    /* comm is truncated at 15 characters, so check the executable itself. */
    if (info->exe && *info->exe && basename_identifies(base_name(info->exe), name))
        return true;

    /* How the program was invoked. */
    const char *argv0 = info->argc > 0 ? info->argv[0] : NULL;
    if (argv0 && *argv0 && basename_identifies(base_name(argv0), name))
        return true;

    /* Interpreted programs: `python /usr/lib/howdy/compare.py` is howdy. */
    if (is_interpreter(info->comm) || is_interpreter(info->exe) || is_interpreter(argv0)) {
        const char *script = script_argument(info);
        if (script && path_identifies(script, name)) return true;
    }

    return false;
}

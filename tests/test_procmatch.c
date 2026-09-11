/*
 * Tests for process identity matching.
 *
 * The cases below are taken from real processes on an Arch/CachyOS system
 * running howdy-git: the ones that must match are how howdy actually starts,
 * the ones that must not are what a system update run looks like.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../src/procmatch.h"

static int failures = 0;
static int checks = 0;

static void check(const char *what, const struct proc_info *info,
                  const char *name, bool want) {
    bool got = proc_matches_name(info, name);
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL: %s\n      watching \"%s\": expected %s, got %s\n",
               what, name, want ? "match" : "no match", got ? "match" : "no match");
    }
}

/* Build a proc_info from a NULL-terminated argument list. */
#define PROC(var, comm_, exe_, ...)                                     \
    struct proc_info var = { .comm = comm_, .exe = exe_ };              \
    do {                                                                \
        const char *a[] = { __VA_ARGS__ };                              \
        var.argc = (int)(sizeof(a) / sizeof(a[0]));                     \
        for (int i = 0; i < var.argc; i++) var.argv[i] = a[i];          \
    } while (0)

static void test_real_howdy_processes_match(void) {
    /* What PAM runs during face authentication. This is the one that must
     * light the ring. */
    PROC(pam, "python", "/usr/bin/python",
         "/usr/bin/python", "/usr/lib/howdy/compare.py", "bryan");
    check("PAM face auth", &pam, "howdy", true);

    /* python invoked with flags before the script. */
    PROC(unbuffered, "python", "/usr/bin/python",
         "/usr/bin/python", "-u", "/usr/lib/howdy/compare.py", "bryan");
    check("PAM face auth, python -u", &unbuffered, "howdy", true);

    /* /usr/bin/howdy is a /bin/sh wrapper, so the kernel reports the shell. */
    PROC(wrapper, "sh", "/usr/bin/dash",
         "/bin/sh", "/usr/bin/howdy", "test");
    check("howdy CLI shell wrapper", &wrapper, "howdy", true);

    /* ...which then execs the python CLI. */
    PROC(cli, "python", "/usr/bin/python",
         "/usr/bin/python", "/usr/lib/howdy/cli.py", "list");
    check("howdy CLI", &cli, "howdy", true);

    /* A watched program that is an ordinary binary. */
    PROC(binary, "zoom", "/usr/bin/zoom", "/usr/bin/zoom");
    check("plain binary", &binary, "zoom", true);

    /* comm is truncated to 15 characters; exe is not. */
    PROC(truncated, "some-very-long-", "/usr/bin/some-very-long-name",
         "/usr/bin/some-very-long-name");
    check("name longer than comm allows", &truncated, "some-very-long-name", true);
}

static void test_update_check_does_not_match(void) {
    /* These are the processes that a CachyOS/Arch update run spawns while it
     * checks the howdy-git package. None of them is howdy. */

    PROC(pacman, "pacman", "/usr/bin/pacman",
         "/usr/bin/pacman", "-Qs", "howdy");
    check("pacman searching for the package", &pacman, "howdy", false);

    PROC(paru, "paru", "/usr/bin/paru",
         "/usr/bin/paru", "-S", "howdy-git");
    check("paru installing the package", &paru, "howdy", false);

    PROC(lsremote, "git", "/usr/bin/git",
         "/usr/bin/git", "ls-remote", "https://github.com/boltgolt/howdy");
    check("paru checking the devel package upstream", &lsremote, "howdy", false);

    PROC(shellcmd, "bash", "/usr/bin/bash",
         "/bin/bash", "-c", "paru -S howdy-git");
    check("shell command mentioning the package", &shellcmd, "howdy", false);

    PROC(gitdir, "bash", "/usr/bin/bash",
         "/bin/bash", "-c", "git -C /home/bryan/.cache/paru/clone/howdy-git pull");
    check("shell command in the package build directory", &gitdir, "howdy", false);

    PROC(grepping, "grep", "/usr/bin/grep",
         "/usr/bin/grep", "howdy", "/var/log/pacman.log");
    check("grep for the package name", &grepping, "howdy", false);

    PROC(makepkg, "bash", "/usr/bin/bash",
         "/bin/bash", "/usr/bin/makepkg", "-si");
    check("makepkg building the package", &makepkg, "howdy", false);

    /* Even an editor with the name in a filename is not howdy. */
    PROC(editor, "nvim", "/usr/bin/nvim",
         "/usr/bin/nvim", "PKGBUILD-howdy.txt");
    check("editor opening a file named after it", &editor, "howdy", false);
}

static void test_cmdline_parsing(void) {
    static const char raw[] = "/usr/bin/python\0/usr/lib/howdy/compare.py\0bryan\0";
    struct proc_info info = { .comm = "python", .exe = "/usr/bin/python" };
    procmatch_parse_cmdline(&info, raw, sizeof(raw) - 1);

    checks++;
    if (info.argc != 3) {
        failures++;
        printf("FAIL: cmdline parsing\n      expected 3 arguments, got %d\n", info.argc);
        return;
    }
    checks++;
    if (strcmp(info.argv[1], "/usr/lib/howdy/compare.py") != 0) {
        failures++;
        printf("FAIL: cmdline parsing\n      argv[1] was \"%s\"\n", info.argv[1]);
    }
}

static void test_empty_and_missing_fields(void) {
    struct proc_info empty = { 0 };
    check("process with no readable details", &empty, "howdy", false);

    PROC(no_exe, "howdy", NULL, "howdy");
    check("process with no exe link", &no_exe, "howdy", true);
}

int main(void) {
    test_real_howdy_processes_match();
    test_update_check_does_not_match();
    test_cmdline_parsing();
    test_empty_and_missing_fields();

    if (failures == 0) {
        printf("ok - %d checks passed\n", checks);
        return 0;
    }
    printf("\n%d of %d checks failed\n", failures, checks);
    return 1;
}

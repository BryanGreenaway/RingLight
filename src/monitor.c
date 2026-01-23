/*
 * RingLight Monitor - Event-driven webcam activity detector
 *
 * Watches for specific processes (like howdy) via netlink proc connector,
 * or polls for camera activity via V4L2, and launches the overlay.
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <pwd.h>
#include <poll.h>

#define MAX_ITEMS 16

enum monitor_mode { MODE_PROCESS, MODE_CAMERA, MODE_HYBRID };

static volatile sig_atomic_t running = 1;
static pid_t overlay_pids[MAX_ITEMS];
static int overlay_count = 0;
static bool verbose = false;

/* Configuration */
static enum monitor_mode mode = MODE_PROCESS;
static char video_dev[256] = "/dev/video0";
static char color[32] = "FFFFFF";
static int brightness = 100, width = 80;
static int poll_interval_ms = 2000;
static bool fullscreen = false;
static char *watch_procs[MAX_ITEMS];
static int watch_proc_count = 0;
static char *screens[MAX_ITEMS];
static int screen_count = 0;

/* Runtime state */
static int nl_sock = -1;
static pid_t watched_pids[MAX_ITEMS];
static int watched_pid_count = 0;
static bool overlay_active = false;

static void sig_handler(int s) { (void)s; running = 0; }

#define log_info(...) do { if (verbose) fprintf(stderr, "[ringlight] " __VA_ARGS__); } while(0)
#define log_err(...) fprintf(stderr, "[ringlight] " __VA_ARGS__)

static char *trim(char *s) {
    while (*s && isspace(*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace(*e)) *e-- = '\0';
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') { s[len-1] = '\0'; s++; }
    return s;
}

static void parse_list(const char *str, char **arr, int *cnt, int max) {
    if (!str || !*str) return;
    char *copy = strdup(str), *tok = strtok(copy, ",");
    while (tok && *cnt < max) {
        tok = trim(tok);
        if (*tok) arr[(*cnt)++] = strdup(tok);
        tok = strtok(NULL, ",");
    }
    free(copy);
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) { struct passwd *pw = getpwuid(getuid()); if (pw) home = pw->pw_dir; }
    if (!home) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/ringlight/config.ini", home);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);

        if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "process") == 0) mode = MODE_PROCESS;
            else if (strcmp(val, "camera") == 0) mode = MODE_CAMERA;
            else if (strcmp(val, "hybrid") == 0) mode = MODE_HYBRID;
        }
        else if (strcmp(key, "color") == 0) strncpy(color, val[0] == '#' ? val+1 : val, sizeof(color)-1);
        else if (strcmp(key, "brightness") == 0) { brightness = atoi(val); if (brightness < 1) brightness = 1; if (brightness > 100) brightness = 100; }
        else if (strcmp(key, "width") == 0) { width = atoi(val); if (width < 10) width = 10; if (width > 500) width = 500; }
        else if (strcmp(key, "fullscreen") == 0) fullscreen = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "video_device") == 0) strncpy(video_dev, val, sizeof(video_dev)-1);
        else if (strcmp(key, "poll_interval") == 0) { poll_interval_ms = atoi(val); if (poll_interval_ms < 100) poll_interval_ms = 100; }
        else if (strcmp(key, "screens") == 0) parse_list(val, screens, &screen_count, MAX_ITEMS);
        else if (strcmp(key, "watch_processes") == 0) parse_list(val, watch_procs, &watch_proc_count, MAX_ITEMS);
    }
    fclose(f);
}

static bool get_proc_comm(pid_t pid, char *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(buf, len, f)) { fclose(f); return false; }
    fclose(f);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return true;
}

static bool get_proc_cmdline(pid_t pid, char *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    for (ssize_t i = 0; i < n - 1; i++) if (buf[i] == '\0') buf[i] = ' ';
    return true;
}

static bool matches_watch_list(pid_t pid) {
    char comm[256] = {0}, cmdline[1024] = {0};
    bool got_comm = get_proc_comm(pid, comm, sizeof(comm));
    bool got_cmdline = get_proc_cmdline(pid, cmdline, sizeof(cmdline));

    if (!got_comm && !got_cmdline) return false;

    for (int i = 0; i < watch_proc_count; i++) {
        if (got_comm && strcasecmp(comm, watch_procs[i]) == 0) return true;
        if (got_cmdline && strcasestr(cmdline, watch_procs[i])) return true;
    }
    return false;
}

static void add_watched_pid(pid_t pid) {
    if (watched_pid_count >= MAX_ITEMS) return;
    for (int i = 0; i < watched_pid_count; i++) if (watched_pids[i] == pid) return;
    watched_pids[watched_pid_count++] = pid;
    log_info("Tracking pid %d\n", pid);
}

static void remove_watched_pid(pid_t pid) {
    for (int i = 0; i < watched_pid_count; i++) {
        if (watched_pids[i] == pid) {
            watched_pids[i] = watched_pids[--watched_pid_count];
            log_info("Untracked pid %d\n", pid);
            return;
        }
    }
}

static int setup_netlink(void) {
    nl_sock = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if (nl_sock < 0) {
        log_err("Netlink socket failed (need CAP_NET_ADMIN): %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = CN_IDX_PROC, .nl_pid = getpid() };
    if (bind(nl_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_err("Netlink bind failed: %s\n", strerror(errno));
        close(nl_sock); nl_sock = -1;
        return -1;
    }

    struct {
        struct nlmsghdr nl;
        struct cn_msg cn;
        enum proc_cn_mcast_op op;
    } msg = {
        .nl = { .nlmsg_len = sizeof(msg), .nlmsg_type = NLMSG_DONE, .nlmsg_pid = getpid() },
        .cn = { .id = { .idx = CN_IDX_PROC, .val = CN_VAL_PROC }, .len = sizeof(enum proc_cn_mcast_op) },
        .op = PROC_CN_MCAST_LISTEN
    };

    if (send(nl_sock, &msg, sizeof(msg), 0) < 0) {
        log_err("Netlink subscribe failed: %s\n", strerror(errno));
        close(nl_sock); nl_sock = -1;
        return -1;
    }

    return 0;
}

static bool v4l2_streaming(void) {
    int fd = open(video_dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    int ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    int err = errno;
    close(fd);
    return (ret < 0 && err == EBUSY);
}

static void start_overlay(void) {
    if (overlay_active) return;
    log_info("Starting overlay\n");

    char bstr[16], wstr[16], sstr[16];
    snprintf(bstr, sizeof(bstr), "%d", brightness);
    snprintf(wstr, sizeof(wstr), "%d", width);

    int num = screen_count > 0 ? screen_count : 1;
    overlay_count = 0;

    for (int i = 0; i < num && overlay_count < MAX_ITEMS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            char *args[16];
            int n = 0;
            args[n++] = (char*)"ringlight-overlay";
            args[n++] = (char*)"-c"; args[n++] = color;
            args[n++] = (char*)"-b"; args[n++] = bstr;
            args[n++] = (char*)"-w"; args[n++] = wstr;
            if (fullscreen) args[n++] = (char*)"-f";
            if (screen_count > 0) {
                snprintf(sstr, sizeof(sstr), "%s", screens[i]);
                args[n++] = (char*)"-s";
                args[n++] = sstr;
            }
            args[n] = NULL;
            execvp("ringlight-overlay", args);
            _exit(1);
        } else if (pid > 0) {
            overlay_pids[overlay_count++] = pid;
        }
    }
    overlay_active = (overlay_count > 0);
}

static void stop_overlay(void) {
    if (!overlay_active) return;
    log_info("Stopping overlay\n");
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_pids[i] > 0) {
            kill(overlay_pids[i], SIGTERM);
            waitpid(overlay_pids[i], NULL, 0);
        }
    }
    overlay_count = 0;
    overlay_active = false;
}

static void cleanup(void) {
    stop_overlay();
    if (nl_sock >= 0) close(nl_sock);
    for (int i = 0; i < watch_proc_count; i++) free(watch_procs[i]);
    for (int i = 0; i < screen_count; i++) free(screens[i]);
}

static void verify_watched_pids(void) {
    int i = 0;
    while (i < watched_pid_count) {
        if (kill(watched_pids[i], 0) != 0) {
            log_info("Process exited: pid %d\n", watched_pids[i]);
            watched_pids[i] = watched_pids[--watched_pid_count];
        } else {
            i++;
        }
    }
}

static void process_netlink_event(char *buf) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct cn_msg *cn = NLMSG_DATA(nlh);
    struct proc_event *ev = (struct proc_event *)cn->data;

    if (ev->what == PROC_EVENT_EXEC) {
        pid_t pid = ev->event_data.exec.process_pid;
        if (matches_watch_list(pid)) {
            log_info("Matched process: pid %d\n", pid);
            add_watched_pid(pid);
            if (!overlay_active) start_overlay();
        }
    } else if (ev->what == PROC_EVENT_EXIT) {
        pid_t pid = ev->event_data.exit.process_pid;
        if (watched_pid_count > 0) {
            remove_watched_pid(pid);
            if (watched_pid_count == 0 && overlay_active) {
                log_info("All watched processes exited\n");
                stop_overlay();
            }
        }
    }
}

static void run_process_mode(void) {
    log_info("Process mode: watching %d process(es)\n", watch_proc_count);

    char buf[8192];
    while (running) {
        struct pollfd pfd = { .fd = nl_sock, .events = POLLIN };
        int ret = poll(&pfd, 1, overlay_active ? 500 : -1);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t len = recv(nl_sock, buf, sizeof(buf), MSG_DONTWAIT);
            if (len > 0) process_netlink_event(buf);
        }

        if (overlay_active && watched_pid_count > 0) {
            verify_watched_pids();
            if (watched_pid_count == 0) stop_overlay();
        }
    }
}

static void run_camera_mode(void) {
    log_info("Camera mode: polling %s every %dms\n", video_dev, poll_interval_ms);
    while (running) {
        bool active = v4l2_streaming();
        if (active && !overlay_active) { log_info("Camera active\n"); start_overlay(); }
        else if (!active && overlay_active) { log_info("Camera inactive\n"); stop_overlay(); }
        usleep(poll_interval_ms * 1000);
    }
}

static void run_hybrid_mode(void) {
    log_info("Hybrid mode: process + camera poll %dms\n", poll_interval_ms);

    char buf[4096];
    while (running) {
        struct pollfd pfd = { .fd = nl_sock, .events = POLLIN };
        int ret = poll(&pfd, 1, poll_interval_ms);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t len = recv(nl_sock, buf, sizeof(buf), MSG_DONTWAIT);
            if (len > 0) process_netlink_event(buf);
        }

        if (watched_pid_count == 0) {
            bool cam = v4l2_streaming();
            if (cam && !overlay_active) { log_info("Camera active\n"); start_overlay(); }
            else if (!cam && overlay_active) { log_info("Camera inactive\n"); stop_overlay(); }
        }
    }
}

static void usage(const char *prog) {
    printf("Usage: %s [options]\n\n"
           "Modes:\n"
           "  -m, --mode MODE      process|camera|hybrid (default: process)\n"
           "                       process: netlink-based, requires CAP_NET_ADMIN\n"
           "                       camera: poll for any camera activity\n"
           "                       hybrid: both methods combined\n\n"
           "Options:\n"
           "  -d, --device PATH    Video device (default: /dev/video0)\n"
           "  -p, --proc NAME      Process to watch, repeatable (default: howdy)\n"
           "  -i, --interval MS    Poll interval for camera mode (default: 2000)\n"
           "  -v, --verbose        Verbose output\n"
           "  -h, --help           Show help\n", prog);
}

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"device", required_argument, 0, 'd'},
        {"proc", required_argument, 0, 'p'},
        {"interval", required_argument, 0, 'i'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "m:d:p:i:vh", opts, NULL)) != -1) {
        switch (c) {
            case 'm':
                if (strcmp(optarg, "process") == 0) mode = MODE_PROCESS;
                else if (strcmp(optarg, "camera") == 0) mode = MODE_CAMERA;
                else if (strcmp(optarg, "hybrid") == 0) mode = MODE_HYBRID;
                break;
            case 'd': strncpy(video_dev, optarg, sizeof(video_dev)-1); break;
            case 'p': if (watch_proc_count < MAX_ITEMS) watch_procs[watch_proc_count++] = strdup(optarg); break;
            case 'i': poll_interval_ms = atoi(optarg); if (poll_interval_ms < 100) poll_interval_ms = 100; break;
            case 'v': verbose = true; break;
            case 'h': usage(argv[0]); return 0;
        }
    }

    load_config();
    if (watch_proc_count == 0) watch_procs[watch_proc_count++] = strdup("howdy");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);
    atexit(cleanup);

    if (mode != MODE_CAMERA) {
        if (setup_netlink() < 0) {
            if (mode == MODE_PROCESS) {
                log_err("Process mode requires CAP_NET_ADMIN capability.\n");
                log_err("Run: sudo setcap cap_net_admin+ep %s\n", argv[0]);
                return 1;
            }
            log_err("Netlink failed, falling back to camera mode\n");
            mode = MODE_CAMERA;
        }
    }

    switch (mode) {
        case MODE_PROCESS: run_process_mode(); break;
        case MODE_CAMERA:  run_camera_mode(); break;
        case MODE_HYBRID:  run_hybrid_mode(); break;
    }

    return 0;
}

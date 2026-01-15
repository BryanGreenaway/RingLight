/*
 * RingLight Monitor - Event-driven webcam activity detector
 * 
 * Architecture:
 * - Netlink proc connector: Instant notification when watched processes start/exit
 * - V4L2 polling: ONLY as fallback, every few seconds, for apps not in watch list
 * - No polling when overlay is already running (netlink handles exit detection)
 * 
 * License: MIT
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
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <pwd.h>

#define MAX_ITEMS 16
#define V4L2_POLL_INTERVAL 3  /* Seconds between V4L2 checks when idle */

static volatile sig_atomic_t running = 1;
static pid_t overlay_pids[MAX_ITEMS];
static int overlay_count = 0;
static bool verbose = false;

static char video_dev[256] = "/dev/video0";
static char video_dev_real[PATH_MAX] = "";
static char color[32] = "FFFFFF";
static int brightness = 100, width = 80;
static bool fullscreen = false;
static char *procs[MAX_ITEMS], *screens[MAX_ITEMS];
static int proc_count = 0, screen_count = 0;

/* State tracking */
static int nl_sock = -1;
static int watched_proc_count = 0;  /* How many watched processes are running */
static time_t last_v4l2_check = 0;
static bool overlay_active = false;

static void sig_handler(int s) { (void)s; running = 0; }
#define log_msg(...) do { if (verbose) { fprintf(stderr, "[monitor] " __VA_ARGS__); fflush(stderr); } } while(0)
#define log_err(...) do { fprintf(stderr, "[monitor] " __VA_ARGS__); fflush(stderr); } while(0)

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t')) *e-- = '\0';
    if (strlen(s) >= 2 && s[0] == '"' && s[strlen(s)-1] == '"') { s[strlen(s)-1] = '\0'; s++; }
    return s;
}

static char *get_config(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    static char val[256];
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq || (size_t)(eq - line) != klen || strncmp(line, key, klen) != 0) continue;
        strncpy(val, trim(eq + 1), sizeof(val) - 1);
        fclose(f);
        return val;
    }
    fclose(f);
    return NULL;
}

static void parse_list(const char *str, char **arr, int *cnt, int max) {
    if (!str || !*str) return;
    char *copy = strdup(str), *tok = strtok(copy, ",");
    while (tok && *cnt < max) { tok = trim(tok); if (*tok) arr[(*cnt)++] = strdup(tok); tok = strtok(NULL, ","); }
    free(copy);
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) { struct passwd *pw = getpwuid(getuid()); if (pw) home = pw->pw_dir; }
    if (!home) return;
    
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/ringlight/config.ini", home);
    
    char *v;
    if ((v = get_config(path, "color"))) strncpy(color, v[0] == '#' ? v+1 : v, sizeof(color)-1);
    if ((v = get_config(path, "brightness"))) { brightness = atoi(v); if (brightness < 1) brightness = 1; if (brightness > 100) brightness = 100; }
    if ((v = get_config(path, "width"))) { width = atoi(v); if (width < 10) width = 10; if (width > 500) width = 500; }
    if ((v = get_config(path, "fullscreen"))) fullscreen = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    if ((v = get_config(path, "video_device"))) strncpy(video_dev, v, sizeof(video_dev)-1);
    if ((v = get_config(path, "screens"))) parse_list(v, screens, &screen_count, MAX_ITEMS);
    if ((v = get_config(path, "watch_processes"))) parse_list(v, procs, &proc_count, MAX_ITEMS);
}

/* Resolve video device symlinks once at startup */
static void resolve_video_dev(void) {
    char *resolved = realpath(video_dev, NULL);
    if (resolved) {
        strncpy(video_dev_real, resolved, sizeof(video_dev_real) - 1);
        free(resolved);
    } else {
        strncpy(video_dev_real, video_dev, sizeof(video_dev_real) - 1);
    }
    log_msg("Video device: %s -> %s\n", video_dev, video_dev_real);
}

/* Get process name from /proc/[pid]/comm */
static bool get_proc_name(pid_t pid, char *buf, size_t len) {
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

/* Check if process name matches our watch list */
static bool is_watched_proc(const char *name) {
    for (int i = 0; i < proc_count; i++) {
        if (strcasecmp(name, procs[i]) == 0) return true;
        /* Also check partial match for things like "python3" running "howdy" */
        if (strcasestr(name, procs[i])) return true;
    }
    return false;
}

/* V4L2 streaming check - returns true if camera is in use */
static bool v4l2_streaming(void) {
    int fd = open(video_dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    
    struct v4l2_requestbuffers req = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP
    };
    
    int ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    int err = errno;
    close(fd);
    
    return (ret < 0 && err == EBUSY);
}

/* Setup netlink proc connector */
static int setup_netlink(void) {
    nl_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (nl_sock < 0) {
        log_err("Netlink socket failed (need root?): %s\n", strerror(errno));
        return -1;
    }
    
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_groups = CN_IDX_PROC,
        .nl_pid = getpid()
    };
    
    if (bind(nl_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_err("Netlink bind failed: %s\n", strerror(errno));
        close(nl_sock);
        nl_sock = -1;
        return -1;
    }
    
    /* Subscribe to proc events */
    struct {
        struct nlmsghdr nl;
        struct cn_msg cn;
        enum proc_cn_mcast_op op;
    } msg = {
        .nl = {
            .nlmsg_len = sizeof(msg),
            .nlmsg_type = NLMSG_DONE,
            .nlmsg_pid = getpid()
        },
        .cn = {
            .id = { .idx = CN_IDX_PROC, .val = CN_VAL_PROC },
            .len = sizeof(enum proc_cn_mcast_op)
        },
        .op = PROC_CN_MCAST_LISTEN
    };
    
    if (send(nl_sock, &msg, sizeof(msg), 0) < 0) {
        log_err("Netlink subscribe failed: %s\n", strerror(errno));
        close(nl_sock);
        nl_sock = -1;
        return -1;
    }
    
    log_msg("Netlink proc connector ready\n");
    return 0;
}

static void start_overlay(void);
static void stop_overlay(void);

/* Process a netlink event - returns true if state changed */
static bool handle_netlink_event(void) {
    char buf[4096];
    ssize_t len = recv(nl_sock, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0) return false;
    
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    if (!NLMSG_OK(nlh, (size_t)len)) return false;
    
    struct cn_msg *cn = NLMSG_DATA(nlh);
    struct proc_event *ev = (struct proc_event *)cn->data;
    
    bool state_changed = false;
    char proc_name[256];
    
    switch (ev->what) {
        case PROC_EVENT_EXEC: {
            pid_t pid = ev->event_data.exec.process_pid;
            if (get_proc_name(pid, proc_name, sizeof(proc_name)) && is_watched_proc(proc_name)) {
                log_msg("Watched process started: %s (pid %d)\n", proc_name, pid);
                watched_proc_count++;
                state_changed = true;
            }
            break;
        }
        case PROC_EVENT_EXIT: {
            /* We can't get the name of an exited process, so we track by count */
            /* When watched_proc_count > 0 and a process exits, we recheck */
            if (watched_proc_count > 0 || overlay_active) {
                state_changed = true;  /* Trigger a V4L2 check */
            }
            break;
        }
        default:
            break;
    }
    
    return state_changed;
}

/* Drain all pending netlink events, return true if any relevant */
static bool process_netlink_events(void) {
    bool any_relevant = false;
    int count = 0;
    
    while (count < 100) {  /* Limit to avoid infinite loop */
        if (!handle_netlink_event()) break;
        any_relevant = true;
        count++;
    }
    
    return any_relevant;
}

static void start_overlay(void) {
    if (overlay_active) return;
    
    log_msg("Starting overlay\n");
    
    char bstr[16], wstr[16], sstr[16];
    snprintf(bstr, sizeof(bstr), "%d", brightness);
    snprintf(wstr, sizeof(wstr), "%d", width);
    
    int num_screens = screen_count > 0 ? screen_count : 1;
    overlay_count = 0;
    
    for (int i = 0; i < num_screens && overlay_count < MAX_ITEMS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Build argument list */
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
            
            /* Debug: log what we're executing */
            fprintf(stderr, "[monitor] Exec:");
            for (int j = 0; args[j]; j++) fprintf(stderr, " %s", args[j]);
            fprintf(stderr, "\n");
            
            /* Check environment */
            if (!getenv("WAYLAND_DISPLAY")) {
                fprintf(stderr, "[monitor] WARNING: WAYLAND_DISPLAY not set!\n");
            }
            if (!getenv("XDG_RUNTIME_DIR")) {
                fprintf(stderr, "[monitor] WARNING: XDG_RUNTIME_DIR not set!\n");
            }
            
            execvp("ringlight-overlay", args);
            
            /* If we get here, exec failed */
            fprintf(stderr, "[monitor] ERROR: exec failed: %s\n", strerror(errno));
            _exit(1);
        } else if (pid > 0) {
            overlay_pids[overlay_count++] = pid;
            log_msg("Spawned overlay pid %d\n", pid);
        } else {
            log_err("Fork failed: %s\n", strerror(errno));
        }
    }
    
    overlay_active = (overlay_count > 0);
}

static void stop_overlay(void) {
    if (!overlay_active) return;
    
    log_msg("Stopping overlay\n");
    
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_pids[i] > 0) {
            kill(overlay_pids[i], SIGTERM);
            waitpid(overlay_pids[i], NULL, 0);
            overlay_pids[i] = 0;
        }
    }
    overlay_count = 0;
    overlay_active = false;
    watched_proc_count = 0;  /* Reset counter */
}

static void cleanup(void) {
    stop_overlay();
    if (nl_sock >= 0) close(nl_sock);
    for (int i = 0; i < proc_count; i++) free(procs[i]);
    for (int i = 0; i < screen_count; i++) free(screens[i]);
}

static void usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  -d, --device PATH    Video device (default: /dev/video0)\n"
           "  -p, --proc NAME      Process to watch (can repeat, default: howdy)\n"
           "  -v, --verbose        Verbose output\n"
           "  -h, --help           Show help\n", prog);
}

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"device", required_argument, 0, 'd'},
        {"proc", required_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "d:p:vh", opts, NULL)) != -1) {
        switch (c) {
            case 'd': strncpy(video_dev, optarg, sizeof(video_dev)-1); break;
            case 'p': if (proc_count < MAX_ITEMS) procs[proc_count++] = strdup(optarg); break;
            case 'v': verbose = true; break;
            case 'h': usage(argv[0]); return 0;
        }
    }
    
    load_config();
    
    /* Default watch process */
    if (proc_count == 0) procs[proc_count++] = strdup("howdy");
    
    resolve_video_dev();
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);
    
    atexit(cleanup);
    
    log_msg("Starting monitor\n");
    log_msg("Watching processes:");
    for (int i = 0; i < proc_count; i++) log_msg(" %s", procs[i]);
    log_msg("\n");
    
    /* Try to setup netlink */
    bool have_netlink = (setup_netlink() == 0);
    
    if (!have_netlink) {
        log_err("Warning: Running in poll-only mode (slower, higher CPU)\n");
    }
    
    /* Main loop */
    while (running) {
        bool need_check = false;
        time_t now = time(NULL);
        
        if (have_netlink) {
            /* Wait for netlink events with timeout */
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(nl_sock, &fds);
            
            /* Timeout: shorter if overlay active (to catch exits), longer if idle */
            struct timeval tv = {
                .tv_sec = overlay_active ? 1 : V4L2_POLL_INTERVAL,
                .tv_usec = 0
            };
            
            int ret = select(nl_sock + 1, &fds, NULL, NULL, &tv);
            
            if (ret > 0 && FD_ISSET(nl_sock, &fds)) {
                /* Process events, only check V4L2 if something relevant happened */
                need_check = process_netlink_events();
            } else if (ret == 0) {
                /* Timeout - do periodic V4L2 check only if overlay not active */
                if (!overlay_active && (now - last_v4l2_check >= V4L2_POLL_INTERVAL)) {
                    need_check = true;
                } else if (overlay_active) {
                    /* Verify overlay children are still running */
                    bool any_alive = false;
                    for (int i = 0; i < overlay_count; i++) {
                        if (overlay_pids[i] > 0 && kill(overlay_pids[i], 0) == 0) {
                            any_alive = true;
                            break;
                        }
                    }
                    if (!any_alive) {
                        log_msg("Overlay processes died\n");
                        overlay_active = false;
                        overlay_count = 0;
                    }
                    need_check = true;  /* Check if we should restart */
                }
            }
        } else {
            /* Fallback: pure polling mode */
            sleep(V4L2_POLL_INTERVAL);
            need_check = true;
        }
        
        /* Only check V4L2 when needed */
        if (need_check) {
            bool cam_in_use = v4l2_streaming();
            last_v4l2_check = now;
            
            if (cam_in_use && !overlay_active) {
                log_msg("Camera active, starting overlay\n");
                start_overlay();
            } else if (!cam_in_use && overlay_active) {
                log_msg("Camera inactive, stopping overlay\n");
                stop_overlay();
            }
        }
    }
    
    log_msg("Shutting down\n");
    return 0;
}

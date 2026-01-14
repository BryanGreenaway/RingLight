/*
 * RingLight Monitor - Lightweight webcam detector daemon
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <pwd.h>

#define MAX_ITEMS 16

static volatile sig_atomic_t running = 1;
static pid_t overlay_pids[MAX_ITEMS];
static int overlay_count = 0;
static bool verbose = false;

static char video_dev[256] = "/dev/video0";
static char video_dev_real[PATH_MAX] = "";
static int poll_ms = 1000;  /* Was 150ms - key optimization! */
static char color[32] = "FFFFFF";
static int brightness = 100, width = 80;
static bool fullscreen = false;
static char *procs[MAX_ITEMS], *screens[MAX_ITEMS];
static int proc_count = 0, screen_count = 0;

static void sig_handler(int s) { (void)s; running = 0; }
#define log_msg(...) do { if (verbose) fprintf(stderr, "[ringlight-monitor] " __VA_ARGS__); } while(0)

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
    if ((v = get_config(path, "brightness"))) { brightness = atoi(v); brightness = brightness < 1 ? 1 : brightness > 100 ? 100 : brightness; }
    if ((v = get_config(path, "width"))) { width = atoi(v); width = width < 10 ? 10 : width > 500 ? 500 : width; }
    if ((v = get_config(path, "fullscreen"))) fullscreen = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    if ((v = get_config(path, "videoDevice")) && *v) strncpy(video_dev, v, sizeof(video_dev)-1);
    if ((v = get_config(path, "processes"))) parse_list(v, procs, &proc_count, MAX_ITEMS);
    if ((v = get_config(path, "enabledScreens"))) parse_list(v, screens, &screen_count, MAX_ITEMS);
    else if ((v = get_config(path, "enabledScreenIndices"))) parse_list(v, screens, &screen_count, MAX_ITEMS);
    if (proc_count == 0) procs[proc_count++] = strdup("howdy");
    if (!realpath(video_dev, video_dev_real)) strncpy(video_dev_real, video_dev, sizeof(video_dev_real) - 1);
}

/* Single-pass /proc scan - checks process names AND fd targets in one traversal */
static bool scan_proc(void) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    pid_t mypid = getpid();
    struct dirent *e;
    bool found = false;
    
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        long pid = strtol(e->d_name, NULL, 10);
        if (pid <= 0 || pid == mypid) continue;
        
        char path[280];
        /* Check process name */
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            char comm[64];
            if (fgets(comm, sizeof(comm), f)) {
                comm[strcspn(comm, "\n")] = '\0';
                for (int i = 0; i < proc_count && !found; i++)
                    if (strcmp(comm, procs[i]) == 0) { log_msg("Process: %s\n", procs[i]); found = true; }
            }
            fclose(f);
        }
        if (found) break;
        
        /* Check file descriptors */
        snprintf(path, sizeof(path), "/proc/%s/fd", e->d_name);
        DIR *fds = opendir(path);
        if (!fds) continue;
        struct dirent *fe;
        while ((fe = readdir(fds))) {
            char lp[512], t[PATH_MAX];
            snprintf(lp, sizeof(lp), "%s/%s", path, fe->d_name);
            ssize_t l = readlink(lp, t, sizeof(t) - 1);
            if (l > 0) { t[l] = '\0'; if (strcmp(t, video_dev_real) == 0) { log_msg("FD open by %ld\n", pid); found = true; break; } }
        }
        closedir(fds);
    }
    closedir(d);
    return found;
}

static bool v4l2_busy(void) {
    int fd = open(video_dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct v4l2_requestbuffers r = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    int ret = ioctl(fd, VIDIOC_REQBUFS, &r);
    int err = errno;
    close(fd);
    return (ret < 0 && err == EBUSY);
}

static bool cam_active(void) { return scan_proc() || v4l2_busy(); }

static void start_overlay(void) {
    if (overlay_count > 0 && overlay_pids[0] > 0 && kill(overlay_pids[0], 0) == 0) return;
    log_msg("Starting overlay\n");
    
    char bstr[16], wstr[16];
    snprintf(bstr, sizeof(bstr), "%d", brightness);
    snprintf(wstr, sizeof(wstr), "%d", width);
    
    int n = screen_count > 0 ? screen_count : 1;
    overlay_count = 0;
    for (int i = 0; i < n && overlay_count < MAX_ITEMS; i++) {
        pid_t p = fork();
        if (p < 0) continue;
        if (p == 0) {
            char *argv[16] = {"ringlight-overlay", "-c", color, "-b", bstr, "-w", wstr};
            int c = 7;
            if (fullscreen) argv[c++] = "-f";
            if (screen_count > 0 && screens[i]) { argv[c++] = "-s"; argv[c++] = screens[i]; }
            argv[c] = NULL;
            execvp("ringlight-overlay", argv);
            _exit(1);
        }
        overlay_pids[overlay_count++] = p;
    }
}

static void stop_overlay(void) {
    if (overlay_count == 0) return;
    log_msg("Stopping overlay\n");
    for (int i = 0; i < overlay_count; i++) if (overlay_pids[i] > 0) kill(overlay_pids[i], SIGTERM);
    for (int j = 0; j < 10; j++) {
        bool done = true;
        for (int i = 0; i < overlay_count; i++)
            if (overlay_pids[i] > 0 && waitpid(overlay_pids[i], NULL, WNOHANG) == 0) done = false;
            else overlay_pids[i] = 0;
        if (done) break;
        usleep(50000);
    }
    for (int i = 0; i < overlay_count; i++) if (overlay_pids[i] > 0) { kill(overlay_pids[i], SIGKILL); waitpid(overlay_pids[i], NULL, 0); }
    overlay_count = 0;
}

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"device", 1, 0, 'd'}, {"interval", 1, 0, 'i'}, {"process", 1, 0, 'p'},
        {"color", 1, 0, 'c'}, {"brightness", 1, 0, 'b'}, {"width", 1, 0, 'w'},
        {"screens", 1, 0, 's'}, {"fullscreen", 0, 0, 'f'}, {"verbose", 0, 0, 'v'}, {"help", 0, 0, 'h'}, {0}
    };
    
    char *cli_procs[MAX_ITEMS] = {0}, *cli_screens[MAX_ITEMS] = {0};
    int cli_pc = 0, cli_sc = 0;
    char cli_col[32] = "", cli_dev[256] = "";
    int cli_br = 0, cli_wd = 0;
    bool has_p = false, has_s = false, has_c = false, has_br = false, has_wd = false, has_d = false, has_f = false;
    
    int o;
    while ((o = getopt_long(argc, argv, "d:i:p:c:b:w:s:fvh", opts, NULL)) != -1) {
        switch (o) {
        case 'd': strncpy(cli_dev, optarg, sizeof(cli_dev)-1); has_d = true; break;
        case 'i': poll_ms = atoi(optarg); if (poll_ms < 100) poll_ms = 100; break;
        case 'p': if (cli_pc < MAX_ITEMS) cli_procs[cli_pc++] = strdup(optarg); has_p = true; break;
        case 'c': strncpy(cli_col, optarg, sizeof(cli_col)-1); has_c = true; break;
        case 'b': cli_br = atoi(optarg); has_br = true; break;
        case 'w': cli_wd = atoi(optarg); has_wd = true; break;
        case 's': parse_list(optarg, cli_screens, &cli_sc, MAX_ITEMS); has_s = true; break;
        case 'f': has_f = true; break;
        case 'v': verbose = true; break;
        case 'h': printf("ringlight-monitor - Auto ring light\nUsage: %s [-d dev] [-i ms] [-p proc] [-c hex] [-b 1-100] [-w px] [-s screens] [-f] [-v]\n", argv[0]); return 0;
        }
    }
    
    load_config();
    
    if (has_d) { strncpy(video_dev, cli_dev, sizeof(video_dev)-1); realpath(video_dev, video_dev_real) || strncpy(video_dev_real, video_dev, sizeof(video_dev_real)-1); }
    if (has_c) strncpy(color, cli_col, sizeof(color)-1);
    if (has_br) { brightness = cli_br; brightness = brightness < 1 ? 1 : brightness > 100 ? 100 : brightness; }
    if (has_wd) { width = cli_wd; width = width < 10 ? 10 : width > 500 ? 500 : width; }
    if (has_f) fullscreen = true;
    if (has_p) { proc_count = 0; for (int i = 0; i < cli_pc; i++) procs[proc_count++] = cli_procs[i]; }
    if (has_s) { screen_count = 0; for (int i = 0; i < cli_sc; i++) screens[screen_count++] = cli_screens[i]; }
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);
    atexit(stop_overlay);
    
    log_msg("Monitoring %s @ %dms\n", video_dev, poll_ms);
    
    bool was = false;
    while (running) {
        bool is = cam_active();
        if (is && !was) start_overlay();
        else if (!is && was) stop_overlay();
        was = is;
        usleep(poll_ms * 1000);
    }
    return 0;
}

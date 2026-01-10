/*
 * RingLight Monitor - Webcam/Howdy detector for automatic ring light
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
static pid_t overlay_pids[MAX_ITEMS] = {0};
static int overlay_count = 0;
static bool verbose = false;

// Config
static char video_dev[256] = "/dev/video0";
static int poll_ms = 150;
static char color[32] = "FFFFFF";
static int brightness = 100;
static int width = 80;
static char *procs[MAX_ITEMS] = {NULL};
static int proc_count = 0;
static char *screens[MAX_ITEMS] = {NULL};
static int screen_count = 0;

static void sig_handler(int s) { (void)s; running = 0; }

#define log_msg(...) do { if (verbose) { fprintf(stderr, "[ringlight-monitor] " __VA_ARGS__); fprintf(stderr, "\n"); } } while(0)

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == '\n' || *e == '\r' || *e == ' ')) *e-- = '\0';
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
    log_msg("Loading config: %s", path);
    
    char *v;
    if ((v = get_config(path, "color"))) { strncpy(color, v[0] == '#' ? v+1 : v, sizeof(color)-1); log_msg("  color: %s", color); }
    if ((v = get_config(path, "brightness"))) { brightness = atoi(v); if (brightness < 1) brightness = 1; if (brightness > 100) brightness = 100; }
    if ((v = get_config(path, "width"))) { width = atoi(v); if (width < 10) width = 10; if (width > 500) width = 500; }
    if ((v = get_config(path, "videoDevice")) && *v) strncpy(video_dev, v, sizeof(video_dev)-1);
    if ((v = get_config(path, "processes"))) parse_list(v, procs, &proc_count, MAX_ITEMS);
    if ((v = get_config(path, "enabledScreenIndices"))) parse_list(v, screens, &screen_count, MAX_ITEMS);
    else if ((v = get_config(path, "enabledScreens"))) parse_list(v, screens, &screen_count, MAX_ITEMS);
    
    if (proc_count == 0) procs[proc_count++] = "howdy";
}

static bool proc_running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        
        char p[280], c[256];
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(c, sizeof(c), f)) {
            c[strcspn(c, "\n")] = 0;
            if (strcmp(c, name) == 0) { fclose(f); closedir(d); return true; }
        }
        fclose(f);
    }
    closedir(d);
    return false;
}

static bool watched_active(void) {
    for (int i = 0; i < proc_count && procs[i]; i++)
        if (proc_running(procs[i])) { log_msg("Process active: %s", procs[i]); return true; }
    return false;
}

static bool dev_in_use(const char *dev) {
    char rp[PATH_MAX];
    if (!realpath(dev, rp)) return false;
    
    DIR *d = opendir("/proc");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0 || pid == getpid()) continue;
        
        char fp[280];
        snprintf(fp, sizeof(fp), "/proc/%s/fd", e->d_name);
        DIR *fds = opendir(fp);
        if (!fds) continue;
        
        struct dirent *fe;
        while ((fe = readdir(fds))) {
            char lp[512], t[PATH_MAX];
            snprintf(lp, sizeof(lp), "%s/%s", fp, fe->d_name);
            ssize_t l = readlink(lp, t, sizeof(t)-1);
            if (l > 0) { t[l] = 0; if (strcmp(t, rp) == 0) {
                log_msg("Device in use by PID %s", e->d_name);
                closedir(fds); closedir(d); return true;
            }}
        }
        closedir(fds);
    }
    closedir(d);
    return false;
}

static bool v4l2_busy(const char *dev) {
    int fd = open(dev, O_RDONLY|O_NONBLOCK);
    if (fd < 0) return false;
    struct v4l2_requestbuffers r = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    int ret = ioctl(fd, VIDIOC_REQBUFS, &r);
    int err = errno;
    close(fd);
    if (ret < 0 && err == EBUSY) { log_msg("V4L2 device streaming"); return true; }
    return false;
}

static bool cam_active(void) {
    return watched_active() || dev_in_use(video_dev) || v4l2_busy(video_dev);
}

static void start_overlay(void) {
    // Check if already running
    if (overlay_count > 0 && overlay_pids[0] > 0 && kill(overlay_pids[0], 0) == 0) return;
    
    log_msg("Starting overlay(s)");
    
    char bstr[16], wstr[16];
    snprintf(bstr, sizeof(bstr), "%d", brightness);
    snprintf(wstr, sizeof(wstr), "%d", width);
    
    // Spawn one process per screen (or just one if no screens specified)
    int num_screens = screen_count > 0 ? screen_count : 1;
    overlay_count = 0;
    
    for (int i = 0; i < num_screens && overlay_count < MAX_ITEMS; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork"); continue; }
        
        if (p == 0) {
            char *argv[16] = {"ringlight-overlay", "-c", color, "-b", bstr, "-w", wstr};
            int c = 7;
            if (screen_count > 0) {
                argv[c++] = "-s";
                argv[c++] = screens[i];
            }
            argv[c] = NULL;
            execvp("ringlight-overlay", argv);
            _exit(1);
        }
        
        overlay_pids[overlay_count++] = p;
        log_msg("Started PID %d for screen %s", p, screen_count > 0 ? screens[i] : "default");
    }
}

static void stop_overlay(void) {
    if (overlay_count == 0) return;
    
    log_msg("Stopping %d overlay(s)", overlay_count);
    
    // Send SIGTERM to all
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_pids[i] > 0) kill(overlay_pids[i], SIGTERM);
    }
    
    // Wait for termination
    for (int j = 0; j < 10; j++) {
        bool all_done = true;
        for (int i = 0; i < overlay_count; i++) {
            if (overlay_pids[i] > 0 && waitpid(overlay_pids[i], NULL, WNOHANG) == 0) {
                all_done = false;
            } else {
                overlay_pids[i] = 0;
            }
        }
        if (all_done) break;
        usleep(50000);
    }
    
    // Force kill any remaining
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_pids[i] > 0) {
            kill(overlay_pids[i], SIGKILL);
            waitpid(overlay_pids[i], NULL, 0);
            overlay_pids[i] = 0;
        }
    }
    
    overlay_count = 0;
}

static void cleanup(void) { stop_overlay(); }

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"device", 1, 0, 'd'}, {"interval", 1, 0, 'i'}, {"process", 1, 0, 'p'},
        {"color", 1, 0, 'c'}, {"brightness", 1, 0, 'b'}, {"width", 1, 0, 'w'},
        {"screens", 1, 0, 's'}, {"verbose", 0, 0, 'v'}, {"help", 0, 0, 'h'}, {0}
    };
    
    // Temp storage for CLI overrides
    char *cli_procs[MAX_ITEMS] = {NULL}; int cli_proc_cnt = 0;
    char *cli_screens[MAX_ITEMS] = {NULL}; int cli_screen_cnt = 0;
    char cli_color[32] = "", cli_dev[256] = "";
    int cli_bright = 0, cli_width = 0;
    bool has_procs = false, has_screens = false, has_color = false, has_bright = false, has_width = false, has_dev = false;
    
    int o;
    while ((o = getopt_long(argc, argv, "d:i:p:c:b:w:s:vh", opts, NULL)) != -1) {
        switch (o) {
        case 'd': strncpy(cli_dev, optarg, sizeof(cli_dev)-1); has_dev = true; break;
        case 'i': poll_ms = atoi(optarg); if (poll_ms < 10) poll_ms = 10; break;
        case 'p': if (cli_proc_cnt < MAX_ITEMS) cli_procs[cli_proc_cnt++] = strdup(optarg); has_procs = true; break;
        case 'c': strncpy(cli_color, optarg, sizeof(cli_color)-1); has_color = true; break;
        case 'b': cli_bright = atoi(optarg); has_bright = true; break;
        case 'w': cli_width = atoi(optarg); has_width = true; break;
        case 's': parse_list(optarg, cli_screens, &cli_screen_cnt, MAX_ITEMS); has_screens = true; break;
        case 'v': verbose = true; break;
        case 'h': printf("ringlight-monitor - Auto ring light\nUsage: %s [-d dev] [-i ms] [-p proc] [-c hex] [-b 1-100] [-w px] [-s screens] [-v]\n", argv[0]); return 0;
        }
    }
    
    load_config();
    
    // Apply CLI overrides
    if (has_dev) strncpy(video_dev, cli_dev, sizeof(video_dev)-1);
    if (has_color) strncpy(color, cli_color, sizeof(color)-1);
    if (has_bright) { brightness = cli_bright; if (brightness < 1) brightness = 1; if (brightness > 100) brightness = 100; }
    if (has_width) { width = cli_width; if (width < 10) width = 10; if (width > 500) width = 500; }
    if (has_procs) { proc_count = 0; for (int i = 0; i < cli_proc_cnt; i++) procs[proc_count++] = cli_procs[i]; }
    if (has_screens) { screen_count = 0; for (int i = 0; i < cli_screen_cnt; i++) screens[screen_count++] = cli_screens[i]; }
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);
    atexit(cleanup);
    
    log_msg("Monitoring %s @ %dms, %d proc(s), color=%s bright=%d width=%d", video_dev, poll_ms, proc_count, color, brightness, width);
    
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

/*
 * RingLight Monitor - Webcam/Howdy detector for automatic ring light
 * 
 * Watches for webcam activity and spawns ringlight-overlay processes.
 * Reads settings from ~/.config/ringlight/config.ini by default.
 *
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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

#define DEFAULT_POLL_MS 150
#define DEFAULT_DEVICE "/dev/video0"
#define MAX_PROCS 16
#define MAX_SCREENS 8

static volatile sig_atomic_t running = 1;
static pid_t overlay_pids[MAX_SCREENS] = {0};
static int overlay_count = 0;

// Settings (can be loaded from config file)
static char video_dev[256] = DEFAULT_DEVICE;
static int poll_ms = DEFAULT_POLL_MS;
static bool verbose = false;
static const char *procs[MAX_PROCS] = {NULL};
static int proc_count = 0;
static char *screens[MAX_SCREENS] = {NULL};
static int screen_count = 0;

// Overlay appearance settings
static char color[32] = "FFFFFF";
static int brightness = 100;
static int width = 80;

static void sig_handler(int s) { (void)s; running = 0; }

static void log_msg(const char *fmt, ...) {
    if (!verbose) return;
    va_list a; va_start(a, fmt);
    fprintf(stderr, "[ringlight-monitor] ");
    vfprintf(stderr, fmt, a);
    va_end(a);
    fprintf(stderr, "\n");
}

// Simple INI parser - get value for key
static char* get_config_value(const char *filepath, const char *key) {
    FILE *f = fopen(filepath, "r");
    if (!f) return NULL;
    
    char line[512];
    static char value[256];
    size_t keylen = strlen(key);
    
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and sections
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        
        // Find key=value
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        // Check if key matches
        size_t len = eq - line;
        if (len == keylen && strncmp(line, key, keylen) == 0) {
            // Extract value (trim whitespace and newline)
            char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            strncpy(value, v, sizeof(value) - 1);
            value[sizeof(value) - 1] = '\0';
            
            // Trim trailing whitespace/newline
            char *end = value + strlen(value) - 1;
            while (end > value && (*end == '\n' || *end == '\r' || *end == ' ')) {
                *end-- = '\0';
            }
            
            fclose(f);
            return value;
        }
    }
    
    fclose(f);
    return NULL;
}

static void load_config(void) {
    // Build config path: ~/.config/ringlight/config.ini
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return;
    
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/.config/ringlight/config.ini", home);
    
    log_msg("Loading config from: %s", config_path);
    
    // Load color (remove # if present)
    char *val = get_config_value(config_path, "color");
    if (val) {
        if (val[0] == '#') val++;
        strncpy(color, val, sizeof(color) - 1);
        log_msg("  color: %s", color);
    }
    
    // Load brightness
    val = get_config_value(config_path, "brightness");
    if (val) {
        brightness = atoi(val);
        if (brightness < 1) brightness = 1;
        if (brightness > 100) brightness = 100;
        log_msg("  brightness: %d", brightness);
    }
    
    // Load width
    val = get_config_value(config_path, "width");
    if (val) {
        width = atoi(val);
        if (width < 10) width = 10;
        if (width > 500) width = 500;
        log_msg("  width: %d", width);
    }
    
    // Load video device
    val = get_config_value(config_path, "videoDevice");
    if (val && strlen(val) > 0) {
        strncpy(video_dev, val, sizeof(video_dev) - 1);
        log_msg("  videoDevice: %s", video_dev);
    }
    
    // Load processes to watch
    val = get_config_value(config_path, "processes");
    if (val && strlen(val) > 0) {
        // Parse comma-separated list
        char *copy = strdup(val);
        char *tok = strtok(copy, ",");
        while (tok && proc_count < MAX_PROCS - 1) {
            // Trim whitespace
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';
            
            if (strlen(tok) > 0) {
                procs[proc_count++] = strdup(tok);
                log_msg("  watch process: %s", tok);
            }
            tok = strtok(NULL, ",");
        }
        free(copy);
    }
    
    // Load enabled screens (prefer indices if available)
    val = get_config_value(config_path, "enabledScreenIndices");
    if (val && strlen(val) > 0) {
        char *copy = strdup(val);
        char *tok = strtok(copy, ",");
        while (tok && screen_count < MAX_SCREENS) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';
            
            if (strlen(tok) > 0) {
                screens[screen_count++] = strdup(tok);
                log_msg("  screen index: %s", tok);
            }
            tok = strtok(NULL, ",");
        }
        free(copy);
    } else {
        // Fall back to screen names if indices not available
        val = get_config_value(config_path, "enabledScreens");
        if (val && strlen(val) > 0) {
            char *copy = strdup(val);
            char *tok = strtok(copy, ",");
            while (tok && screen_count < MAX_SCREENS) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                
                if (strlen(tok) > 0) {
                    screens[screen_count++] = strdup(tok);
                    log_msg("  screen: %s", tok);
                }
                tok = strtok(NULL, ",");
            }
            free(copy);
        }
    }
    
    // Default to howdy if no processes specified
    if (proc_count == 0) {
        procs[proc_count++] = "howdy";
        log_msg("  watch process (default): howdy");
    }
}

static bool proc_running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        char p[280]; snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char c[256];
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
    char rp[PATH_MAX]; if (!realpath(dev, rp)) return false;
    DIR *d = opendir("/proc"); if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0 || pid == getpid()) continue;
        char fp[280]; snprintf(fp, sizeof(fp), "/proc/%s/fd", e->d_name);
        DIR *fds = opendir(fp); if (!fds) continue;
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
    int fd = open(dev, O_RDONLY|O_NONBLOCK); if (fd < 0) return false;
    struct v4l2_requestbuffers r = {0};
    r.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; r.memory = V4L2_MEMORY_MMAP;
    int ret = ioctl(fd, VIDIOC_REQBUFS, &r); int err = errno;
    close(fd);
    if (ret < 0 && err == EBUSY) { log_msg("V4L2 device streaming"); return true; }
    return false;
}

static bool cam_active(void) {
    return watched_active() || dev_in_use(video_dev) || v4l2_busy(video_dev);
}

static void start_overlays(void) {
    // Check if already running
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_pids[i] > 0 && kill(overlay_pids[i], 0) == 0) {
            return; // Already running
        }
    }
    
    log_msg("Starting overlays (color=%s, brightness=%d, width=%d)", color, brightness, width);
    overlay_count = 0;
    
    // Build common args
    char bright_str[16], width_str[16];
    snprintf(bright_str, sizeof(bright_str), "%d", brightness);
    snprintf(width_str, sizeof(width_str), "%d", width);
    
    // If no screens specified, just start one on screen 0
    if (screen_count == 0) {
        pid_t p = fork();
        if (p < 0) { perror("fork"); return; }
        if (p == 0) {
            execlp("ringlight-overlay", "ringlight-overlay",
                   "-s", "0",
                   "-c", color,
                   "-b", bright_str,
                   "-w", width_str,
                   (char*)NULL);
            perror("exec ringlight-overlay");
            _exit(1);
        }
        overlay_pids[overlay_count++] = p;
        log_msg("Started overlay PID %d (screen 0)", p);
        return;
    }
    
    // Start one overlay per screen
    for (int i = 0; i < screen_count && i < MAX_SCREENS; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork"); continue; }
        if (p == 0) {
            // screens[i] contains the screen name from config
            // For now pass it directly - overlay will try to match
            execlp("ringlight-overlay", "ringlight-overlay",
                   "-s", screens[i],
                   "-c", color,
                   "-b", bright_str,
                   "-w", width_str,
                   (char*)NULL);
            perror("exec ringlight-overlay");
            _exit(1);
        }
        overlay_pids[overlay_count++] = p;
        log_msg("Started overlay PID %d for screen %s", p, screens[i]);
    }
}

static void stop_overlays(void) {
    log_msg("Stopping overlays");
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_pids[i] <= 0) continue;
        kill(overlay_pids[i], SIGTERM);
        for (int j = 0; j < 10; j++) {
            if (waitpid(overlay_pids[i], NULL, WNOHANG) != 0) {
                overlay_pids[i] = 0;
                break;
            }
            usleep(50000);
        }
        if (overlay_pids[i] > 0) {
            kill(overlay_pids[i], SIGKILL);
            waitpid(overlay_pids[i], NULL, 0);
            overlay_pids[i] = 0;
        }
    }
    overlay_count = 0;
}

static void cleanup(void) { stop_overlays(); }

static void print_help(const char *prog) {
    printf("ringlight-monitor - Auto ring light for webcam/Howdy\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("By default, reads settings from ~/.config/ringlight/config.ini\n\n");
    printf("Options:\n");
    printf("  -d, --device DEV     Video device (default: from config or %s)\n", DEFAULT_DEVICE);
    printf("  -i, --interval MS    Poll interval (default: %d)\n", DEFAULT_POLL_MS);
    printf("  -p, --process NAME   Process to watch (repeatable, overrides config)\n");
    printf("  -c, --color HEX      Ring color RRGGBB (overrides config)\n");
    printf("  -b, --brightness N   Brightness 1-100 (overrides config)\n");
    printf("  -w, --width PX       Border width (overrides config)\n");
    printf("  -s, --screens LIST   Comma-separated screen indices (overrides config)\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -h, --help           Show help\n");
    printf("\nExamples:\n");
    printf("  %s -v                      # Use GUI settings, verbose\n", prog);
    printf("  %s -p howdy -p zoom        # Watch specific processes\n", prog);
    printf("  %s -s 0,1 -c FF9900        # Screens 0,1 with orange color\n", prog);
}

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"device", required_argument, 0, 'd'},
        {"interval", required_argument, 0, 'i'},
        {"process", required_argument, 0, 'p'},
        {"color", required_argument, 0, 'c'},
        {"brightness", required_argument, 0, 'b'},
        {"width", required_argument, 0, 'w'},
        {"screens", required_argument, 0, 's'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    // Track which settings were overridden by command line
    bool custom_procs = false;
    bool custom_screens = false;
    bool custom_color = false;
    bool custom_brightness = false;
    bool custom_width = false;
    bool custom_device = false;
    
    // Temp storage for command line values
    char cmd_color[32] = "";
    int cmd_brightness = 0;
    int cmd_width = 0;
    char cmd_device[256] = "";
    const char *cmd_procs[MAX_PROCS] = {NULL};
    int cmd_proc_count = 0;
    char *cmd_screens[MAX_SCREENS] = {NULL};
    int cmd_screen_count = 0;
    
    int o;
    while ((o = getopt_long(argc, argv, "d:i:p:c:b:w:s:vh", opts, NULL)) != -1) {
        switch (o) {
        case 'd': 
            strncpy(cmd_device, optarg, sizeof(cmd_device)-1);
            custom_device = true;
            break;
        case 'i': 
            poll_ms = atoi(optarg); 
            if (poll_ms < 10) poll_ms = 10; 
            break;
        case 'p':
            if (cmd_proc_count < MAX_PROCS-1) { 
                cmd_procs[cmd_proc_count++] = strdup(optarg);
            }
            custom_procs = true;
            break;
        case 'c':
            strncpy(cmd_color, optarg, sizeof(cmd_color)-1);
            custom_color = true;
            break;
        case 'b':
            cmd_brightness = atoi(optarg);
            custom_brightness = true;
            break;
        case 'w':
            cmd_width = atoi(optarg);
            custom_width = true;
            break;
        case 's': {
            char *copy = strdup(optarg);
            char *tok = strtok(copy, ",");
            while (tok && cmd_screen_count < MAX_SCREENS) {
                cmd_screens[cmd_screen_count++] = strdup(tok);
                tok = strtok(NULL, ",");
            }
            custom_screens = true;
            break;
        }
        case 'v': verbose = true; break;
        case 'h': print_help(argv[0]); return 0;
        default: print_help(argv[0]); return 1;
        }
    }
    
    // Load config file first
    load_config();
    
    // Apply command line overrides
    if (custom_device) {
        strncpy(video_dev, cmd_device, sizeof(video_dev)-1);
    }
    if (custom_color) {
        strncpy(color, cmd_color, sizeof(color)-1);
    }
    if (custom_brightness) {
        brightness = cmd_brightness;
        if (brightness < 1) brightness = 1;
        if (brightness > 100) brightness = 100;
    }
    if (custom_width) {
        width = cmd_width;
        if (width < 10) width = 10;
        if (width > 500) width = 500;
    }
    if (custom_procs) {
        proc_count = 0;
        for (int i = 0; i < cmd_proc_count; i++) {
            procs[proc_count++] = cmd_procs[i];
        }
    }
    if (custom_screens) {
        screen_count = 0;
        for (int i = 0; i < cmd_screen_count; i++) {
            screens[screen_count++] = cmd_screens[i];
        }
    }
    
    if (access(video_dev, F_OK) != 0) {
        fprintf(stderr, "Warning: %s not found\n", video_dev);
    }
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);
    atexit(cleanup);
    
    log_msg("Monitoring %s @ %dms", video_dev, poll_ms);
    log_msg("Watching %d process(es)", proc_count);
    log_msg("Overlay: color=%s brightness=%d width=%d", color, brightness, width);
    if (screen_count > 0) {
        log_msg("Target screens: %d", screen_count);
        for (int i = 0; i < screen_count; i++) {
            log_msg("  - %s", screens[i]);
        }
    } else {
        log_msg("Target screens: default (0)");
    }
    
    bool was = false;
    while (running) {
        bool is = cam_active();
        if (is && !was) start_overlays();
        else if (!is && was) stop_overlays();
        was = is;
        usleep(poll_ms * 1000);
    }
    
    return 0;
}

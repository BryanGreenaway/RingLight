/*
 * ringlight-monitor - Webcam usage monitor for RingLight
 * Detects when webcam is in use and automatically starts/stops ringlight.
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

#define DEFAULT_POLL_MS 150
#define DEFAULT_VIDEO_DEVICE "/dev/video0"
#define MAX_PROCESSES 16

static volatile sig_atomic_t running = 1;
static pid_t ringlight_pid = 0;
static char ringlight_args[512] = "";
static char video_device[256] = DEFAULT_VIDEO_DEVICE;
static int poll_interval_ms = DEFAULT_POLL_MS;
static bool verbose = false;
static const char *watch_processes[MAX_PROCESSES] = { "howdy", NULL };
static int watch_count = 1;

static void signal_handler(int sig) { (void)sig; running = 0; }

static void log_msg(const char *fmt, ...) {
    if (!verbose) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool process_running(const char *name) {
    DIR *proc = opendir("/proc");
    if (!proc) return false;
    struct dirent *entry;
    while ((entry = readdir(proc))) {
        if (entry->d_type != DT_DIR) continue;
        char *endptr;
        long pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid <= 0) continue;
        char path[280];
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char comm[256];
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;
            if (strcmp(comm, name) == 0) {
                fclose(f); closedir(proc); return true;
            }
        }
        fclose(f);
    }
    closedir(proc);
    return false;
}

static bool watched_process_active(void) {
    for (int i = 0; i < watch_count && watch_processes[i]; i++)
        if (process_running(watch_processes[i])) {
            log_msg("Process active: %s", watch_processes[i]);
            return true;
        }
    return false;
}

static bool device_in_use(const char *device) {
    char rpath[PATH_MAX];
    if (!realpath(device, rpath)) return false;
    DIR *proc = opendir("/proc");
    if (!proc) return false;
    struct dirent *entry;
    while ((entry = readdir(proc))) {
        if (entry->d_type != DT_DIR) continue;
        char *endptr;
        long pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid <= 0 || pid == getpid()) continue;
        char fd_path[280];
        snprintf(fd_path, sizeof(fd_path), "/proc/%s/fd", entry->d_name);
        DIR *fds = opendir(fd_path);
        if (!fds) continue;
        struct dirent *fd_entry;
        while ((fd_entry = readdir(fds))) {
            char link_path[512], target[PATH_MAX];
            snprintf(link_path, sizeof(link_path), "%s/%s", fd_path, fd_entry->d_name);
            ssize_t len = readlink(link_path, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                if (strcmp(target, rpath) == 0) {
                    log_msg("Device in use by PID %s", entry->d_name);
                    closedir(fds); closedir(proc); return true;
                }
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return false;
}

static bool v4l2_streaming(const char *device) {
    int fd = open(device, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct v4l2_requestbuffers req = {0};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    int ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    int err = errno;
    close(fd);
    if (ret < 0 && err == EBUSY) { log_msg("V4L2 streaming"); return true; }
    return false;
}

static bool webcam_active(void) {
    return watched_process_active() || device_in_use(video_device) || v4l2_streaming(video_device);
}

static void start_ringlight(void) {
    if (ringlight_pid > 0 && kill(ringlight_pid, 0) == 0) return;
    log_msg("Starting ringlight");
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) {
        char *argv[32] = { "ringlight", NULL };
        int argc = 1;
        if (ringlight_args[0]) {
            char *copy = strdup(ringlight_args);
            char *tok = strtok(copy, " ");
            while (tok && argc < 30) { argv[argc++] = tok; tok = strtok(NULL, " "); }
            argv[argc] = NULL;
        }
        execvp("ringlight", argv);
        perror("execvp"); _exit(1);
    }
    ringlight_pid = pid;
    log_msg("Started PID %d", pid);
}

static void stop_ringlight(void) {
    if (ringlight_pid <= 0) return;
    log_msg("Stopping PID %d", ringlight_pid);
    kill(ringlight_pid, SIGTERM);
    for (int i = 0; i < 10; i++) {
        if (waitpid(ringlight_pid, NULL, WNOHANG) != 0) { ringlight_pid = 0; return; }
        usleep(50000);
    }
    kill(ringlight_pid, SIGKILL);
    waitpid(ringlight_pid, NULL, 0);
    ringlight_pid = 0;
}

static void cleanup(void) { stop_ringlight(); }

static void print_usage(const char *prog) {
    printf("ringlight-monitor - Auto ring light when webcam active\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -d, --device DEV    Video device (default: %s)\n", DEFAULT_VIDEO_DEVICE);
    printf("  -i, --interval MS   Poll interval (default: %d)\n", DEFAULT_POLL_MS);
    printf("  -p, --process NAME  Process to watch (repeatable, default: howdy)\n");
    printf("  -a, --args \"ARGS\"   Arguments for ringlight\n");
    printf("  -v, --verbose       Verbose output\n");
    printf("  -h, --help          Show help\n");
}

int main(int argc, char *argv[]) {
    bool custom_procs = false;
    static struct option opts[] = {
        {"device", 1, 0, 'd'}, {"interval", 1, 0, 'i'}, {"process", 1, 0, 'p'},
        {"args", 1, 0, 'a'}, {"verbose", 0, 0, 'v'}, {"help", 0, 0, 'h'}, {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "d:i:p:a:vh", opts, NULL)) != -1) {
        switch (opt) {
        case 'd': strncpy(video_device, optarg, sizeof(video_device)-1); break;
        case 'i': poll_interval_ms = atoi(optarg); if (poll_interval_ms < 10) poll_interval_ms = 10; break;
        case 'p':
            if (!custom_procs) { watch_count = 0; custom_procs = true; }
            if (watch_count < MAX_PROCESSES-1) { watch_processes[watch_count++] = strdup(optarg); watch_processes[watch_count] = NULL; }
            break;
        case 'a': strncpy(ringlight_args, optarg, sizeof(ringlight_args)-1); break;
        case 'v': verbose = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 1;
        }
    }
    if (access(video_device, F_OK) != 0) fprintf(stderr, "Warning: %s not found\n", video_device);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, SIG_IGN);
    atexit(cleanup);
    log_msg("Monitoring %s, poll %dms", video_device, poll_interval_ms);
    bool was_active = false;
    while (running) {
        bool is_active = webcam_active();
        if (is_active && !was_active) start_ringlight();
        else if (!is_active && was_active) stop_ringlight();
        was_active = is_active;
        usleep(poll_interval_ms * 1000);
    }
    return 0;
}

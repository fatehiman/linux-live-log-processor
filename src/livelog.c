/*
 * livelog - run a command, read its output live, and save only the
 * interesting chunks into an encrypted daily log file.
 *
 * Logic:
 *   - A line that starts with event_prefix begins a new chunk.
 *   - If that line contains the match texts, logging is ON: a timestamp
 *     line (yyyymmddhhmmss) and the line itself are written, and all next
 *     lines are written too.
 *   - If it does not contain them, logging is OFF until the next chunk.
 *   - If the command stops, wait restart_delay seconds and run it again.
 *   - Stop only with SIGTERM / SIGINT (systemctl stop).
 *
 * Usage: livelog [-c config-file] [-s]
 *   -c  config file (default: livelog.conf next to the program)
 *   -s  read lines from stdin once instead of running the command (for tests)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rc4.h"

#define LIVELOG_VERSION "1.0.0"
#define MAX_MATCH       64
#define MAX_LINE        (1024 * 1024)

struct config {
    char command[4096];
    char event_prefix[256];
    char *match[MAX_MATCH];
    int nmatch;
    int match_all;
    char key[LL_MAX_KEY + 1];
    char log_dir[1024];
    char file_prefix[64];
    int keep_files;
    int restart_delay;
    int use_pty;
    int strip_ansi;
};

static struct config cfg;
static volatile sig_atomic_t g_stop = 0;

static FILE *g_log = NULL;
static char g_logdate[9] = "";
static int g_rand_fd = -1;
static unsigned long long g_counter = 0;

static int g_logging = 0;
static char g_line[MAX_LINE + 1];
static size_t g_len = 0;
static unsigned char g_rec[MAX_LINE + LL_CHECK_LEN + 2];

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    fputs("livelog: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------- config ---------- */

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static int parse_bool(const char *v)
{
    return !strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
           !strcasecmp(v, "on") || !strcmp(v, "1");
}

static void setstr(char *dst, size_t size, const char *v, const char *name)
{
    if (strlen(v) >= size)
        logmsg("config: value of '%s' is too long, it was cut", name);
    snprintf(dst, size, "%s", v);
}

static int load_config(const char *path)
{
    FILE *f;
    char buf[8192], *p, *eq, *k, *v;
    size_t vl;
    int ln = 0;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.log_dir, sizeof(cfg.log_dir), "/var/log/livelog");
    snprintf(cfg.file_prefix, sizeof(cfg.file_prefix), "ast-");
    cfg.keep_files = 20;
    cfg.restart_delay = 5;
    cfg.use_pty = 1;
    cfg.strip_ansi = 1;

    f = fopen(path, "r");
    if (!f) {
        logmsg("cannot open config file %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(buf, sizeof(buf), f)) {
        ln++;
        p = trim(buf);
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;
        eq = strchr(p, '=');
        if (!eq) {
            logmsg("config %s:%d: missing '=', line ignored", path, ln);
            continue;
        }
        *eq = '\0';
        k = trim(p);
        v = trim(eq + 1);
        vl = strlen(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
            v[vl - 1] = '\0';
            v++;
        }

        if (!strcmp(k, "command"))
            setstr(cfg.command, sizeof(cfg.command), v, k);
        else if (!strcmp(k, "event_prefix"))
            setstr(cfg.event_prefix, sizeof(cfg.event_prefix), v, k);
        else if (!strcmp(k, "match")) {
            if (cfg.nmatch >= MAX_MATCH)
                logmsg("config %s:%d: too many match lines (max %d)", path, ln, MAX_MATCH);
            else if (*v)
                cfg.match[cfg.nmatch++] = strdup(v);
        } else if (!strcmp(k, "match_mode")) {
            if (!strcasecmp(v, "all"))
                cfg.match_all = 1;
            else if (!strcasecmp(v, "any"))
                cfg.match_all = 0;
            else
                logmsg("config %s:%d: match_mode must be 'any' or 'all'", path, ln);
        } else if (!strcmp(k, "key"))
            setstr(cfg.key, sizeof(cfg.key), v, k);
        else if (!strcmp(k, "log_dir"))
            setstr(cfg.log_dir, sizeof(cfg.log_dir), v, k);
        else if (!strcmp(k, "file_prefix"))
            setstr(cfg.file_prefix, sizeof(cfg.file_prefix), v, k);
        else if (!strcmp(k, "keep_files"))
            cfg.keep_files = atoi(v);
        else if (!strcmp(k, "restart_delay"))
            cfg.restart_delay = atoi(v);
        else if (!strcmp(k, "use_pty"))
            cfg.use_pty = parse_bool(v);
        else if (!strcmp(k, "strip_ansi"))
            cfg.strip_ansi = parse_bool(v);
        else
            logmsg("config %s:%d: unknown key '%s', ignored", path, ln, k);
    }
    fclose(f);

    if (!cfg.command[0]) {
        logmsg("config: 'command' is empty");
        return -1;
    }
    if (!cfg.event_prefix[0]) {
        logmsg("config: 'event_prefix' is empty");
        return -1;
    }
    if (!cfg.key[0] || !strcmp(cfg.key, "change-me-please")) {
        logmsg("config: set your own 'key' first");
        return -1;
    }
    if (cfg.restart_delay < 1)
        cfg.restart_delay = 1;
    return 0;
}

/* ---------- log files ---------- */

static void mkdir_p(const char *path)
{
    char tmp[PATH_MAX], *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

/* prefix + 8 digits + ".log" */
static int is_log_name(const char *name)
{
    size_t pl = strlen(cfg.file_prefix);
    int i;

    if (strncmp(name, cfg.file_prefix, pl) != 0)
        return 0;
    name += pl;
    for (i = 0; i < 8; i++)
        if (!isdigit((unsigned char)name[i]))
            return 0;
    return strcmp(name + 8, ".log") == 0;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Keep only the newest keep_files log files. Names sort by date. */
static void cleanup_old_files(void)
{
    DIR *d;
    struct dirent *e;
    char **names = NULL, **tmp, path[PATH_MAX];
    size_t n = 0, cap = 0, i;

    if (cfg.keep_files <= 0)
        return;
    d = opendir(cfg.log_dir);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (!is_log_name(e->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            tmp = realloc(names, cap * sizeof(*names));
            if (!tmp)
                break;
            names = tmp;
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);

    if (n > (size_t)cfg.keep_files) {
        qsort(names, n, sizeof(*names), cmp_names);
        for (i = 0; i < n - (size_t)cfg.keep_files; i++) {
            snprintf(path, sizeof(path), "%s/%s", cfg.log_dir, names[i]);
            if (unlink(path) == 0)
                logmsg("removed old log file %s", path);
            else
                logmsg("cannot remove %s: %s", path, strerror(errno));
        }
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static int open_log(const char *date)
{
    char path[PATH_MAX];
    struct stat st;

    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    mkdir_p(cfg.log_dir);
    snprintf(path, sizeof(path), "%s/%s%s.log", cfg.log_dir, cfg.file_prefix, date);
    g_log = fopen(path, "abe");
    if (!g_log) {
        logmsg("cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fileno(g_log), &st) == 0 && st.st_size == 0) {
        fwrite(LL_FILE_MAGIC, 1, LL_MAGIC_LEN, g_log);
        fflush(g_log);
    }
    snprintf(g_logdate, sizeof(g_logdate), "%s", date);
    logmsg("writing to %s", path);
    cleanup_old_files();
    return 0;
}

static void get_nonce(unsigned char *nonce)
{
    struct timespec ts;
    unsigned long long v;

    if (g_rand_fd >= 0 && read(g_rand_fd, nonce, LL_NONCE_LEN) == LL_NONCE_LEN)
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    v = ((unsigned long long)ts.tv_sec << 30) ^ (unsigned long long)ts.tv_nsec ^
        (++g_counter * 0x9E3779B97F4A7C15ULL) ^ ((unsigned long long)getpid() << 40);
    memcpy(nonce, &v, LL_NONCE_LEN);
}

/* Encrypt one line and append it to the log file of that day. */
static void write_record(const char *text, size_t len, const struct tm *tm)
{
    char date[9];
    unsigned char hdr[LL_HDR_LEN];
    size_t rlen = LL_CHECK_LEN + len + 1;
    rc4_t st;

    strftime(date, sizeof(date), "%Y%m%d", tm);
    if (!g_log || strcmp(date, g_logdate) != 0)
        if (open_log(date) < 0)
            return;

    memcpy(g_rec, LL_CHECK, LL_CHECK_LEN);
    memcpy(g_rec + LL_CHECK_LEN, text, len);
    g_rec[rlen - 1] = '\n';

    get_nonce(hdr);
    hdr[8] = (unsigned char)(rlen >> 24);
    hdr[9] = (unsigned char)(rlen >> 16);
    hdr[10] = (unsigned char)(rlen >> 8);
    hdr[11] = (unsigned char)rlen;
    rc4_init(&st, hdr, (const unsigned char *)cfg.key, strlen(cfg.key));
    rc4_xor(&st, g_rec, rlen);

    if (fwrite(hdr, 1, LL_HDR_LEN, g_log) != LL_HDR_LEN ||
        fwrite(g_rec, 1, rlen, g_log) != rlen || fflush(g_log) != 0) {
        logmsg("write error on log file: %s", strerror(errno));
        fclose(g_log);
        g_log = NULL;   /* try to open it again on the next line */
    }
}

/* ---------- line processing ---------- */

/* Remove ANSI color/escape codes and other control characters. */
static size_t strip_ctrl(char *s, size_t n)
{
    size_t i = 0, o = 0;
    unsigned char c;

    while (i < n) {
        c = (unsigned char)s[i];
        if (c == 0x1b) {
            i++;
            if (i < n && s[i] == '[') {            /* CSI: ESC [ ... final byte */
                i++;
                while (i < n && !(s[i] >= 0x40 && s[i] <= 0x7e))
                    i++;
                i++;
            } else if (i < n && s[i] == ']') {     /* OSC: ESC ] ... BEL or ESC \ */
                i++;
                while (i < n && s[i] != 0x07 && s[i] != 0x1b)
                    i++;
                i += (i < n && s[i] == 0x1b) ? 2 : 1;
            } else {
                i++;
            }
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            i++;
            continue;
        }
        s[o++] = (char)c;
        i++;
    }
    return o;
}

static int line_matches(const char *line)
{
    int i;

    for (i = 0; i < cfg.nmatch; i++) {
        int found = strstr(line, cfg.match[i]) != NULL;
        if (found && !cfg.match_all)
            return 1;
        if (!found && cfg.match_all)
            return 0;
    }
    return cfg.nmatch == 0 || cfg.match_all;
}

static void handle_line(char *line, size_t len)
{
    size_t plen = strlen(cfg.event_prefix);
    time_t now;
    struct tm tm;
    char ts[16];

    if (cfg.strip_ansi)
        len = strip_ctrl(line, len);
    line[len] = '\0';

    now = time(NULL);
    localtime_r(&now, &tm);

    if (len >= plen && memcmp(line, cfg.event_prefix, plen) == 0) {
        g_logging = line_matches(line);
        if (g_logging) {
            strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tm);
            write_record(ts, strlen(ts), &tm);
        }
    }
    if (g_logging)
        write_record(line, len, &tm);
}

static void feed(const char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            handle_line(g_line, g_len);
            g_len = 0;
        } else if (c == '\r') {
            /* terminals send "\r\n"; drop the \r */
        } else if (g_len < MAX_LINE) {
            g_line[g_len++] = c;
        }
    }
}

/* Read until EOF, error, or stop signal. */
static void read_loop(int fd)
{
    static char buf[65536];
    struct pollfd pfd;
    ssize_t n;
    int r;

    pfd.fd = fd;
    pfd.events = POLLIN;
    while (!g_stop) {
        r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            logmsg("poll error: %s", strerror(errno));
            return;
        }
        if (r == 0)
            continue;
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            if (errno != EIO)   /* EIO = pty closed, normal end */
                logmsg("read error: %s", strerror(errno));
            return;
        }
        if (n == 0)
            return;
        feed(buf, (size_t)n);
    }
}

/* ---------- child command ---------- */

static void child_exec(void)
{
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    setenv("TERM", "dumb", 1);
    execl("/bin/sh", "sh", "-c", cfg.command, (char *)NULL);
    _exit(127);
}

static pid_t start_child(int *outfd)
{
    pid_t pid;
    int p[2], devnull;

    if (cfg.use_pty) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = 50;
        ws.ws_col = 512;
        pid = forkpty(outfd, NULL, NULL, &ws);
        if (pid < 0) {
            logmsg("forkpty failed: %s", strerror(errno));
            return -1;
        }
        if (pid == 0)
            child_exec();
        return pid;
    }

    if (pipe(p) < 0) {
        logmsg("pipe failed: %s", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        logmsg("fork failed: %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        setsid();
        devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
            dup2(devnull, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        child_exec();
    }
    close(p[1]);
    *outfd = p[0];
    return pid;
}

/* Stop the command and all its children (it runs in its own session). */
static int stop_child(pid_t pid)
{
    int st = 0, i;

    if (waitpid(pid, &st, WNOHANG) == pid)
        return st;
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    for (i = 0; i < 30; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            return st;
        usleep(100000);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return st;
}

static void sleep_seconds(int sec)
{
    while (sec-- > 0 && !g_stop)
        sleep(1);
}

/* ---------- main ---------- */

static void default_config_path(char *out, size_t size)
{
    char exe[PATH_MAX - 16], *slash;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

    if (n <= 0) {
        snprintf(out, size, "livelog.conf");
        return;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';
    snprintf(out, size, "%s/livelog.conf", exe);
}

int main(int argc, char **argv)
{
    char conf[PATH_MAX];
    const char *conf_arg = NULL;
    int use_stdin = 0, opt, fd, st;
    struct sigaction sa;
    pid_t pid;

    while ((opt = getopt(argc, argv, "c:svh")) != -1) {
        switch (opt) {
        case 'c':
            conf_arg = optarg;
            break;
        case 's':
            use_stdin = 1;
            break;
        case 'v':
            printf("livelog %s\n", LIVELOG_VERSION);
            return 0;
        default:
            fprintf(stderr, "usage: %s [-c config-file] [-s]\n"
                            "  -c  config file (default: livelog.conf next to the program)\n"
                            "  -s  read stdin once instead of running the command (for tests)\n",
                    argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (conf_arg)
        snprintf(conf, sizeof(conf), "%s", conf_arg);
    else
        default_config_path(conf, sizeof(conf));
    if (load_config(conf) < 0)
        return 1;

    umask(077);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;   /* no SA_RESTART: blocking calls return EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_rand_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    mkdir_p(cfg.log_dir);
    cleanup_old_files();

    logmsg("started (version %s, config %s, prefix '%s', %d match text(s), mode %s)",
           LIVELOG_VERSION, conf, cfg.event_prefix, cfg.nmatch,
           cfg.match_all ? "all" : "any");

    if (use_stdin) {
        read_loop(0);
        if (g_len > 0)
            handle_line(g_line, g_len);
    } else {
        while (!g_stop) {
            logmsg("running: %s", cfg.command);
            pid = start_child(&fd);
            if (pid > 0) {
                read_loop(fd);
                close(fd);
                st = stop_child(pid);
                if (!g_stop) {
                    if (WIFEXITED(st))
                        logmsg("command ended (exit code %d)", WEXITSTATUS(st));
                    else if (WIFSIGNALED(st))
                        logmsg("command ended (signal %d)", WTERMSIG(st));
                }
            }
            /* a cut line from the old run must not mix with the new run */
            g_len = 0;
            g_logging = 0;
            if (!g_stop) {
                logmsg("waiting %d s, then running the command again", cfg.restart_delay);
                sleep_seconds(cfg.restart_delay);
            }
        }
    }

    if (g_log)
        fclose(g_log);
    if (g_rand_fd >= 0)
        close(g_rand_fd);
    logmsg("stopped");
    return 0;
}

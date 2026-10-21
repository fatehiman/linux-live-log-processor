/*
 * ftp.h - a very small FTP client for livelog (plain FTP, passive mode).
 *
 * No library is needed. It can do: login, change/create the target folder,
 * ask the size of a remote file (SIZE) and upload a file (STOR).
 * FTPS / SFTP are not supported.
 *
 * All sockets have a timeout, so a dead server can not block it for ever.
 * On an error a function returns -1 and f->err has the reason.
 * After an error, call ftp_close() and open a new session.
 */
#ifndef LIVELOG_FTP_H
#define LIVELOG_FTP_H

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

struct ftp {
    int ctrl;                           /* control socket, -1 = not connected */
    int timeout;                        /* seconds, for every socket operation */
    struct sockaddr_storage peer;       /* server address (used for data connections) */
    socklen_t peerlen;
    char buf[4096];                     /* bytes read from the control socket */
    size_t blen;
    char reply[512];                    /* last line of the last reply */
    char err[640];                      /* reason of the last error */
};

static void ftp_seterr(struct ftp *f, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(f->err, sizeof(f->err), fmt, ap);
    va_end(ap);
}

/* code < 0: the error text is already set. Else: the server said "no". */
static void ftp_fail(struct ftp *f, int code, const char *what)
{
    if (code >= 0)
        ftp_seterr(f, "%s: %s", what, f->reply);
}

static void ftp_init(struct ftp *f)
{
    memset(f, 0, sizeof(*f));
    f->ctrl = -1;
}

static void ftp_close(struct ftp *f)
{
    if (f->ctrl >= 0) {
        send(f->ctrl, "QUIT\r\n", 6, MSG_NOSIGNAL);
        close(f->ctrl);
        f->ctrl = -1;
    }
    f->blen = 0;
}

static int ftp_send_all(int fd, const char *p, size_t n)
{
    ssize_t w;

    while (n > 0) {
        w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Connect with a timeout (send/receive timeout also covers connect on Linux). */
static int ftp_connect_addr(const struct sockaddr *sa, socklen_t len, int timeout)
{
    struct timeval tv;
    int fd = socket(sa->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, sa, len) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* Read one line from the control socket (without the line end). */
static int ftp_readline(struct ftp *f, char *line, size_t size)
{
    char *nl;
    size_t n, c;
    ssize_t r;

    for (;;) {
        nl = memchr(f->buf, '\n', f->blen);
        if (nl) {
            n = (size_t)(nl - f->buf);
            c = (n && f->buf[n - 1] == '\r') ? n - 1 : n;
            if (c >= size)
                c = size - 1;
            memcpy(line, f->buf, c);
            line[c] = '\0';
            f->blen -= n + 1;
            memmove(f->buf, nl + 1, f->blen);
            return 0;
        }
        if (f->blen == sizeof(f->buf)) {
            ftp_seterr(f, "reply line is too long");
            return -1;
        }
        r = recv(f->ctrl, f->buf + f->blen, sizeof(f->buf) - f->blen, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            ftp_seterr(f, "no reply from server: %s",
                       (errno == EAGAIN || errno == EWOULDBLOCK) ? "timeout" : strerror(errno));
            return -1;
        }
        if (r == 0) {
            ftp_seterr(f, "connection closed by server");
            return -1;
        }
        f->blen += (size_t)r;
    }
}

/* Read a full reply (may have many lines). Returns the 3 digit code, or -1. */
static int ftp_reply(struct ftp *f)
{
    char line[512], codestr[8];
    int code;

    if (ftp_readline(f, line, sizeof(line)) < 0)
        return -1;
    if (!isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2])) {
        ftp_seterr(f, "bad reply from server: %.100s", line);
        return -1;
    }
    code = atoi(line);
    snprintf(codestr, sizeof(codestr), "%03d", code);
    if (line[3] == '-') {                       /* multi line: until "NNN " */
        do {
            if (ftp_readline(f, line, sizeof(line)) < 0)
                return -1;
        } while (strncmp(line, codestr, 3) != 0 || line[3] != ' ');
    }
    snprintf(f->reply, sizeof(f->reply), "%s", line);
    return code;
}

/* Send one command and read the reply. Returns the reply code, or -1. */
static int ftp_cmd(struct ftp *f, const char *fmt, ...)
{
    char cmd[1200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof(cmd) - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(cmd) - 2) {
        ftp_seterr(f, "command is too long");
        return -1;
    }
    if (strpbrk(cmd, "\r\n")) {
        ftp_seterr(f, "bad character in command");
        return -1;
    }
    memcpy(cmd + n, "\r\n", 2);
    if (ftp_send_all(f->ctrl, cmd, (size_t)n + 2) < 0) {
        ftp_seterr(f, "cannot send to server: %s", strerror(errno));
        return -1;
    }
    return ftp_reply(f);
}

/* Go into the target folder. A missing folder is created (one level at a time). */
static int ftp_cd(struct ftp *f, const char *path)
{
    char tmp[1024], *tok, *save = NULL;
    int code;

    if (!path || !*path)
        return 0;
    snprintf(tmp, sizeof(tmp), "%s", path);
    if (tmp[0] == '/') {
        code = ftp_cmd(f, "CWD /");
        if (code != 250) {
            ftp_fail(f, code, "CWD /");
            return -1;
        }
    }
    for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        code = ftp_cmd(f, "CWD %s", tok);
        if (code != 250) {
            if (code < 0)
                return -1;
            code = ftp_cmd(f, "MKD %s", tok);
            if (code != 257) {
                ftp_fail(f, code, "cannot create folder");
                return -1;
            }
            code = ftp_cmd(f, "CWD %s", tok);
            if (code != 250) {
                ftp_fail(f, code, "CWD");
                return -1;
            }
        }
    }
    return 0;
}

/* Connect and log in, set binary mode, go into the target folder. */
static int ftp_open(struct ftp *f, const char *host, int port, const char *user,
                    const char *pass, const char *path, int timeout)
{
    struct addrinfo hints, *res, *ai;
    char ps[16];
    int rc, code, e = 0;

    ftp_close(f);
    f->timeout = timeout;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    rc = getaddrinfo(host, ps, &hints, &res);
    if (rc != 0) {
        ftp_seterr(f, "cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        f->ctrl = ftp_connect_addr(ai->ai_addr, ai->ai_addrlen, timeout);
        if (f->ctrl >= 0) {
            memcpy(&f->peer, ai->ai_addr, ai->ai_addrlen);
            f->peerlen = ai->ai_addrlen;
            break;
        }
        e = errno;
    }
    freeaddrinfo(res);
    if (f->ctrl < 0) {
        ftp_seterr(f, "cannot connect to %s:%d: %s", host, port,
                   (e == EAGAIN || e == EINPROGRESS) ? "timeout" : strerror(e));
        return -1;
    }

    code = ftp_reply(f);
    if (code != 220) {
        ftp_fail(f, code, "server greeting");
        goto fail;
    }
    code = ftp_cmd(f, "USER %s", user);
    if (code == 331 || code == 332)
        code = ftp_cmd(f, "PASS %s", pass);
    if (code != 230) {
        ftp_fail(f, code, "login failed");
        goto fail;
    }
    code = ftp_cmd(f, "TYPE I");
    if (code != 200) {
        ftp_fail(f, code, "TYPE I");
        goto fail;
    }
    if (ftp_cd(f, path) < 0)
        goto fail;
    return 0;
fail:
    ftp_close(f);
    return -1;
}

/* Remote file size, or -1 if the file is not there (or SIZE is not supported). */
static long long ftp_size(struct ftp *f, const char *name)
{
    if (ftp_cmd(f, "SIZE %s", name) == 213)
        return atoll(f->reply + 4);
    return -1;
}

/* Open a passive data connection. EPSV first, then PASV. Returns the socket or -1. */
static int ftp_data_connect(struct ftp *f)
{
    struct sockaddr_storage sa = f->peer;
    char *p;
    int code, port = 0, a[6], fd;

    code = ftp_cmd(f, "EPSV");
    if (code == 229 && (p = strchr(f->reply, '(')) != NULL &&
        p[1] && p[2] == p[1] && p[3] == p[1])
        port = atoi(p + 4);
    else {
        if (code < 0)
            return -1;
        code = ftp_cmd(f, "PASV");
        if (code == 227 && (p = strchr(f->reply, '(')) != NULL &&
            sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6)
            port = a[4] * 256 + a[5];   /* the address in the reply is ignored: use the server's */
        else {
            ftp_fail(f, code, "passive mode");
            return -1;
        }
    }
    if (port < 1 || port > 65535) {
        ftp_seterr(f, "bad data port from server");
        return -1;
    }
    if (sa.ss_family == AF_INET)
        ((struct sockaddr_in *)&sa)->sin_port = htons((unsigned short)port);
    else
        ((struct sockaddr_in6 *)&sa)->sin6_port = htons((unsigned short)port);
    fd = ftp_connect_addr((struct sockaddr *)&sa, f->peerlen, f->timeout);
    if (fd < 0) {
        ftp_seterr(f, "cannot open data connection: %s",
                   (errno == EAGAIN || errno == EINPROGRESS) ? "timeout" : strerror(errno));
        return -1;
    }
    return fd;
}

/* Upload the file in descriptor "in" (from its current position) as "name". */
static int ftp_store_fd(struct ftp *f, int in, const char *name)
{
    char buf[65536];
    ssize_t n;
    int ds, code;

    ds = ftp_data_connect(f);
    if (ds < 0)
        return -1;
    code = ftp_cmd(f, "STOR %s", name);
    if (code != 150 && code != 125) {
        ftp_fail(f, code, "STOR");
        close(ds);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (ftp_send_all(ds, buf, (size_t)n) < 0) {
            ftp_seterr(f, "data send error: %s",
                       (errno == EAGAIN || errno == EWOULDBLOCK) ? "timeout" : strerror(errno));
            close(ds);
            return -1;
        }
    }
    close(ds);
    if (n < 0) {
        ftp_seterr(f, "cannot read local file: %s", strerror(errno));
        return -1;
    }
    code = ftp_reply(f);
    if (code != 226 && code != 250) {
        ftp_fail(f, code, "upload");
        return -1;
    }
    return 0;
}

#endif

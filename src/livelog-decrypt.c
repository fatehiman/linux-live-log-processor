/*
 * livelog-decrypt - decrypt livelog files (ast-yyyymmdd.log).
 *
 * Usage:
 *   livelog-decrypt <file> <key> [output-file]
 *       Decrypt one file. Without output-file the plain text goes to stdout.
 *   livelog-decrypt
 *       No arguments: read the key from livelog.conf next to this program,
 *       and decrypt every *.log file in the folder of this program that is
 *       still encrypted. The original is kept as <name>.log.bak and the
 *       plain text is saved as <name>.log. Files that are already decrypted
 *       are skipped.
 *
 * Plain C, no extra libraries. Builds on Linux and on Windows (MinGW).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "rc4.h"

#define PATHLEN 4096

/* Decrypt all records. Returns 0 = ok, 1 = damaged file, 2 = wrong key. */
static int decode_stream(FILE *in, FILE *out, const unsigned char *key, size_t keylen,
                         unsigned long *records)
{
    unsigned char hdr[LL_HDR_LEN];
    unsigned char *buf = NULL, *tmp;
    unsigned long len;
    size_t got;
    rc4_t st;
    int rc = 0;

    *records = 0;
    for (;;) {
        got = fread(hdr, 1, LL_HDR_LEN, in);
        if (got == 0)
            break;
        if (got < LL_HDR_LEN) {
            fprintf(stderr, "warning: last record is cut (file was not closed cleanly)\n");
            break;
        }
        len = ((unsigned long)hdr[8] << 24) | ((unsigned long)hdr[9] << 16) |
              ((unsigned long)hdr[10] << 8) | (unsigned long)hdr[11];
        if (len < LL_CHECK_LEN + 1 || len > LL_MAX_RECORD) {
            fprintf(stderr, "error: file is damaged (bad record length)\n");
            rc = 1;
            break;
        }
        tmp = realloc(buf, len);
        if (!tmp) {
            fprintf(stderr, "error: out of memory\n");
            rc = 1;
            break;
        }
        buf = tmp;
        if (fread(buf, 1, len, in) != len) {
            fprintf(stderr, "warning: last record is cut (file was not closed cleanly)\n");
            break;
        }
        rc4_init(&st, hdr, key, keylen);
        rc4_xor(&st, buf, len);
        if (memcmp(buf, LL_CHECK, LL_CHECK_LEN) != 0) {
            fprintf(stderr, "error: wrong key\n");
            rc = 2;
            break;
        }
        fwrite(buf + LL_CHECK_LEN, 1, len - LL_CHECK_LEN, out);
        (*records)++;
    }
    free(buf);
    return rc;
}

/* 1 if the file starts with the livelog magic (= still encrypted). */
static int is_encrypted(const char *path)
{
    unsigned char magic[LL_MAGIC_LEN];
    FILE *f = fopen(path, "rb");
    int r;

    if (!f)
        return 0;
    r = fread(magic, 1, LL_MAGIC_LEN, f) == LL_MAGIC_LEN &&
        memcmp(magic, LL_FILE_MAGIC, LL_MAGIC_LEN) == 0;
    fclose(f);
    return r;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* ---------- no arguments: decrypt the folder ---------- */

/* Folder of this program (without a trailing slash). */
static void exe_dir(char *out, size_t size, const char *argv0)
{
    char *slash;
    ssize_t n = -1;

#ifndef _WIN32
    n = readlink("/proc/self/exe", out, size - 1);
#endif
    if (n > 0)
        out[n] = '\0';
    else
        snprintf(out, size, "%s", argv0);
    slash = strrchr(out, '/');
#ifdef _WIN32
    if (!slash || (strrchr(out, '\\') && strrchr(out, '\\') > slash))
        slash = strrchr(out, '\\');
#endif
    if (slash)
        *slash = '\0';
    else
        snprintf(out, size, ".");
}

/* Read "key = value" from the config file (the last one wins). */
static int read_key(const char *path, char *key, size_t size)
{
    char line[8192], *p, *eq, *e;
    size_t vl;
    int found = 0;
    FILE *f = fopen(path, "r");

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '#' || *p == ';' || !(eq = strchr(p, '=')))
            continue;
        e = eq;
        while (e > p && isspace((unsigned char)e[-1]))
            e--;
        if (e - p != 3 || strncmp(p, "key", 3) != 0)
            continue;
        p = eq + 1;
        while (isspace((unsigned char)*p))
            p++;
        e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1]))
            *--e = '\0';
        vl = strlen(p);
        if (vl >= 2 && p[0] == '"' && p[vl - 1] == '"') {
            p[vl - 1] = '\0';
            p++;
        }
        snprintf(key, size, "%s", p);
        found = 1;
    }
    fclose(f);
    return found && key[0] ? 0 : -1;
}

/* Decrypt one file in place. The original is kept as <path>.bak. Returns 0 or -1. */
static int decode_in_place(const char *path, const char *key)
{
    char tmp[PATHLEN * 2 + 8], bak[PATHLEN * 2 + 8];
    FILE *in, *out;
    unsigned long records;
    int rc;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (file_exists(bak)) {
        fprintf(stderr, "%s: skipped, %s already exists\n", path, bak);
        return -1;
    }
    in = fopen(path, "rb");
    if (!in) {
        perror(path);
        return -1;
    }
    fseek(in, LL_MAGIC_LEN, SEEK_SET);
    out = fopen(tmp, "wb");
    if (!out) {
        perror(tmp);
        fclose(in);
        return -1;
    }
    rc = decode_stream(in, out, (const unsigned char *)key, strlen(key), &records);
    fclose(in);
    if (fclose(out) != 0 && rc == 0) {
        fprintf(stderr, "error: cannot write %s\n", tmp);
        rc = 1;
    }
    if (rc != 0) {
        remove(tmp);
        fprintf(stderr, "%s: NOT decrypted (file is unchanged)\n", path);
        return -1;
    }
    if (rename(path, bak) != 0 || rename(tmp, path) != 0) {
        perror(path);
        remove(tmp);
        return -1;
    }
    printf("%s: decrypted, %lu lines (original kept as %s)\n", path, records, bak);
    return 0;
}

static int decode_folder(const char *argv0)
{
    char dir[PATHLEN], conf[PATHLEN + 16], path[PATHLEN * 2], key[LL_MAX_KEY + 1] = "";
    DIR *d;
    struct dirent *e;
    size_t l;
    int done = 0, skipped = 0, failed = 0;

    exe_dir(dir, sizeof(dir), argv0);
    snprintf(conf, sizeof(conf), "%s/livelog.conf", dir);
    if (read_key(conf, key, sizeof(key)) < 0) {
        fprintf(stderr, "No arguments, and no key found in %s\n"
                        "usage: %s <file> <key> [output-file]\n"
                        "   or: %s   (with livelog.conf next to the program)\n",
                conf, argv0, argv0);
        return 1;
    }
    d = opendir(dir);
    if (!d) {
        perror(dir);
        return 1;
    }
    while ((e = readdir(d)) != NULL) {
        l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".log") != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (!is_encrypted(path)) {
            skipped++;
            continue;
        }
        if (decode_in_place(path, key) == 0)
            done++;
        else
            failed++;
    }
    closedir(d);
    printf("%s: %d decrypted, %d already decrypted, %d failed\n", dir, done, skipped, failed);
    return failed ? 1 : 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    FILE *in, *out = stdout;
    unsigned char magic[LL_MAGIC_LEN];
    unsigned long records;
    int rc;

    if (argc == 1)
        return decode_folder(argv[0]);
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <file> <key> [output-file]\n"
                        "   or: %s   (decrypt all *.log files next to the program, key from livelog.conf)\n",
                argv[0], argv[0]);
        return 1;
    }

    in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    if (fread(magic, 1, LL_MAGIC_LEN, in) != LL_MAGIC_LEN ||
        memcmp(magic, LL_FILE_MAGIC, LL_MAGIC_LEN) != 0) {
        fprintf(stderr, "error: %s is not a livelog file\n", argv[1]);
        fclose(in);
        return 1;
    }
    if (argc == 4) {
        out = fopen(argv[3], "wb");
        if (!out) {
            perror(argv[3]);
            fclose(in);
            return 1;
        }
    }

    rc = decode_stream(in, out, (const unsigned char *)argv[2], strlen(argv[2]), &records);

    fclose(in);
    if (out != stdout)
        fclose(out);
    else
        fflush(stdout);
    if (rc == 0 && out != stdout)
        fprintf(stderr, "%lu lines written to %s\n", records, argv[3]);
    return rc;
}

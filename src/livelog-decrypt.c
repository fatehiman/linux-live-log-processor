/*
 * livelog-decrypt - decrypt a livelog file (ast-yyyymmdd.log).
 *
 * Usage: livelog-decrypt <file> <key> [output-file]
 * Without output-file the plain text goes to stdout.
 *
 * Plain C, no extra libraries. Builds on Linux and on Windows (MinGW).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc4.h"

int main(int argc, char **argv)
{
    FILE *in, *out = stdout;
    unsigned char magic[LL_MAGIC_LEN], hdr[LL_HDR_LEN];
    unsigned char *buf = NULL;
    const unsigned char *key;
    size_t keylen, got;
    unsigned long len, records = 0;
    rc4_t st;
    int rc = 0;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <file> <key> [output-file]\n", argv[0]);
        return 1;
    }
    key = (const unsigned char *)argv[2];
    keylen = strlen(argv[2]);

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
        buf = realloc(buf, len);
        if (!buf) {
            fprintf(stderr, "error: out of memory\n");
            rc = 1;
            break;
        }
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
        records++;
    }

    free(buf);
    fclose(in);
    if (out != stdout)
        fclose(out);
    else
        fflush(stdout);
    if (rc == 0 && out != stdout)
        fprintf(stderr, "%lu lines written to %s\n", records, argv[3]);
    return rc;
}

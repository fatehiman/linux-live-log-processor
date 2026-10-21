/*
 * rc4.h - small shared cipher code for livelog and livelog-decrypt.
 *
 * This is simple encryption (RC4-drop1024 with a random nonce per record).
 * Its goal is only to keep the log files from being plain text.
 * It is not strong, modern encryption.
 *
 * File format (see reference-docs/file-format.md):
 *   "LLP1"                         4 bytes, once at the start of the file
 *   then records, one per line:
 *     nonce                        8 bytes, random
 *     length                       4 bytes, big endian, length of the data below
 *     data                         encrypted ("LL" + line text + "\n")
 *
 * RC4 key = nonce (8 bytes) + user key (max 248 bytes).
 * The first 1024 bytes of the RC4 key stream are thrown away.
 */
#ifndef LIVELOG_RC4_H
#define LIVELOG_RC4_H

#include <stddef.h>
#include <string.h>

#define LL_FILE_MAGIC   "LLP1"
#define LL_MAGIC_LEN    4
#define LL_NONCE_LEN    8
#define LL_HDR_LEN      12      /* nonce + length */
#define LL_CHECK        "LL"    /* first 2 plain bytes of each record, to detect a wrong key */
#define LL_CHECK_LEN    2
#define LL_MAX_KEY      248
#define LL_DROP         1024
#define LL_MAX_RECORD   (16u * 1024u * 1024u)

typedef struct {
    unsigned char s[256];
    unsigned char i, j;
} rc4_t;

static unsigned char rc4_next(rc4_t *st)
{
    unsigned char t;
    st->i = (unsigned char)(st->i + 1);
    st->j = (unsigned char)(st->j + st->s[st->i]);
    t = st->s[st->i];
    st->s[st->i] = st->s[st->j];
    st->s[st->j] = t;
    return st->s[(unsigned char)(st->s[st->i] + st->s[st->j])];
}

static void rc4_init(rc4_t *st, const unsigned char *nonce,
                     const unsigned char *key, size_t keylen)
{
    unsigned char k[LL_NONCE_LEN + LL_MAX_KEY];
    size_t klen, n;
    unsigned char j = 0, t;
    int i;

    if (keylen > LL_MAX_KEY)
        keylen = LL_MAX_KEY;
    memcpy(k, nonce, LL_NONCE_LEN);
    memcpy(k + LL_NONCE_LEN, key, keylen);
    klen = LL_NONCE_LEN + keylen;

    for (i = 0; i < 256; i++)
        st->s[i] = (unsigned char)i;
    for (i = 0; i < 256; i++) {
        j = (unsigned char)(j + st->s[i] + k[(size_t)i % klen]);
        t = st->s[i];
        st->s[i] = st->s[j];
        st->s[j] = t;
    }
    st->i = st->j = 0;
    for (n = 0; n < LL_DROP; n++)
        rc4_next(st);
}

static void rc4_xor(rc4_t *st, unsigned char *buf, size_t len)
{
    size_t n;
    for (n = 0; n < len; n++)
        buf[n] ^= rc4_next(st);
}

#endif

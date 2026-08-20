#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "hml.h"

struct Imap {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    unsigned tag;
    char *line; /* growable logical line buffer */
    size_t len, cap;
    char rd[16384]; /* raw read buffer */
    size_t rdlen, rdoff;
    FILE *sink; /* when set, BODY[] literals stream here, CRLF -> LF */
    int sinkcr; /* carry: last literal byte was CR */
};

static int readbyte(Imap *im) {
    if (im->rdoff == im->rdlen) {
        int n = SSL_read(im->ssl, im->rd, sizeof im->rd);
        if (n <= 0)
            return -1;
        im->rdlen = (size_t)n;
        im->rdoff = 0;
    }
    return (unsigned char)im->rd[im->rdoff++];
}

static int pushbyte(Imap *im, char c) {
    if (im->len + 2 > im->cap) {
        size_t ncap = im->cap ? im->cap * 2 : 4096;
        char *p = realloc(im->line, ncap);
        if (!p)
            return -1;
        im->line = p;
        im->cap = ncap;
    }
    im->line[im->len++] = c;
    return 0;
}

/* does the line end in an IMAP literal marker {N}? if so return N and the
 * offset of the '{' so it can be stripped */
static int literalat(const char *s, size_t len, size_t *n, size_t *at) {
    size_t i = len;

    if (!i || s[i - 1] != '}')
        return 0;
    for (i--; i && s[i - 1] >= '0' && s[i - 1] <= '9'; i--)
        ;
    if (!i || s[i - 1] != '{' || i == len - 1)
        return 0;
    *n = strtoul(s + i, NULL, 10);
    *at = i - 1;
    return 1;
}

/* consume a literal of n bytes; when it is message body and a sink is set,
 * stream it there with CRLF collapsed to LF (maildir stores bare LF) */
static int literalcopy(Imap *im, size_t n, int tosink) {
    while (n) {
        if (im->rdoff == im->rdlen) {
            int r = SSL_read(im->ssl, im->rd, sizeof im->rd);
            if (r <= 0)
                return -1;
            im->rdlen = (size_t)r;
            im->rdoff = 0;
        }
        size_t k = im->rdlen - im->rdoff;
        if (k > n)
            k = n;
        if (tosink) {
            const char *p = im->rd + im->rdoff, *end = p + k;
            while (p < end) {
                const char *cr;
                if (im->sinkcr) {
                    if (*p != '\n')
                        fputc('\r', im->sink);
                    im->sinkcr = 0;
                }
                if (!(cr = memchr(p, '\r', (size_t)(end - p)))) {
                    fwrite(p, 1, (size_t)(end - p), im->sink);
                    break;
                }
                fwrite(p, 1, (size_t)(cr - p), im->sink);
                im->sinkcr = 1;
                p = cr + 1;
            }
        }
        im->rdoff += k;
        n -= k;
    }
    return 0;
}

/* read one logical reply line; literal payloads go to the sink when they are
 * message body, and are skipped otherwise */
static char *readline(Imap *im, char *err, size_t errlen) {
    im->len = 0;
    for (;;) {
        int c = readbyte(im);
        if (c < 0) {
            snprintf(err, errlen, "connection lost");
            return NULL;
        }
        if (c != '\n') {
            if (pushbyte(im, (char)c) < 0) {
                snprintf(err, errlen, "out of memory");
                return NULL;
            }
            continue;
        }
        if (im->len && im->line[im->len - 1] == '\r')
            im->len--;
        size_t n, at;
        if (literalat(im->line, im->len, &n, &at)) {
            int tosink;
            im->len = at;
            im->line[im->len] = '\0';
            tosink = im->sink && strstr(im->line, "BODY[") != NULL;
            if (literalcopy(im, n, tosink) < 0) {
                snprintf(err, errlen, "connection lost");
                return NULL;
            }
            continue; /* rest of the logical line follows */
        }
        im->line[im->len] = '\0';
        return im->line;
    }
}

static int writeall(Imap *im, const char *s, size_t n) {
    while (n) {
        int w = SSL_write(im->ssl, s, n > 16384 ? 16384 : (int)n);
        if (w <= 0)
            return -1;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

Imap *tlsconnect(const char *host, int port, char *err, size_t errlen) {
    Imap *im;
    struct addrinfo hints, *res, *ai;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res)) {
        snprintf(err, errlen, "cannot resolve %s", host);
        return NULL;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(err, errlen, "cannot connect to %s:%d", host, port);
        return NULL;
    }
    struct timeval tv = {60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    if (!(im = calloc(1, sizeof *im))) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    im->fd = fd;
    im->ctx = SSL_CTX_new(TLS_client_method());
    if (!im->ctx)
        goto tlsfail;
    SSL_CTX_set_min_proto_version(im->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(im->ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(im->ctx);
    SSL_CTX_set_mode(im->ctx, SSL_MODE_AUTO_RETRY);
    if (!(im->ssl = SSL_new(im->ctx)))
        goto tlsfail;
    SSL_set_fd(im->ssl, fd);
    SSL_set_tlsext_host_name(im->ssl, host);
    SSL_set1_host(im->ssl, host);
    if (SSL_connect(im->ssl) != 1)
        goto tlsfail;
    return im;

tlsfail:
    snprintf(err, errlen, "TLS to %s failed: %s", host,
             ERR_reason_error_string(ERR_get_error()));
    imapclose(im);
    return NULL;
}

char *imapline(Imap *im, char *err, size_t errlen) {
    return readline(im, err, errlen);
}

int imapwrite(Imap *im, const char *s, size_t n) { return writeall(im, s, n); }

Imap *imapconnect(const char *host, int port, char *err, size_t errlen) {
    char tmp[8];
    Imap *im = tlsconnect(host, port, err, errlen);

    if (!im)
        return NULL;
    if (!readline(im, tmp, sizeof tmp) || strncmp(im->line, "* OK", 4) != 0) {
        snprintf(err, errlen, "bad IMAP greeting from %s", host);
        imapclose(im);
        return NULL;
    }
    return im;
}

void imapquote(char *dst, size_t cap, const char *s) {
    size_t i = 0;

    if (cap < 3) {
        if (cap)
            dst[0] = '\0';
        return;
    }
    dst[i++] = '"';
    for (; *s && i + 3 < cap; s++) {
        if (*s == '"' || *s == '\\')
            dst[i++] = '\\';
        dst[i++] = *s;
    }
    dst[i++] = '"';
    dst[i] = '\0';
}

int imapexec(Imap *im, Linefn fn, void *ud, char *err, size_t errlen,
             const char *fmt, ...) {
    char cmd[1024], tag[16];
    va_list ap;
    size_t taglen;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    taglen = (size_t)snprintf(tag, sizeof tag, "h%u", ++im->tag);
    if (writeall(im, tag, taglen) < 0 || writeall(im, " ", 1) < 0 ||
        writeall(im, cmd, strlen(cmd)) < 0 || writeall(im, "\r\n", 2) < 0) {
        snprintf(err, errlen, "connection lost");
        return -1;
    }
    for (;;) {
        char *l = readline(im, err, errlen);
        if (!l)
            return -1;
        if (!strncmp(l, tag, taglen) && l[taglen] == ' ') {
            if (!strncmp(l + taglen + 1, "OK", 2))
                return 0;
            snprintf(err, errlen, "%s", l + taglen + 1);
            return -1;
        }
        if (l[0] == '*' && fn)
            fn(l, ud);
    }
}

int imaplogin(Imap *im, const char *user, const char *pass, char *err,
              size_t errlen) {
    char qu[256], qp[256];
    int r;

    imapquote(qu, sizeof qu, user);
    imapquote(qp, sizeof qp, pass);
    r = imapexec(im, NULL, NULL, err, errlen, "LOGIN %s %s", qu, qp);
    memset(qp, 0, sizeof qp);
    return r;
}

unsigned imapflags(const char *s) {
    unsigned f = 0;

    if (strstr(s, "\\Seen"))
        f |= FSeen;
    if (strstr(s, "\\Answered"))
        f |= FAnswered;
    if (strstr(s, "\\Flagged"))
        f |= FFlagged;
    if (strstr(s, "\\Deleted"))
        f |= FDeleted;
    if (strstr(s, "\\Draft"))
        f |= FDraft;
    if (strstr(s, "$Forwarded"))
        f |= FPassed;
    return f;
}

void imapflagstr(unsigned f, char *out, size_t cap) {
    out[0] = '\0';
    if (f & FSeen)
        strncat(out, "\\Seen ", cap - strlen(out) - 1);
    if (f & FAnswered)
        strncat(out, "\\Answered ", cap - strlen(out) - 1);
    if (f & FFlagged)
        strncat(out, "\\Flagged ", cap - strlen(out) - 1);
    if (f & FDeleted)
        strncat(out, "\\Deleted ", cap - strlen(out) - 1);
    if (f & FDraft)
        strncat(out, "\\Draft ", cap - strlen(out) - 1);
    if (f & FPassed)
        strncat(out, "$Forwarded ", cap - strlen(out) - 1);
    if (out[0])
        out[strlen(out) - 1] = '\0'; /* trailing space */
}

typedef struct {
    uint32_t uid;
    unsigned flags;
    int got;
} Fetchone;

static void fetchonecb(const char *l, void *ud) {
    Fetchone *b = ud;
    char pat[24];
    const char *p, *fl;
    size_t plen;

    plen = (size_t)snprintf(pat, sizeof pat, "UID %u", b->uid);
    if (!(p = strstr(l, pat)) || (p[plen] >= '0' && p[plen] <= '9'))
        return;
    if ((fl = strstr(l, "FLAGS (")))
        b->flags = imapflags(fl);
    b->got = 1;
}

int imapfetchbody(Imap *im, uint32_t uid, FILE *out, unsigned *flags, char *err,
                  size_t errlen) {
    Fetchone b = {uid, 0, 0};
    int r;

    im->sink = out;
    im->sinkcr = 0;
    r = imapexec(im, fetchonecb, &b, err, errlen,
                 "UID FETCH %u (UID FLAGS BODY.PEEK[])", uid);
    if (im->sinkcr)
        fputc('\r', out); /* message ended on a bare CR */
    im->sink = NULL;
    if (r == 0 && !b.got) {
        snprintf(err, errlen, "uid %u vanished before fetch", uid);
        r = -1;
    }
    *flags = b.flags;
    return r;
}

int imapappendfile(Imap *im, const char *qbox, unsigned flags, FILE *src,
                   char *err, size_t errlen, uint32_t *uid) {
    char head[512], tag[16], flagstr[80], buf[8192], out[16384];
    long size = 0;
    size_t taglen, n, i, o;
    int c, prev = 0;
    char *l;

    /* IMAP wants CRLF line endings and an exact byte count up front */
    while ((c = fgetc(src)) != EOF) {
        size += (c == '\n' && prev != '\r') ? 2 : 1;
        prev = c;
    }
    rewind(src);

    imapflagstr(flags, flagstr, sizeof flagstr);
    taglen = (size_t)snprintf(tag, sizeof tag, "h%u", ++im->tag);
    snprintf(head, sizeof head, "%s APPEND %s (%s) {%ld}\r\n", tag, qbox,
             flagstr, size);
    if (writeall(im, head, strlen(head)) < 0) {
        snprintf(err, errlen, "connection lost");
        return -1;
    }
    for (;;) { /* wait for the go-ahead */
        if (!(l = readline(im, err, errlen)))
            return -1;
        if (l[0] == '+')
            break;
        if (!strncmp(l, tag, taglen) && l[taglen] == ' ') {
            snprintf(err, errlen, "%s", l + taglen + 1);
            return -1;
        }
    }
    prev = 0;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        for (i = o = 0; i < n; i++) {
            if (buf[i] == '\n' && prev != '\r')
                out[o++] = '\r';
            out[o++] = buf[i];
            prev = buf[i];
        }
        if (writeall(im, out, o) < 0) {
            snprintf(err, errlen, "connection lost");
            return -1;
        }
    }
    if (writeall(im, "\r\n", 2) < 0) {
        snprintf(err, errlen, "connection lost");
        return -1;
    }
    for (;;) {
        if (!(l = readline(im, err, errlen)))
            return -1;
        if (!strncmp(l, tag, taglen) && l[taglen] == ' ') {
            const char *p;
            if (strncmp(l + taglen + 1, "OK", 2) != 0) {
                snprintf(err, errlen, "%s", l + taglen + 1);
                return -1;
            }
            uint32_t uv, u;
            if (!(p = strstr(l, "APPENDUID")) ||
                sscanf(p, "APPENDUID %u %u", &uv, &u) != 2) {
                snprintf(err, errlen, "no APPENDUID in append reply");
                return -1;
            }
            *uid = u;
            return 0;
        }
    }
}

void imapclose(Imap *im) {
    if (!im)
        return;
    if (im->ssl) {
        SSL_shutdown(im->ssl);
        SSL_free(im->ssl);
    }
    if (im->ctx)
        SSL_CTX_free(im->ctx);
    if (im->fd >= 0)
        close(im->fd);
    free(im->line);
    free(im);
}

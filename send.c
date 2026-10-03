/* send.c - "hml send": SMTP submission with a sendmail-shaped interface,
 * so MUAs configured for msmtp/sendmail work by swapping one path.
 * Reads the message on stdin; -t takes recipients from To/Cc/Bcc (and
 * strips Bcc before transmission); -f overrides the envelope sender;
 * -a picks the account, otherwise the From address selects it.
 * Recipients @localdomain never reach SMTP: the message is delivered
 * into the maildir <localbox>/<localpart>/ on this machine. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <stb_ds.h>

#include "hml.h"

typedef struct { /* header block to strip (Bcc, including continuations) */
    size_t start, end;
} Range;

static void base64_encode(const unsigned char *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;

    for (i = 0; i + 2 < n; i += 3) {
        out[o++] = t[in[i] >> 2];
        out[o++] = t[(in[i] & 3) << 4 | in[i + 1] >> 4];
        out[o++] = t[(in[i + 1] & 15) << 2 | in[i + 2] >> 6];
        out[o++] = t[in[i + 2] & 63];
    }
    if (i < n) {
        out[o++] = t[in[i] >> 2];
        if (i + 1 < n) {
            out[o++] = t[(in[i] & 3) << 4 | in[i + 1] >> 4];
            out[o++] = t[(in[i + 1] & 15) << 2];
        } else {
            out[o++] = t[(in[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
}

static char *read_all(FILE *f, size_t *lenp) {
    char *buf = NULL;
    size_t len = 0, cap = 0, n;

    for (;;) {
        if (len + 65536 > cap) {
            cap = cap ? cap * 2 : 131072;
            if (!(buf = realloc(buf, cap)))
                return NULL;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    *lenp = len;
    return buf;
}

/* copy a header value, dropping line breaks (unfolding) */
static char *header_value(const char *msg, size_t vs, size_t be) {
    char *v = malloc(be - vs + 1);
    size_t i, n = 0;

    if (!v)
        return NULL;
    for (i = vs; i < be; i++)
        if (msg[i] != '\r' && msg[i] != '\n')
            v[n++] = msg[i];
    v[n] = '\0';
    return v;
}

/* pull one address out of a fragment: prefer <...>, else the bare token */
static void add_address(char ***out, const char *s, const char *e) {
    const char *lt = NULL, *gt = NULL, *p;
    char *a;
    size_t n;

    for (p = s; p < e; p++) {
        if (*p == '<')
            lt = p;
        else if (*p == '>' && lt && !gt)
            gt = p;
    }
    if (lt && gt) {
        s = lt + 1;
        e = gt;
    }
    while (s < e && (*s == ' ' || *s == '\t'))
        s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    if (e <= s)
        return;
    n = (size_t)(e - s);
    if (!(a = malloc(n + 1)))
        return;
    memcpy(a, s, n);
    a[n] = '\0';
    arrput(*out, a);
}

/* split an address list on commas outside quoted strings */
static void split_addresses(const char *v, char ***out) {
    const char *start = v, *p = v;
    int q = 0;

    for (;; p++) {
        if (*p == '"')
            q = !q;
        if ((*p == ',' && !q) || *p == '\0') {
            add_address(out, start, p);
            start = p + 1;
            if (!*p)
                break;
        }
    }
}

static int smtp_read_reply(Imap *im, char *err, size_t errlen) {
    for (;;) {
        char *l = imap_read_line(im, err, errlen);
        int code;
        if (!l)
            return -1;
        if (strlen(l) >= 4 && l[3] == '-')
            continue; /* multiline reply */
        code = atoi(l);
        if (code >= 400) /* keep the server's words for the error report */
            snprintf(err, errlen, "%s", l);
        return code;
    }
}

/* send one command line, expect a reply of the given class (2xx, 3xx) */
static int smtp_command(Imap *im, int class, char *err, size_t errlen, const char *fmt, ...) {
    char cmd[1100];
    va_list ap;
    size_t n;
    int code;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd - 2, fmt, ap);
    va_end(ap);
    n = strlen(cmd);
    cmd[n] = '\r';
    cmd[n + 1] = '\n';
    if (imap_write(im, cmd, n + 2) < 0) {
        snprintf(err, errlen, "connection lost");
        return -1;
    }
    code = smtp_read_reply(im, err, errlen);
    if (code < 0)
        return -1;
    if (code / 100 != class) {
        if (code < 400)
            snprintf(err, errlen, "unexpected reply %d", code);
        return -1;
    }
    return 0;
}

static const Account *pick_account(const char *name, const char *from) {
    int i;

    for (i = 0; i < naccounts; i++)
        if (name && !strcmp(name, accounts[i].name))
            return &accounts[i];
    for (i = 0; i < naccounts; i++)
        if (from && !strcasecmp(from, accounts[i].user))
            return &accounts[i];
    return name ? NULL : &accounts[0];
}

static void append_string(char **b, const char *s) {
    size_t n = strlen(s);

    if (n)
        memcpy(arraddnptr(*b, n), s, n);
}

static int compare_ranges(const void *a, const void *b) {
    const Range *x = a, *y = b;

    return x->start < y->start ? -1 : x->start > y->start;
}

static int fail(const char *what, const char *why) {
    fprintf(stderr, "hml send: %s: %s\n", what, why);
    return 1;
}

/* --- local delivery --- */

static int is_local_address(const char *addr) {
    const char *at = strrchr(addr, '@');

    return at && (!strcasecmp(at + 1, localdomain) || !strcasecmp(at + 1, "localhost"));
}

/* the message, Bcc blocks skipped and CR stripped, into the maildir of
 * the local part; Date and Message-ID are added when missing, and a
 * bare body (no header block at all) gets To: and the blank line */
static int deliver_local(const char *msg, size_t msglen, const Range *bcc, const char *addr, int hasdate, const char *mid, int hashdrs) {
    const char *at = strrchr(addr, '@');
    char err[256], root[4096], dir[4160], tmp[4160];
    size_t n = (size_t)(at - addr), o = 0;
    time_t now = time(NULL);
    long bi = 0;
    FILE *f;

    if (n == 0 || n > 200 || addr[0] == '.' || memchr(addr, '/', n))
        return fail(addr, "bad local part");
    expand_home(localbox, root, sizeof root);
    snprintf(dir, sizeof dir, "%s/%.*s", root, (int)n, addr);
    if (maildir_create(dir, err, sizeof err) < 0)
        return fail(dir, err);
    maildir_temp_path(tmp, sizeof tmp, dir);
    if (!(f = fopen(tmp, "w")))
        return fail(tmp, "cannot create");
    if (!hasdate) {
        char d[64];
        strftime(d, sizeof d, "%a, %d %b %Y %H:%M:%S %z", localtime(&now));
        fprintf(f, "Date: %s\n", d);
    }
    if (mid)
        fprintf(f, "Message-ID: %s\n", mid);
    if (!hashdrs)
        fprintf(f, "To: %s\n\n", addr);
    while (o < msglen) {
        const char *nl;
        size_t le, l;
        if (bi < arrlen(bcc) && o == bcc[bi].start) {
            o = bcc[bi].end;
            bi++;
            continue;
        }
        nl = memchr(msg + o, '\n', msglen - o);
        le = nl ? (size_t)(nl - msg) : msglen;
        l = le - o;
        if (l && msg[le - 1] == '\r')
            l--;
        fwrite(msg + o, 1, l, f);
        fputc('\n', f);
        o = nl ? le + 1 : msglen;
    }
    if (fclose(f) != 0 || maildir_deliver(dir, tmp, err, sizeof err) < 0) {
        unlink(tmp);
        return fail(dir, err[0] ? err : "write failed");
    }
    return 0;
}

int smtp_submit(const Account *a, const char *envfrom, char **rcpts, const char *msg, size_t msglen, char *err, size_t errlen) {
    char e[256] = "", host[64], auth[600], authb64[820], *pass;
    Imap *im;
    size_t o;
    long i;
    int rc = -1;

    if (!(pass = run_password_command(a->passcmd, e, sizeof e))) {
        snprintf(err, errlen, "password: %s", e);
        return -1;
    }
    if (!(im = tls_connect(a->smtphost, a->smtpport, e, sizeof e))) {
        memset(pass, 0, strlen(pass));
        free(pass);
        snprintf(err, errlen, "%s: %s", a->smtphost, e);
        return -1;
    }
    if (gethostname(host, sizeof host - 1) < 0)
        strcpy(host, "localhost");
    host[sizeof host - 1] = '\0';

    /* \0user\0password, base64-encoded */
    {
        size_t ul = strlen(a->user), pl = strlen(pass), n = 0;
        auth[n++] = '\0';
        memcpy(auth + n, a->user, ul);
        n += ul;
        auth[n++] = '\0';
        memcpy(auth + n, pass, pl);
        n += pl;
        base64_encode((unsigned char *)auth, n, authb64);
        memset(auth, 0, sizeof auth);
        memset(pass, 0, pl);
        free(pass);
    }

    if (smtp_read_reply(im, e, sizeof e) / 100 != 2) {
        snprintf(err, errlen, "greeting: %s", e);
        goto close;
    }
    if (smtp_command(im, 2, e, sizeof e, "EHLO %s", host) < 0 || smtp_command(im, 2, e, sizeof e, "AUTH PLAIN %s", authb64) < 0) {
        snprintf(err, errlen, "auth: %s", e);
        goto close;
    }
    memset(authb64, 0, sizeof authb64);
    if (smtp_command(im, 2, e, sizeof e, "MAIL FROM:<%s>", envfrom) < 0) {
        snprintf(err, errlen, "%s: %s", envfrom, e);
        goto close;
    }
    for (i = 0; i < arrlen(rcpts); i++) {
        if (smtp_command(im, 2, e, sizeof e, "RCPT TO:<%s>", rcpts[i]) < 0) {
            snprintf(err, errlen, "%s: %s", rcpts[i], e);
            goto close;
        }
    }
    if (smtp_command(im, 3, e, sizeof e, "DATA") < 0) {
        snprintf(err, errlen, "DATA: %s", e);
        goto close;
    }
    /* the message, CRLF line endings, dot-stuffed */
    o = 0;
    while (o < msglen) {
        const char *nl = memchr(msg + o, '\n', msglen - o);
        size_t le = nl ? (size_t)(nl - msg) : msglen, l = le - o;
        if (l && msg[le - 1] == '\r')
            l--;
        if ((l && msg[o] == '.' && imap_write(im, ".", 1) < 0) || imap_write(im, msg + o, l) < 0 || imap_write(im, "\r\n", 2) < 0) {
            snprintf(err, errlen, "write: connection lost");
            goto close;
        }
        o = nl ? le + 1 : msglen;
    }
    if (imap_write(im, ".\r\n", 3) < 0 || smtp_read_reply(im, e, sizeof e) / 100 != 2) {
        snprintf(err, errlen, "delivery: %s", e[0] ? e : "rejected");
        goto close;
    }
    smtp_command(im, 2, e, sizeof e, "QUIT");
    rc = 0;
close:
    imap_close(im);
    return rc;
}

int send_main(int argc, char **argv) {
    char err[256] = "", mid[128], *msg, *wire = NULL, *from = NULL;
    char *midhdr = NULL, *intent = NULL, *buslocal = NULL, *id;
    const char *acctname = NULL, *envfrom = NULL;
    char **rcpts = NULL;
    Range *bccblk = NULL, fromblk = {0, 0};
    const Account *a = NULL;
    size_t msglen, o;
    long i, bi = 0;
    int tflag = 0, rc = 1, hasdate = 0, hasmid = 0, hashdrs = 0, togw = 0;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) {
            tflag = 1;
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            envfrom = argv[++i];
        } else if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            acctname = argv[++i];
        } else if (!strcmp(argv[i], "-i") || !strncmp(argv[i], "-o", 2)) {
            /* accepted for sendmail compatibility, meaningless here */
        } else if (argv[i][0] == '-') {
            fputs("usage: hml send [-t] [-f from] [-a account] [rcpt ...] < "
                  "msg\n",
                  stderr);
            return 1;
        } else {
            arrput(rcpts, strdup(argv[i]));
        }
    }

    if (!(msg = read_all(stdin, &msglen)) || msglen == 0)
        return fail("stdin", "empty message");

    /* walk the headers: find From, collect -t recipients, mark Bcc blocks */
    o = 0;
    while (o < msglen) {
        const char *nl = memchr(msg + o, '\n', msglen - o);
        size_t ls = o, le = nl ? (size_t)(nl - msg) : msglen, be;
        const char *colon;
        if (le - ls == 0 || (le - ls == 1 && msg[ls] == '\r')) {
            hashdrs = 1;
            break; /* end of headers */
        }
        be = nl ? le + 1 : msglen;
        while (be < msglen && (msg[be] == ' ' || msg[be] == '\t')) {
            const char *nl2 = memchr(msg + be, '\n', msglen - be);
            be = nl2 ? (size_t)(nl2 - msg) + 1 : msglen;
        }
        if ((colon = memchr(msg + ls, ':', le - ls))) {
            size_t nlen = (size_t)(colon - (msg + ls));
            size_t vs = (size_t)(colon - msg) + 1;
            char *v;
            if (nlen == 4 && !strncasecmp(msg + ls, "date", 4))
                hasdate = 1;
            else if (nlen == 10 && !strncasecmp(msg + ls, "message-id", 10)) {
                hasmid = 1;
                if (!midhdr)
                    midhdr = header_value(msg, vs, be);
            } else if (nlen == 10 && !strncasecmp(msg + ls, "hai-intent", 10)) {
                if (!intent)
                    intent = header_value(msg, vs, be);
            }
            if (nlen == 4 && !strncasecmp(msg + ls, "from", 4) && !from) {
                char **one = NULL;
                fromblk.start = ls;
                fromblk.end = be;
                if ((v = header_value(msg, vs, be))) {
                    split_addresses(v, &one);
                    free(v);
                }
                if (arrlen(one))
                    from = one[0];
                for (i = 1; i < arrlen(one); i++)
                    free(one[i]);
                arrfree(one);
            } else if (tflag && ((nlen == 2 && !strncasecmp(msg + ls, "to", 2)) || (nlen == 2 && !strncasecmp(msg + ls, "cc", 2)))) {
                if ((v = header_value(msg, vs, be))) {
                    split_addresses(v, &rcpts);
                    free(v);
                }
            } else if (nlen == 3 && !strncasecmp(msg + ls, "bcc", 3)) {
                if (tflag) {
                    Range r = {ls, be};
                    if ((v = header_value(msg, vs, be))) {
                        split_addresses(v, &rcpts);
                        free(v);
                    }
                    arrput(bccblk, r);
                }
            }
        }
        o = be;
    }

    if (!arrlen(rcpts)) {
        rc = fail("recipients", tflag ? "none found in headers" : "none given");
        goto out;
    }
    /* local recipients first (no network to fail), then the rest by SMTP;
     * one synthesized Message-ID is shared by every copy */
    if (!hasmid)
        snprintf(mid, sizeof mid, "<%ld.%d@%s>", (long)time(NULL), (int)getpid(), localdomain);
    if (from && is_local_address(from) && strcasecmp(from, gateway))
        buslocal = strdup(from);
    for (i = 0; i < arrlen(rcpts);) {
        if (!is_local_address(rcpts[i])) {
            i++;
            continue;
        }
        if (deliver_local(msg, msglen, bccblk, rcpts[i], hasdate, hasmid ? NULL : mid, hashdrs) != 0)
            goto out;
        if (!strcasecmp(rcpts[i], gateway))
            togw = 1;
        else if (!buslocal)
            buslocal = strdup(rcpts[i]);
        free(rcpts[i]);
        arrdel(rcpts, i);
    }
    if (arrlen(rcpts) && !(a = pick_account(acctname, from))) {
        rc = fail("account", "no such account");
        goto out;
    }
    /* what goes on the wire: Bcc blocks out, the synthesized id in, and
     * a bus sender shown in the account's name (the outside would not
     * accept From: user@hai) */
    if (!hasmid && hashdrs) {
        append_string(&wire, "Message-ID: ");
        append_string(&wire, mid);
        append_string(&wire, "\n");
    }
    if (a && from && is_local_address(from) && fromblk.end) {
        append_string(&wire, "From: \"");
        append_string(&wire, from);
        append_string(&wire, "\" <");
        append_string(&wire, a->user);
        append_string(&wire, ">\n");
        arrput(bccblk, fromblk); /* the original: skipped like a Bcc block */
        if (arrlen(bccblk) > 1)
            qsort(bccblk, arrlenu(bccblk), sizeof *bccblk, compare_ranges);
    }
    for (o = 0; o < msglen;) {
        size_t end = bi < arrlen(bccblk) ? bccblk[bi].start : msglen;
        if (end > o)
            memcpy(arraddnptr(wire, end - o), msg + o, end - o);
        if (bi < arrlen(bccblk)) {
            o = bccblk[bi].end;
            bi++;
        } else
            o = msglen;
    }
    /* the bus answering the outside: a message to the gateway address
     * that replies to something that crossed goes out too */
    if (togw && gateway[0] && hashdrs && gateway_outbound(wire, arrlenu(wire), err, sizeof err) < 0)
        fprintf(stderr, "hml send: gateway: %s\n", err);
    if (!arrlen(rcpts)) {
        rc = 0;
        goto out;
    }
    if (!envfrom)
        envfrom = from && !is_local_address(from) ? from : a->user;
    if (smtp_submit(a, envfrom, rcpts, wire, arrlenu(wire), err, sizeof err) < 0) {
        fprintf(stderr, "hml send: %s\n", err);
        goto out;
    }
    /* a bus message with an outside recipient as well: replies from
     * there come back to the bus address */
    if (buslocal && gateway[0] && hashdrs) {
        id = gateway_message_id(midhdr ? midhdr : mid);
        if (gateway_log_add(id, buslocal, rcpts[0], a->name, intent ? intent : "") < 0)
            fprintf(stderr, "hml send: gateway: cannot log %s\n", id);
        free(id);
    }
    rc = 0;

out:
    for (i = 0; i < arrlen(rcpts); i++)
        free(rcpts[i]);
    arrfree(rcpts);
    arrfree(bccblk);
    arrfree(wire);
    free(buslocal);
    free(midhdr);
    free(intent);
    free(from);
    free(msg);
    if (rc == 0 && postsend[0] && system(postsend) != 0)
        fputs("hml: post-send command failed\n", stderr);
    return rc;
}

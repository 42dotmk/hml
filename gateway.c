/* gateway.c - where the bus meets the outside. The local addresses
 * (@localdomain) have no server; mail from outside reaches them
 * through an account `hml recv` syncs, and mail from them goes out
 * through the same account:
 *
 * - inbound, in `hml new`: among the messages just indexed from an
 *   account box, a match of a route's query (a fresh message from
 *   outside) is delivered to the route's local address; a reply to
 *   anything that crossed before goes back to the local address that
 *   sent it, stamped Hai-Intent: answer when that was a question. The
 *   message is rewritten into bus form: UTF-8 text/plain, unencoded,
 *   the quoted mail below the answer dropped, the Message-ID kept.
 * - outbound, in `hml send`: a message from the bus to the gateway
 *   address (the user's, user@hai) that replies to something that
 *   crossed is submitted by SMTP to the outside party as well, From
 *   the account, its Hai-* headers left behind.
 *
 * Every crossing is one line of <mailroot>/.hroutes: Message-ID, the
 * bus address, the outside address, the account, the message's intent.
 * That log threads the two worlds - a reply finds its crossing by
 * In-Reply-To and References - and guards against loops: a message
 * whose id is logged has crossed already (the Sent copy of an
 * outbound reply, the bus copy of an inbound one) and is left alone.
 * Never delete it: a rebuilt index would deliver every old message
 * again. Appends are fsynced; readers take the last line per id. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>
#include <stb_ds.h>

#include "hml.h"

typedef struct {
    char *key; /* Message-ID, no brackets */
    char *local, *remote, *account, *intent;
} Crossing;

static void logpath(char *dst, size_t cap) {
    size_t n;

    expand(mailroot, dst, cap);
    n = strlen(dst);
    snprintf(dst + n, cap - n, "/.hroutes");
}

static void setfields(Crossing *c, char **f) {
    c->local = strdup(f[1]);
    c->remote = strdup(f[2]);
    c->account = strdup(f[3]);
    c->intent = strdup(f[4]);
}

static void freefields(Crossing *c) {
    free(c->local);
    free(c->remote);
    free(c->account);
    free(c->intent);
}

/* the log as a map by Message-ID; a later line for the same id wins */
static Crossing *logread(void) {
    Crossing *log = NULL;
    char path[4096], line[8192];
    FILE *f;

    sh_new_strdup(log);
    logpath(path, sizeof path);
    if (!(f = fopen(path, "r")))
        return log;
    while (fgets(line, sizeof line, f)) {
        char *fld[5], *p = line;
        Crossing c, *old;
        int i;
        line[strcspn(line, "\n")] = '\0';
        for (i = 0; i < 5 && p; i++) {
            char *t = strchr(p, '\t');
            fld[i] = p;
            if (t)
                *t++ = '\0';
            p = t;
        }
        if (i < 5)
            continue;
        if ((old = shgetp_null(log, fld[0]))) {
            freefields(old);
            setfields(old, fld);
            continue;
        }
        c.key = fld[0];
        setfields(&c, fld);
        shputs(log, c);
    }
    fclose(f);
    return log;
}

static void logfree(Crossing *log) {
    ptrdiff_t i;

    for (i = 0; i < shlen(log); i++)
        freefields(&log[i]);
    shfree(log);
}

int gwlogadd(const char *mid, const char *local, const char *remote, const char *account, const char *intent) {
    const char *f[5] = {mid, local, remote, account, intent};
    char path[4096], *buf = NULL;
    size_t n, o = 0;
    int i, fd, rc = 0;

    for (i = 0; i < 5; i++) { /* a tab or newline would break the line */
        const char *s;
        for (s = f[i]; *s; s++)
            arrput(buf, *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
        arrput(buf, i < 4 ? '\t' : '\n');
    }
    logpath(path, sizeof path);
    if ((fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600)) < 0) {
        arrfree(buf);
        return -1;
    }
    n = arrlenu(buf);
    while (o < n) {
        ssize_t w = write(fd, buf + o, n - o);
        if (w < 0) {
            rc = -1;
            break;
        }
        o += (size_t)w;
    }
    if (rc == 0 && fsync(fd) < 0)
        rc = -1;
    close(fd);
    arrfree(buf);
    return rc;
}

/* --- ids ---------------------------------------------------------------- */

char *gwid(const char *v) {
    const char *lt = strchr(v, '<'), *gt;

    if (lt && (gt = strchr(lt, '>')))
        return strndup(lt + 1, (size_t)(gt - lt - 1));
    while (*v == ' ' || *v == '\t')
        v++;
    return strdup(v);
}

/* every <...> of v, in order */
static void idsof(const char *v, char ***out) {
    const char *lt, *gt;

    for (; (lt = strchr(v, '<')) && (gt = strchr(lt, '>')); v = gt + 1)
        arrput(*out, strndup(lt + 1, (size_t)(gt - lt - 1)));
}

static void freeids(char **ids) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(ids); i++)
        free(ids[i]);
    arrfree(ids);
}

/* the crossing a message answers: what In-Reply-To names if that
 * crossed, else the nearest logged ancestor in References */
static Crossing *answers(Crossing *log, Hdr *h) {
    char *v, **ids = NULL;
    Crossing *c = NULL;
    ptrdiff_t i;

    if ((v = mimehget(h, "In-Reply-To"))) {
        idsof(v, &ids);
        free(v);
        for (i = 0; i < arrlen(ids) && !c; i++)
            c = shgetp_null(log, ids[i]);
        freeids(ids);
        ids = NULL;
    }
    if (!c && (v = mimehget(h, "References"))) {
        idsof(v, &ids);
        free(v);
        for (i = arrlen(ids) - 1; i >= 0 && !c; i--)
            c = shgetp_null(log, ids[i]);
        freeids(ids);
    }
    return c;
}

/* --- inbound: outside mail rewritten for the bus ------------------------ */

static char *slurp(const char *path, size_t *n) {
    struct stat sb;
    char *buf;
    FILE *f;

    if (!(f = fopen(path, "rb")))
        return NULL;
    if (fstat(fileno(f), &sb) < 0 || !(buf = malloc((size_t)sb.st_size + 1))) {
        fclose(f);
        return NULL;
    }
    *n = fread(buf, 1, (size_t)sb.st_size, f);
    buf[*n] = '\0';
    fclose(f);
    return buf;
}

static int endswith(const char *s, size_t n, const char *t) {
    size_t k = strlen(t);

    return n >= k && !memcmp(s + n - k, t, k);
}

/* the answer above the quoted mail: from the first quoted line, "On
 * ... wrote:" attribution, "-- " signature, phone signature or Outlook
 * separator on, everything is dropped, an attribution wrapped onto the
 * lines before a quote with it; then trailing blank lines. CRs go too.
 * A reply that was all quote keeps its text. In place. */
static void unquote(char *s) {
    char *orig = strdup(s), *out = s, *line, *nl, *e;
    size_t n;
    int quoted = 0;

    for (line = s; *line; line = nl ? nl + 1 : line + n) {
        nl = strchr(line, '\n');
        n = nl ? (size_t)(nl - line) : strlen(line);
        if (n && line[n - 1] == '\r')
            n--;
        if (line[0] == '>' || (n == 3 && !memcmp(line, "-- ", 3)) || !strncmp(line, "-----Original Message", 21) || !strncmp(line, "Sent from my ", 13) ||
            !strncmp(line, "Get Outlook for ", 16) || (n >= 20 && strspn(line, "_") == n)) {
            quoted = 1;
            break;
        }
        if (n > 9 && !strncmp(line, "On ", 3) && endswith(line, n, "wrote:"))
            break;
        memmove(out, line, n);
        out += n;
        *out++ = '\n';
    }
    *out = '\0';
    if (quoted) { /* "On ... <x@y>\n wrote:" above the first "> " */
        e = out;
        while (e > s && (e[-1] == '\n' || e[-1] == ' '))
            e--;
        line = e;
        while (line > s && line[-1] != '\n')
            line--;
        if (endswith(line, (size_t)(e - line), "wrote:")) {
            e = line;
            while (e > s && e[-1] == '\n')
                e--;
            line = e;
            while (line > s && line[-1] != '\n')
                line--;
            if (!strncmp(line, "On ", 3))
                e = line;
            *e = '\0';
        }
    }
    n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t'))
        s[--n] = '\0';
    if (!n)
        strcpy(s, orig);
    free(orig);
}

/* header value on one line, RFC 2047 decoded */
static char *hdrline(Hdr *h, const char *name) {
    char *v = mimehget(h, name), *d, *p;

    if (!v)
        return NULL;
    d = mimedecode(v);
    free(v);
    for (p = d; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    return d;
}

/* the file at path, rewritten into bus form, delivered into the
 * maildir of the local address; intent, when given, is its Hai-Intent.
 * With expect set, a sender other than that address is refused. from
 * receives the bare sender address. */
static int busdeliver(const char *path, const char *to, const char *intent, const char *expect, char *from, size_t fromcap, char *err, size_t errlen) {
    static const char *const keep[] = {"Date", "Message-ID", "In-Reply-To", "References"};
    const char *at = strrchr(to, '@');
    size_t n, lp = at ? (size_t)(at - to) : strlen(to), i;
    char *buf, *body, *v, root[4096], dir[4160], tmp[4160];
    Hdr *h = NULL;
    FILE *f;

    if (!(buf = slurp(path, &n))) {
        snprintf(err, errlen, "cannot read %.200s", path);
        return -1;
    }
    mimehdrs(buf, n, &h);
    v = hdrline(h, "From");
    mimeaddr(v ? v : "", from, fromcap);
    if (expect && strcasecmp(from, expect)) {
        snprintf(err, errlen, "reply from %.100s to a crossing of %.100s ignored", from, expect);
        goto fail;
    }
    expand(localbox, root, sizeof root);
    snprintf(dir, sizeof dir, "%s/%.*s", root, (int)lp, to);
    if (mdensure(dir, err, errlen) < 0)
        goto fail;
    mdtmp(tmp, sizeof tmp, dir);
    if (!(f = fopen(tmp, "w"))) {
        snprintf(err, errlen, "cannot create %.200s", tmp);
        goto fail;
    }
    fprintf(f, "From: %s\nTo: %s\n", v ? v : "", to);
    free(v);
    v = hdrline(h, "Subject");
    fprintf(f, "Subject: %s\n", v ? v : "");
    free(v);
    if (!(v = mimehget(h, "Date"))) {
        time_t now = time(NULL);
        char d[64];
        strftime(d, sizeof d, "%a, %d %b %Y %H:%M:%S %z", localtime(&now));
        fprintf(f, "Date: %s\n", d);
    }
    free(v);
    for (i = 0; i < sizeof keep / sizeof *keep; i++) {
        if ((v = mimehget(h, keep[i])))
            fprintf(f, "%s: %s\n", keep[i], v);
        free(v);
    }
    fputs("MIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\n"
          "Content-Transfer-Encoding: 8bit\n",
          f);
    if (intent)
        fprintf(f, "Hai-Intent: %s\n", intent);
    fputc('\n', f);
    body = mimeplain(buf, n);
    unquote(body);
    fputs(body, f);
    fputc('\n', f);
    free(body);
    if (fclose(f) != 0 || mddeliver(dir, tmp, err, errlen) < 0) {
        unlink(tmp);
        goto fail;
    }
    arrfree(h);
    free(buf);
    return 0;
fail:
    arrfree(h);
    free(buf);
    return -1;
}

/* the crossing the file answers, by its own headers */
static Crossing *answersof(Crossing *log, const char *path) {
    Crossing *c;
    Hdr *h = NULL;
    size_t n;
    char *buf = slurp(path, &n);

    if (!buf)
        return NULL;
    mimehdrs(buf, n, &h);
    c = answers(log, h);
    arrfree(h);
    free(buf);
    return c;
}

void gwinbound(sqlite3 *db) {
    struct {
        sqlite3_int64 key;
        int value;
    } *hits = NULL;
    Crossing *log;
    sqlite3_stmt *st, *refs;
    sqlite3_int64 last = -1;
    char *err = NULL;
    int i, rc;

    if (!gateway[0])
        return;
    log = logread();
    for (i = 0; i < nroutes; i++) { /* fresh mail from outside */
        Query q;
        if (querycompile(routes[i].query, &q, &err) < 0) {
            fprintf(stderr, "hml new: route %d: %s\n", i, err);
            free(err);
            err = NULL;
            continue;
        }
        st = queryprep(db,
                       "SELECT id FROM msg WHERE id IN (SELECT id FROM "
                       "newmsg) AND (%s)",
                       &q, "", &err);
        if (!st) {
            fprintf(stderr, "hml new: route %d: %s\n", i, err);
            free(err);
            err = NULL;
        } else {
            while (sqlite3_step(st) == SQLITE_ROW) {
                sqlite3_int64 id = sqlite3_column_int64(st, 0);
                if (hmgeti(hits, id) < 0)
                    hmput(hits, id, i);
            }
            sqlite3_finalize(st);
        }
        queryfree(&q);
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT m.id, m.mid, f.box, f.sub, f.name FROM msg "
                           "m JOIN file f ON f.msg = m.id WHERE m.id IN "
                           "(SELECT id FROM newmsg) ORDER BY m.id",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT mid FROM ref WHERE msg = ?", -1, &refs, NULL) != SQLITE_OK) {
        fprintf(stderr, "hml new: gateway: %s\n", sqlite3_errmsg(db));
        logfree(log);
        hmfree(hits);
        return;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        const char *mid = (const char *)sqlite3_column_text(st, 1), *box = (const char *)sqlite3_column_text(st, 2),
                   *sub = (const char *)sqlite3_column_text(st, 3), *name = (const char *)sqlite3_column_text(st, 4), *to, *intent = NULL, *expect = NULL,
                   *slash;
        char path[4200], from[256], e[256], acct[64];
        Crossing *c = NULL, add;
        ptrdiff_t k;
        size_t ld = strlen(localdomain);

        if (id == last) /* one file per message is enough */
            continue;
        last = id;
        if (!strncmp(box, localdomain, ld) && box[ld] == '/')
            continue; /* the bus itself */
        if (shgetp_null(log, mid))
            continue; /* crossed already: our own copy coming back */
        sqlite3_bind_int64(refs, 1, id);
        while (!c && sqlite3_step(refs) == SQLITE_ROW)
            c = shgetp_null(log, (const char *)sqlite3_column_text(refs, 0));
        sqlite3_reset(refs);
        if (!c && (k = hmgeti(hits, id)) < 0)
            continue;
        if (!filepath(box, sub, name, path, sizeof path))
            continue;
        if (c) { /* a reply: to whoever it answers, precisely */
            Crossing *p = answersof(log, path);
            if (p)
                c = p;
            to = c->local;
            expect = c->remote;
            if (!strcmp(c->intent, "ask"))
                intent = "answer";
        } else
            to = routes[hits[k].value].local;
        if (busdeliver(path, to, intent, expect, from, sizeof from, e, sizeof e) < 0) {
            fprintf(stderr, "hml new: gateway: %s: %s\n", mid, e);
            continue;
        }
        slash = strchr(box, '/');
        snprintf(acct, sizeof acct, "%.*s", (int)(slash ? slash - box : 0), box);
        if (gwlogadd(mid, to, from, acct, intent ? intent : "") < 0)
            fprintf(stderr, "hml new: gateway: cannot log %s\n", mid);
        printf("hml new: gateway: %s -> %s%s\n", from, to, intent ? " (answer)" : "");
        /* a later message of this run may answer this one */
        add.key = (char *)mid;
        add.local = strdup(to);
        add.remote = strdup(from);
        add.account = strdup(acct);
        add.intent = strdup(intent ? intent : "");
        shputs(log, add);
    }
    if (rc != SQLITE_DONE)
        fprintf(stderr, "hml new: gateway: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    sqlite3_finalize(refs);
    logfree(log);
    hmfree(hits);
}

/* --- outbound: the bus answering the outside ---------------------------- */

static void sadd(char **b, const char *s, size_t n) {
    if (n)
        memcpy(arraddnptr(*b, n), s, n);
}

int gwoutbound(const char *msg, size_t n, char *err, size_t errlen) {
    const Account *a = NULL;
    Crossing *log, *c;
    Hdr *h = NULL;
    char *v, *wire = NULL, *mid, *intent, **rcpts = NULL, local[256];
    size_t bo, i;
    int k, rc;

    if (!gateway[0])
        return 0;
    bo = mimehdrs(msg, n, &h);
    log = logread();
    if (!(c = answers(log, h))) {
        arrfree(h);
        logfree(log);
        return 0;
    }
    for (k = 0; k < naccounts; k++)
        if (!strcmp(accounts[k].name, c->account))
            a = &accounts[k];
    if (!a) {
        snprintf(err, errlen, "account %s is gone", c->account);
        arrfree(h);
        logfree(log);
        return -1;
    }
    v = mimehget(h, "From");
    mimeaddr(v ? v : "", local, sizeof local);
    free(v);
    v = mimehget(h, "Message-ID");
    mid = gwid(v ? v : "");
    free(v);
    if (!(intent = mimehget(h, "Hai-Intent")))
        intent = strdup("");
    /* the same message: From the account in the bus address's name, To
     * the outside party, the bus's own headers left behind */
    sadd(&wire, "From: \"", 7);
    sadd(&wire, local, strlen(local));
    sadd(&wire, "\" <", 3);
    sadd(&wire, a->user, strlen(a->user));
    sadd(&wire, ">\nTo: ", 6);
    sadd(&wire, c->remote, strlen(c->remote));
    sadd(&wire, "\n", 1);
    for (i = 0; i < arrlenu(h); i++) {
        const char *nm = h[i].name;
        size_t nl = h[i].nlen;
        if ((nl == 4 && !strncasecmp(nm, "From", 4)) || (nl == 2 && !strncasecmp(nm, "To", 2)) || (nl == 2 && !strncasecmp(nm, "Cc", 2)) ||
            (nl == 3 && !strncasecmp(nm, "Bcc", 3)) || (nl >= 4 && !strncasecmp(nm, "Hai-", 4)))
            continue;
        sadd(&wire, nm, nl);
        sadd(&wire, ":", 1);
        sadd(&wire, h[i].val, h[i].vlen);
        sadd(&wire, "\n", 1);
    }
    sadd(&wire, "\n", 1);
    sadd(&wire, msg + bo, n - bo);
    arrput(rcpts, strdup(c->remote));
    rc = smtpsubmit(a, a->user, rcpts, wire, arrlenu(wire), err, errlen);
    if (rc == 0 && gwlogadd(mid, local, c->remote, a->name, intent) < 0)
        fprintf(stderr, "hml send: gateway: cannot log %s\n", mid);
    free(rcpts[0]);
    arrfree(rcpts);
    arrfree(wire);
    free(mid);
    free(intent);
    arrfree(h);
    logfree(log);
    return rc == 0 ? 1 : -1;
}

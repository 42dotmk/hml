/* show.c - what a mail reader needs beyond search: "hml show" prints the
 * matching messages in notmuch's --format=text framing (\fmessage{,
 * \fheader{, \fpart{ ID: n, ... — the shape hed's mail plugin parses), or
 * a message / one decoded MIME part as raw bytes; "hml reply" prints a
 * reply template (headers, blank line, quoted text) for the newest match.
 * Parts are numbered pre-order from 1 like notmuch, so a part id found in
 * the text output addresses the same part in --format=raw --part=N. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <sqlite3.h>
#include <stb_ds.h>

#include "hml.h"

enum { DepthMax = 16 };

typedef struct {
    sqlite3_int64 id;
    char *mid;
    long date;
    char *box;  /* "acct/Sub" */
    char *path; /* absolute maildir file */
} Hit;

static void sadd(char **b, const char *t) {
    size_t n = strlen(t);

    if (n)
        memcpy(arraddnptr(*b, n), t, n);
}

static int fail(const char *cmd, char *err) {
    fprintf(stderr, "hml %s: %s\n", cmd, err ? err : "error");
    free(err);
    return 2;
}

/* --- the matching messages ---------------------------------------------- */

/* messages matching c with the path of one of their files, by date */
static Hit *hits(sqlite3 *db, const Query *c, int newest, char **err) {
    sqlite3_stmt *st;
    Hit *out = NULL;
    char path[4608];
    int rc;

    if (!(st = queryprep(db,
                         "SELECT msg.id,msg.mid,msg.date,file.box,file.sub,"
                         "file.name FROM msg JOIN file ON file.msg=msg.id"
                         " WHERE (%s)",
                         c, newest ? " ORDER BY msg.date DESC,msg.id DESC"
                                   : " ORDER BY msg.date,msg.id",
                         err)))
        return NULL;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        const char *box = (const char *)sqlite3_column_text(st, 3);
        Hit h;
        if (arrlen(out) && arrlast(out).id == id)
            continue; /* another file of the same message */
        if (!filepath(box, (const char *)sqlite3_column_text(st, 4),
                      (const char *)sqlite3_column_text(st, 5), path,
                      sizeof path))
            continue;
        h.id = id;
        h.mid = strdup((const char *)sqlite3_column_text(st, 1));
        h.date = (long)sqlite3_column_int64(st, 2);
        h.box = strdup(box);
        h.path = strdup(path);
        arrput(out, h);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && err && !*err) {
        *err = strdup(sqlite3_errmsg(db));
        return NULL;
    }
    return out;
}

static void hitsfree(Hit *h) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(h); i++) {
        free(h[i].mid);
        free(h[i].box);
        free(h[i].path);
    }
    arrfree(h);
}

static int readfile(const char *path, char **buf, size_t *n) {
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t k;

    if (!f)
        return -1;
    for (;;) {
        char tmp[65536];
        k = fread(tmp, 1, sizeof tmp, f);
        if (k)
            memcpy(arraddnptr(b, k), tmp, k);
        if (k < sizeof tmp)
            break;
    }
    fclose(f);
    *n = arrlenu(b);
    arrput(b, '\0');
    *buf = b;
    return 0;
}

/* --- the MIME walk ------------------------------------------------------ */

enum { WShow, WRaw, WText };

typedef struct {
    int mode;
    int html;    /* show: include text/html bodies */
    int want;    /* raw: the part id to extract */
    int found;
    int next;    /* next part id */
    int depth;
    char *plain; /* text: stb arrays */
    char *htmltext;
} Walk;

static void walk(Walk *w, const char *s, size_t n);

/* the entities between --boundary lines, each walked on its own */
static void multipart(Walk *w, const char *s, size_t n, const char *b) {
    size_t bl = strlen(b), pos = 0, start = 0, end;
    const char *nl;
    int in = 0;

    while (pos < n) {
        nl = memchr(s + pos, '\n', n - pos);
        end = nl ? (size_t)(nl - s) + 1 : n;
        if (end - pos >= bl + 2 && s[pos] == '-' && s[pos + 1] == '-' &&
            !memcmp(s + pos + 2, b, bl)) {
            if (in)
                walk(w, s + start, pos > start ? pos - start : 0);
            if (end - pos >= bl + 4 && s[pos + bl + 2] == '-' &&
                s[pos + bl + 3] == '-')
                return;
            in = 1;
            start = end;
        }
        pos = end;
    }
    if (in)
        walk(w, s + start, n - start);
}

/* the header block notmuch prints for a message (also inside a
 * message/rfc822 part) */
static void showheaders(Hdr *h, long date, const char *tags) {
    static const char *names[] = {"Subject", "From", "To", "Cc", "Date"};
    char *from = mimehget(h, "From"), *d, name[128], rel[32];
    size_t i;

    puts("\fheader{");
    d = mimedecode(from ? from : "");
    dispname(d, name, sizeof name);
    reldate(date, rel, sizeof rel);
    printf("%s (%s) (%s)\n", name, rel, tags ? tags : "");
    free(d);
    free(from);
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        char *v = mimehget(h, names[i]);
        if (!v)
            continue;
        if (*v || i != 3) { /* Cc only when present */
            d = i == 4 ? strdup(v) : mimedecode(v);
            printf("%s: %s\n", names[i], d);
            free(d);
        }
        free(v);
    }
    puts("\fheader}");
}

/* decoded text of a text part body, UTF-8 */
static char *textof(const char *ct, const char *cte, const char *s,
                    size_t n) {
    char *dec = mimecte(cte, s, n), *cs = ct ? mimeparam(ct, "charset") : NULL,
         *u = NULL;

    mimeutf8(&u, cs, dec, arrlenu(dec));
    arrfree(dec);
    free(cs);
    return u;
}

/* one entity: header block, then per its type; ids are assigned in the
 * order notmuch does (this part, then its children) */
static void walk(Walk *w, const char *s, size_t n) {
    Hdr *h = NULL;
    size_t bo = mimehdrs(s, n, &h);
    char *ct = mimehget(h, "Content-Type"),
         *cte = mimehget(h, "Content-Transfer-Encoding"),
         *cd = mimehget(h, "Content-Disposition"), *fn = NULL, *b, *u, *cid,
         type[128] = "text/plain";
    int id = w->next++, ismulti, isrfc, istext, isatt;

    if (ct)
        mimetype(ct, type, sizeof type);
    if (cd)
        fn = mimeparam(cd, "filename");
    if (!fn && ct)
        fn = mimeparam(ct, "name");
    ismulti = !strncmp(type, "multipart/", 10);
    isrfc = !strcmp(type, "message/rfc822");
    istext = !strncmp(type, "text/", 5);
    /* an attachment is what says so, or a named non-text leaf that is not
     * a Content-ID image the HTML references (the same rule the index
     * uses for the attachment tag) */
    cid = mimehget(h, "Content-ID");
    isatt = !(!strcmp(type, "application/pkcs7-signature") ||
              !strcmp(type, "application/x-pkcs7-signature") ||
              !strcmp(type, "application/pgp-signature")) &&
            ((cd && !strncasecmp(cd, "attachment", 10)) ||
             (fn && !cid && !ismulti && !isrfc && !istext));
    free(cid);

    if (w->mode == WRaw) {
        if (id == w->want) {
            w->found = 1;
            if (ismulti || isrfc)
                fwrite(s + bo, 1, n - bo, stdout);
            else {
                char *dec = mimecte(cte, s + bo, n - bo);
                fwrite(dec, 1, arrlenu(dec), stdout);
                arrfree(dec);
            }
        } else if (ismulti && w->depth < DepthMax && ct &&
                   (b = mimeparam(ct, "boundary"))) {
            w->depth++;
            multipart(w, s + bo, n - bo, b);
            w->depth--;
            free(b);
        } else if (isrfc && w->depth < DepthMax) {
            char *dec = mimecte(cte, s + bo, n - bo);
            w->depth++;
            walk(w, dec, arrlenu(dec));
            w->depth--;
            arrfree(dec);
        }
    } else if (w->mode == WText) {
        if (ismulti && w->depth < DepthMax && ct &&
            (b = mimeparam(ct, "boundary"))) {
            w->depth++;
            multipart(w, s + bo, n - bo, b);
            w->depth--;
            free(b);
        } else if (isrfc && w->depth < DepthMax) {
            char *dec = mimecte(cte, s + bo, n - bo);
            w->depth++;
            walk(w, dec, arrlenu(dec));
            w->depth--;
            arrfree(dec);
        } else if (istext && !isatt) {
            u = textof(ct, cte, s + bo, n - bo);
            if (!strcmp(type, "text/html"))
                mimehtmltext(&w->htmltext, u, arrlenu(u));
            else
                memcpy(arraddnptr(w->plain, arrlenu(u)), u, arrlenu(u));
            arrfree(u);
        }
    } else if (isatt) {
        char *dn = mimedecode(fn ? fn : "");
        printf("\fattachment{ ID: %d, Filename: %s, Content-type: %s\n", id,
               dn, type);
        printf("Non-text part: %s\n", type);
        puts("\fattachment}");
        free(dn);
    } else if (ismulti) {
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        if (w->depth < DepthMax && ct && (b = mimeparam(ct, "boundary"))) {
            w->depth++;
            multipart(w, s + bo, n - bo, b);
            w->depth--;
            free(b);
        }
        puts("\fpart}");
    } else if (isrfc) {
        char *dec = mimecte(cte, s + bo, n - bo);
        Hdr *ih = NULL;
        char *dt;
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        if (w->depth < DepthMax) {
            mimehdrs(dec, arrlenu(dec), &ih);
            dt = mimehget(ih, "Date");
            showheaders(ih, dt ? mimedate(dt) : 0, NULL);
            free(dt);
            arrfree(ih);
            puts("\fbody{");
            w->depth++;
            walk(w, dec, arrlenu(dec));
            w->depth--;
            puts("\fbody}");
        }
        puts("\fpart}");
        arrfree(dec);
    } else if (istext) {
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        if (strcmp(type, "text/html") || w->html) {
            u = textof(ct, cte, s + bo, n - bo);
            fwrite(u, 1, arrlenu(u), stdout);
            if (!arrlenu(u) || arrlast(u) != '\n')
                putchar('\n'); /* the closing marker gets its own line */
            arrfree(u);
        }
        puts("\fpart}");
    } else {
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        printf("Non-text part: %s\n", type);
        puts("\fpart}");
    }
    free(ct);
    free(cte);
    free(cd);
    free(fn);
    arrfree(h);
}

static char *msgtags(sqlite3 *db, sqlite3_int64 id) {
    sqlite3_stmt *st;
    char *out = NULL;

    sqlite3_prepare_v2(db, "SELECT name FROM tag WHERE msg=? ORDER BY name",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (out)
            sadd(&out, " ");
        sadd(&out, (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    arrput(out, '\0');
    return out;
}

/* one message as an mbox entry: a "From " separator line, the message,
 * body lines that would read as separators escaped mboxrd-style (">From "),
 * a blank line after — what `git am` and every mbox reader expect */
static void mboxout(const char *buf, size_t n, long date) {
    time_t t = date;
    char stamp[64];
    size_t a = 0, b;

    strftime(stamp, sizeof stamp, "%a %b %e %H:%M:%S %Y", gmtime(&t));
    printf("From MAILER-DAEMON %s\n", stamp);
    while (a < n) {
        for (b = a; b < n && buf[b] != '\n'; b++)
            ;
        {
            size_t k = a;
            while (k < b && buf[k] == '>')
                k++;
            if (b - k >= 5 && !memcmp(buf + k, "From ", 5))
                putchar('>');
        }
        fwrite(buf + a, 1, b - a, stdout);
        putchar('\n');
        a = b + 1;
    }
    putchar('\n');
}

/* --- hml show ------------------------------------------------------------ */

/* the options both commands share: --key=value / --flag, "--" ends them,
 * the rest is the query */
typedef struct {
    const char *format, *replyto;
    int part, html, entire;
} SOpts;

static char *sopts(int argc, char **argv, SOpts *o, const char *cmd) {
    char *q = NULL;
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                if (q)
                    sadd(&q, " ");
                sadd(&q, argv[i]);
            }
            break;
        }
        if (!strncmp(a, "--format=", 9))
            o->format = a + 9;
        else if (!strncmp(a, "--part=", 7))
            o->part = atoi(a + 7);
        else if (!strcmp(a, "--include-html"))
            o->html = 1;
        else if (!strcmp(a, "--entire-thread") ||
                 !strncmp(a, "--entire-thread=", 16))
            o->entire = 1;
        else if (!strncmp(a, "--reply-to=", 11))
            o->replyto = a + 11;
        else if (!strncmp(a, "--", 2)) {
            fprintf(stderr, "hml %s: unknown option %s\n", cmd, a);
            return NULL;
        } else {
            if (q)
                sadd(&q, " ");
            sadd(&q, a);
        }
    }
    arrput(q, '\0');
    return q;
}

int showmain(int argc, char **argv) {
    SOpts o = {"text", "sender", 0, 0, 0};
    Query c;
    sqlite3 *db;
    Hit *hs;
    char *q, *err = NULL, dberr[256], *buf;
    size_t n;
    ptrdiff_t i;
    int raw, mbox;

    if (!(q = sopts(argc, argv, &o, "show")))
        return 2;
    if (!*q) {
        fputs("usage: hml show [--format=text|raw|mbox] [--part=N] "
              "[--include-html] [--] <query>\n",
              stderr);
        arrfree(q);
        return 2;
    }
    raw = !strcmp(o.format, "raw");
    mbox = !strcmp(o.format, "mbox");
    if (!raw && !mbox && strcmp(o.format, "text")) {
        arrfree(q);
        return fail("show", strdup("--format must be text, raw or mbox"));
    }
    if (querycompile(q, &c, &err) < 0) {
        arrfree(q);
        return fail("show", err);
    }
    arrfree(q);
    if (!(db = dbopen(dberr, sizeof dberr))) {
        queryfree(&c);
        return fail("show", strdup(dberr));
    }
    hs = hits(db, &c, 0, &err);
    queryfree(&c);
    if (err) {
        sqlite3_close(db);
        return fail("show", err);
    }
    if (!arrlen(hs)) {
        sqlite3_close(db);
        hitsfree(hs);
        return fail("show", strdup("no messages match"));
    }
    for (i = 0; i < (raw ? 1 : arrlen(hs)); i++) {
        Walk w = {WShow, o.html, o.part, 0, 1, 0, NULL, NULL};
        if (readfile(hs[i].path, &buf, &n) < 0) {
            if (raw) {
                sqlite3_close(db);
                hitsfree(hs);
                return fail("show", strdup("cannot read message file"));
            }
            continue; /* vanished since the index was written */
        }
        if (mbox) {
            mboxout(buf, n, hs[i].date);
        } else if (raw && !o.part) {
            fwrite(buf, 1, n, stdout);
        } else if (raw) {
            w.mode = WRaw;
            walk(&w, buf, n);
            if (!w.found) {
                arrfree(buf);
                sqlite3_close(db);
                hitsfree(hs);
                return fail("show", strdup("no such part"));
            }
        } else {
            Hdr *h = NULL;
            char *tags = msgtags(db, hs[i].id);
            mimehdrs(buf, n, &h);
            printf("\fmessage{ id:%s depth:0 match:1 excluded:0 filename:%s\n",
                   hs[i].mid, hs[i].path);
            showheaders(h, hs[i].date, tags);
            puts("\fbody{");
            walk(&w, buf, n);
            puts("\fbody}");
            puts("\fmessage}");
            arrfree(h);
            arrfree(tags);
        }
        arrfree(buf);
    }
    sqlite3_close(db);
    hitsfree(hs);
    return 0;
}

/* --- hml reply ----------------------------------------------------------- */

/* split an address header on the commas between mailboxes (not the ones
 * inside quotes or <>), each trimmed; stb array of malloc'd strings */
static void addrsplit(const char *v, char ***out) {
    const char *p = v, *start = v;
    int quote = 0, angle = 0;

    for (;; p++) {
        if (*p == '"' && !angle)
            quote = !quote;
        else if (*p == '<' && !quote)
            angle = 1;
        else if (*p == '>' && !quote)
            angle = 0;
        if ((*p == ',' && !quote && !angle) || !*p) {
            const char *e = p;
            while (start < e && isspace((unsigned char)*start))
                start++;
            while (e > start && isspace((unsigned char)e[-1]))
                e--;
            if (e > start) {
                char *a = malloc((size_t)(e - start) + 1);
                memcpy(a, start, (size_t)(e - start));
                a[e - start] = '\0';
                arrput(*out, a);
            }
            if (!*p)
                return;
            start = p + 1;
        }
    }
}

/* the bare address of a mailbox, lowercased */
static void addrof(const char *m, char *out, size_t cap) {
    const char *lt = strchr(m, '<'), *gt = lt ? strchr(lt, '>') : NULL, *s, *e;
    size_t i, n;

    if (lt && gt) {
        s = lt + 1;
        e = gt;
    } else {
        s = m;
        e = m + strlen(m);
        while (s < e && isspace((unsigned char)*s))
            s++;
        while (e > s && isspace((unsigned char)e[-1]))
            e--;
    }
    n = (size_t)(e - s);
    if (n >= cap)
        n = cap - 1;
    for (i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[n] = '\0';
}

static int sameaddr(const char *a, const char *b) {
    char x[256], y[256];

    addrof(a, x, sizeof x);
    addrof(b, y, sizeof y);
    return !strcmp(x, y);
}

static int isown(const char *m) {
    int k;

    for (k = 0; k < naccounts; k++)
        if (sameaddr(m, accounts[k].user))
            return 1;
    return 0;
}

static int inlist(char **l, const char *m) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(l); i++)
        if (sameaddr(l[i], m))
            return 1;
    return 0;
}

static void freelist(char **l) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(l); i++)
        free(l[i]);
    arrfree(l);
}

/* a decoded header split into mailboxes; empty when absent */
static char **mailboxes(Hdr *h, const char *name) {
    char *v = mimehget(h, name), *d, **l = NULL;

    if (!v)
        return NULL;
    d = mimedecode(v);
    addrsplit(d, &l);
    free(d);
    free(v);
    return l;
}

static void printlist(const char *name, char **l) {
    ptrdiff_t i;

    if (!arrlen(l))
        return;
    printf("%s: ", name);
    for (i = 0; i < arrlen(l); i++)
        printf("%s%s", i ? ", " : "", l[i]);
    putchar('\n');
}

int replymain(int argc, char **argv) {
    SOpts o = {"text", "sender", 0, 0, 0};
    Query c;
    sqlite3 *db;
    Hit *hs;
    Hdr *h = NULL;
    Walk w = {WText, 0, 0, 0, 1, 0, NULL, NULL};
    char *q, *err = NULL, dberr[256], *buf, *v, *from, *subject, *date,
        *mid, *me = NULL, **to = NULL, **cc = NULL, **orig, **l, *text;
    const char *acct = NULL, *p;
    size_t n;
    ptrdiff_t i;
    int all, k;

    if (!(q = sopts(argc, argv, &o, "reply")))
        return 2;
    if (!*q) {
        fputs("usage: hml reply [--reply-to=sender|all] [--] <query>\n",
              stderr);
        arrfree(q);
        return 2;
    }
    all = !strcmp(o.replyto, "all");
    if (querycompile(q, &c, &err) < 0) {
        arrfree(q);
        return fail("reply", err);
    }
    arrfree(q);
    if (!(db = dbopen(dberr, sizeof dberr))) {
        queryfree(&c);
        return fail("reply", strdup(dberr));
    }
    hs = hits(db, &c, 1, &err); /* newest first: reply to the latest */
    queryfree(&c);
    sqlite3_close(db);
    if (err)
        return fail("reply", err);
    if (!arrlen(hs)) {
        hitsfree(hs);
        return fail("reply", strdup("no messages match"));
    }
    if (readfile(hs[0].path, &buf, &n) < 0) {
        hitsfree(hs);
        return fail("reply", strdup("cannot read message file"));
    }
    mimehdrs(buf, n, &h);

    /* the account the message lives in is the one replying */
    for (k = 0; k < naccounts; k++)
        if (!strncmp(hs[0].box, accounts[k].name, strlen(accounts[k].name)) &&
            hs[0].box[strlen(accounts[k].name)] == '/')
            acct = accounts[k].user;
    if (!acct && naccounts)
        acct = accounts[0].user;

    /* From: our address as the original addressed it (keeps the display
     * name the sender used for us), else bare */
    orig = mailboxes(h, "To");
    l = mailboxes(h, "Cc");
    for (i = 0; i < arrlen(l); i++)
        arrput(orig, l[i]);
    arrfree(l);
    for (i = 0; i < arrlen(orig); i++)
        if (acct && sameaddr(orig[i], acct)) {
            me = strdup(orig[i]);
            break;
        }
    if (!me)
        me = strdup(acct ? acct : "");

    /* To: the sender (Reply-To wins); all: plus everyone else, minus us */
    l = mailboxes(h, "Reply-To");
    if (!arrlen(l)) {
        freelist(l);
        l = mailboxes(h, "From");
    }
    for (i = 0; i < arrlen(l); i++)
        if (!inlist(to, l[i]))
            arrput(to, strdup(l[i]));
    freelist(l);
    if (all) {
        for (i = 0; i < arrlen(orig); i++)
            if (!isown(orig[i]) && !inlist(to, orig[i]) &&
                !inlist(cc, orig[i]))
                arrput(cc, strdup(orig[i]));
    }
    freelist(orig);

    from = (v = mimehget(h, "From")) ? mimedecode(v) : strdup("");
    free(v);
    subject = (v = mimehget(h, "Subject")) ? mimedecode(v) : strdup("");
    free(v);
    date = mimehget(h, "Date");
    mid = mimehget(h, "Message-ID");

    printf("From: %s\n", me);
    p = subject;
    while (isspace((unsigned char)*p))
        p++;
    printf("Subject: %s%s\n", strncasecmp(p, "re:", 3) ? "Re: " : "", p);
    printlist("To", to);
    printlist("Cc", cc);
    if (mid && *mid)
        printf("In-Reply-To: %s\n", mid);
    v = mimehget(h, "References");
    if (!v || !*v) {
        free(v);
        v = mimehget(h, "In-Reply-To");
    }
    if ((v && *v) || (mid && *mid))
        printf("References: %s%s%s\n", v && *v ? v : "",
               v && *v && mid && *mid ? " " : "", mid && *mid ? mid : "");
    free(v);
    putchar('\n');

    /* the quoted text: plain parts, else the html stripped to text */
    walk(&w, buf, n);
    text = arrlen(w.plain) ? w.plain : w.htmltext;
    printf("On %s, %s wrote:\n", date ? date : "", from);
    if (text) {
        size_t a = 0, b, len = arrlenu(text);
        while (a < len) {
            for (b = a; b < len && text[b] != '\n'; b++)
                ;
            if (b > a && text[b - 1] == '\r')
                printf("> %.*s\n", (int)(b - a - 1), text + a);
            else
                printf("> %.*s\n", (int)(b - a), text + a);
            a = b + 1;
        }
    }
    arrfree(w.plain);
    arrfree(w.htmltext);
    freelist(to);
    freelist(cc);
    free(me);
    free(from);
    free(subject);
    free(date);
    free(mid);
    arrfree(h);
    arrfree(buf);
    hitsfree(hs);
    return 0;
}

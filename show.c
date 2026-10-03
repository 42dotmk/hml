/* show.c - what a mail reader needs beyond search: "hml show" prints the
 * matching messages in notmuch's --format=text framing (\fmessage{,
 * \fheader{, \fpart{ ID: n, ... — the shape hed's mail plugin parses), or
 * a message / one decoded MIME part as raw bytes; "hml reply" prints a
 * reply template (headers, blank line, quoted text) for the newest match.
 * Parts are numbered pre-order from 1 like notmuch, so a part id found in
 * the text output addresses the same part in --format=raw --part=N. A
 * body line that starts with a form feed goes out with it doubled, so
 * a mail quoting this output cannot pass for framing (see print_body). */
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

/* A text part's body, line by line, with a leading form feed doubled.
 * A mail may quote the output of this very command verbatim — hai's
 * tool results are full of it — and the reader must not read that
 * framing as this stream's: no marker of ours ever starts with two.
 * The closing marker gets its own line. */
static void print_body(const char *s, size_t n) {
    size_t a = 0;

    if (!n) {
        putchar('\n');
        return;
    }
    while (a < n) {
        size_t b = a;

        while (b < n && s[b] != '\n')
            b++;
        if (s[a] == '\f')
            putchar('\f');
        fwrite(s + a, 1, b - a, stdout);
        putchar('\n');
        a = b + 1;
    }
}

static void append_string(char **b, const char *t) {
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
static Hit *find_hits(sqlite3 *db, const Query *c, int newest, char **err) {
    sqlite3_stmt *st;
    Hit *out = NULL;
    char path[4608];
    int rc;

    if (!(st = query_prepare(db,
                             "SELECT msg.id,msg.mid,msg.date,file.box,file.sub,"
                             "file.name FROM msg JOIN file ON file.msg=msg.id"
                             " WHERE (%s)",
                             c, newest ? " ORDER BY msg.date DESC,msg.id DESC" : " ORDER BY msg.date,msg.id", err)))
        return NULL;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        const char *box = (const char *)sqlite3_column_text(st, 3);
        Hit h;
        if (arrlen(out) && arrlast(out).id == id)
            continue; /* another file of the same message */
        if (!file_path(box, (const char *)sqlite3_column_text(st, 4), (const char *)sqlite3_column_text(st, 5), path, sizeof path))
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

static void free_hits(Hit *h) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(h); i++) {
        free(h[i].mid);
        free(h[i].box);
        free(h[i].path);
    }
    arrfree(h);
}

static int read_file(const char *path, char **buf, size_t *n) {
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
    int html; /* show: include text/html bodies */
    int want; /* raw: the part id to extract */
    int found;
    int next; /* next part id */
    int depth;
    char *plain; /* text: stb arrays */
    char *htmltext;
} Walk;

static void walk_entity(Walk *w, const char *s, size_t n);

/* the entities between --boundary lines, each walked on its own */
static void walk_multipart(Walk *w, const char *s, size_t n, const char *b) {
    size_t bl = strlen(b), pos = 0, start = 0, end;
    const char *nl;
    int in = 0;

    while (pos < n) {
        nl = memchr(s + pos, '\n', n - pos);
        end = nl ? (size_t)(nl - s) + 1 : n;
        if (end - pos >= bl + 2 && s[pos] == '-' && s[pos + 1] == '-' && !memcmp(s + pos + 2, b, bl)) {
            if (in)
                walk_entity(w, s + start, pos > start ? pos - start : 0);
            if (end - pos >= bl + 4 && s[pos + bl + 2] == '-' && s[pos + bl + 3] == '-')
                return;
            in = 1;
            start = end;
        }
        pos = end;
    }
    if (in)
        walk_entity(w, s + start, n - start);
}

/* the header block notmuch prints for a message (also inside a
 * message/rfc822 part) */
static void print_headers(Hdr *h, long date, const char *tags) {
    static const char *names[] = {"Subject", "From", "To", "Cc", "Date"};
    char *from = mime_header_get(h, "From"), *d, name[128], rel[32];
    size_t i;

    puts("\fheader{");
    d = mime_decode_header(from ? from : "");
    display_name(d, name, sizeof name);
    relative_date(date, rel, sizeof rel);
    printf("%s (%s) (%s)\n", name, rel, tags ? tags : "");
    free(d);
    free(from);
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        char *v = mime_header_get(h, names[i]);
        if (!v)
            continue;
        if (*v || i != 3) { /* Cc only when present */
            d = i == 4 ? strdup(v) : mime_decode_header(v);
            printf("%s: %s\n", names[i], d);
            free(d);
        }
        free(v);
    }
    puts("\fheader}");
}

/* decoded text of a text part body, UTF-8 */
static char *decode_text_part(const char *ct, const char *cte, const char *s, size_t n) {
    char *dec = mime_decode_transfer_encoding(cte, s, n), *cs = ct ? mime_parameter(ct, "charset") : NULL, *u = NULL;

    mime_convert_to_utf8(&u, cs, dec, arrlenu(dec));
    arrfree(dec);
    free(cs);
    return u;
}

/* one entity: header block, then per its type; ids are assigned in the
 * order notmuch does (this part, then its children) */
static void walk_entity(Walk *w, const char *s, size_t n) {
    Hdr *h = NULL;
    size_t bo = mime_parse_headers(s, n, &h);
    char *ct = mime_header_get(h, "Content-Type"), *cte = mime_header_get(h, "Content-Transfer-Encoding"), *cd = mime_header_get(h, "Content-Disposition"),
         *fn = NULL, *b, *u, *cid, type[128] = "text/plain";
    int id = w->next++, ismulti, isrfc, istext, isatt;

    if (ct)
        mime_media_type(ct, type, sizeof type);
    if (cd)
        fn = mime_parameter(cd, "filename");
    if (!fn && ct)
        fn = mime_parameter(ct, "name");
    ismulti = !strncmp(type, "multipart/", 10);
    isrfc = !strcmp(type, "message/rfc822");
    istext = !strncmp(type, "text/", 5);
    /* an attachment is what says so, or a named non-text leaf that is not
     * a Content-ID image the HTML references (the same rule the index
     * uses for the attachment tag) */
    cid = mime_header_get(h, "Content-ID");
    isatt = !(!strcmp(type, "application/pkcs7-signature") || !strcmp(type, "application/x-pkcs7-signature") || !strcmp(type, "application/pgp-signature")) &&
            ((cd && !strncasecmp(cd, "attachment", 10)) || (fn && !cid && !ismulti && !isrfc && !istext));
    free(cid);

    if (w->mode == WRaw) {
        if (id == w->want) {
            w->found = 1;
            if (ismulti || isrfc)
                fwrite(s + bo, 1, n - bo, stdout);
            else {
                char *dec = mime_decode_transfer_encoding(cte, s + bo, n - bo);
                fwrite(dec, 1, arrlenu(dec), stdout);
                arrfree(dec);
            }
        } else if (ismulti && w->depth < DepthMax && ct && (b = mime_parameter(ct, "boundary"))) {
            w->depth++;
            walk_multipart(w, s + bo, n - bo, b);
            w->depth--;
            free(b);
        } else if (isrfc && w->depth < DepthMax) {
            char *dec = mime_decode_transfer_encoding(cte, s + bo, n - bo);
            w->depth++;
            walk_entity(w, dec, arrlenu(dec));
            w->depth--;
            arrfree(dec);
        }
    } else if (w->mode == WText) {
        if (ismulti && w->depth < DepthMax && ct && (b = mime_parameter(ct, "boundary"))) {
            w->depth++;
            walk_multipart(w, s + bo, n - bo, b);
            w->depth--;
            free(b);
        } else if (isrfc && w->depth < DepthMax) {
            char *dec = mime_decode_transfer_encoding(cte, s + bo, n - bo);
            w->depth++;
            walk_entity(w, dec, arrlenu(dec));
            w->depth--;
            arrfree(dec);
        } else if (istext && !isatt) {
            u = decode_text_part(ct, cte, s + bo, n - bo);
            if (!strcmp(type, "text/html"))
                mime_html_to_text(&w->htmltext, u, arrlenu(u));
            else
                memcpy(arraddnptr(w->plain, arrlenu(u)), u, arrlenu(u));
            arrfree(u);
        }
    } else if (isatt) {
        char *dn = mime_decode_header(fn ? fn : "");
        printf("\fattachment{ ID: %d, Filename: %s, Content-type: %s\n", id, dn, type);
        printf("Non-text part: %s\n", type);
        puts("\fattachment}");
        free(dn);
    } else if (ismulti) {
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        if (w->depth < DepthMax && ct && (b = mime_parameter(ct, "boundary"))) {
            w->depth++;
            walk_multipart(w, s + bo, n - bo, b);
            w->depth--;
            free(b);
        }
        puts("\fpart}");
    } else if (isrfc) {
        char *dec = mime_decode_transfer_encoding(cte, s + bo, n - bo);
        Hdr *ih = NULL;
        char *dt;
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        if (w->depth < DepthMax) {
            mime_parse_headers(dec, arrlenu(dec), &ih);
            dt = mime_header_get(ih, "Date");
            print_headers(ih, dt ? mime_parse_date(dt) : 0, NULL);
            free(dt);
            arrfree(ih);
            puts("\fbody{");
            w->depth++;
            walk_entity(w, dec, arrlenu(dec));
            w->depth--;
            puts("\fbody}");
        }
        puts("\fpart}");
        arrfree(dec);
    } else if (istext) {
        printf("\fpart{ ID: %d, Content-type: %s\n", id, type);
        if (strcmp(type, "text/html") || w->html) {
            u = decode_text_part(ct, cte, s + bo, n - bo);
            print_body(u, arrlenu(u));
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

static char *message_tags(sqlite3 *db, sqlite3_int64 id) {
    sqlite3_stmt *st;
    char *out = NULL;

    sqlite3_prepare_v2(db, "SELECT name FROM tag WHERE msg=? ORDER BY name", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (out)
            append_string(&out, " ");
        append_string(&out, (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    arrput(out, '\0');
    return out;
}

/* one message as an mbox entry: a "From " separator line, the message,
 * body lines that would read as separators escaped mboxrd-style (">From "),
 * a blank line after — what `git am` and every mbox reader expect */
static void print_mbox(const char *buf, size_t n, long date) {
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

static char *parse_show_options(int argc, char **argv, SOpts *o, const char *cmd) {
    char *q = NULL;
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                if (q)
                    append_string(&q, " ");
                append_string(&q, argv[i]);
            }
            break;
        }
        if (!strncmp(a, "--format=", 9))
            o->format = a + 9;
        else if (!strncmp(a, "--part=", 7))
            o->part = atoi(a + 7);
        else if (!strcmp(a, "--include-html"))
            o->html = 1;
        else if (!strcmp(a, "--entire-thread") || !strncmp(a, "--entire-thread=", 16))
            o->entire = 1;
        else if (!strncmp(a, "--reply-to=", 11))
            o->replyto = a + 11;
        else if (!strncmp(a, "--", 2)) {
            fprintf(stderr, "hml %s: unknown option %s\n", cmd, a);
            return NULL;
        } else {
            if (q)
                append_string(&q, " ");
            append_string(&q, a);
        }
    }
    arrput(q, '\0');
    return q;
}

int show_main(int argc, char **argv) {
    SOpts o = {"text", "sender", 0, 0, 0};
    Query c;
    sqlite3 *db;
    Hit *hs;
    char *q, *err = NULL, dberr[256], *buf;
    size_t n;
    ptrdiff_t i;
    int raw, mbox;

    if (!(q = parse_show_options(argc, argv, &o, "show")))
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
    if (query_compile(q, &c, &err) < 0) {
        arrfree(q);
        return fail("show", err);
    }
    arrfree(q);
    if (!(db = db_open(dberr, sizeof dberr))) {
        query_free(&c);
        return fail("show", strdup(dberr));
    }
    if (o.entire) { /* every message of every thread that has a hit */
        Query t = {NULL, NULL, NULL};
        append_string(&t.sql, "msg.thread IN (SELECT thread FROM msg WHERE (");
        append_string(&t.sql, c.sql);
        append_string(&t.sql, "))");
        arrput(t.sql, '\0');
        t.params = c.params;
        hs = find_hits(db, &t, 0, &err);
        arrfree(t.sql);
    } else
        hs = find_hits(db, &c, 0, &err);
    query_free(&c);
    if (err) {
        sqlite3_close(db);
        return fail("show", err);
    }
    if (!arrlen(hs)) {
        sqlite3_close(db);
        free_hits(hs);
        return fail("show", strdup("no messages match"));
    }
    for (i = 0; i < (raw ? 1 : arrlen(hs)); i++) {
        Walk w = {WShow, o.html, o.part, 0, 1, 0, NULL, NULL};
        if (read_file(hs[i].path, &buf, &n) < 0) {
            if (raw) {
                sqlite3_close(db);
                free_hits(hs);
                return fail("show", strdup("cannot read message file"));
            }
            continue; /* vanished since the index was written */
        }
        if (mbox) {
            print_mbox(buf, n, hs[i].date);
        } else if (raw && !o.part) {
            fwrite(buf, 1, n, stdout);
        } else if (raw) {
            w.mode = WRaw;
            walk_entity(&w, buf, n);
            if (!w.found) {
                arrfree(buf);
                sqlite3_close(db);
                free_hits(hs);
                return fail("show", strdup("no such part"));
            }
        } else {
            Hdr *h = NULL;
            char *tags = message_tags(db, hs[i].id);
            mime_parse_headers(buf, n, &h);
            printf("\fmessage{ id:%s depth:0 match:1 excluded:0 filename:%s\n", hs[i].mid, hs[i].path);
            print_headers(h, hs[i].date, tags);
            puts("\fbody{");
            walk_entity(&w, buf, n);
            puts("\fbody}");
            puts("\fmessage}");
            arrfree(h);
            arrfree(tags);
        }
        arrfree(buf);
    }
    sqlite3_close(db);
    free_hits(hs);
    return 0;
}

/* --- hml reply ----------------------------------------------------------- */

static int same_address(const char *a, const char *b) {
    char x[256], y[256];

    mime_bare_address(a, x, sizeof x);
    mime_bare_address(b, y, sizeof y);
    return !strcmp(x, y);
}

static int is_own_address(const char *m) {
    int k;

    for (k = 0; k < naccounts; k++)
        if (same_address(m, accounts[k].user))
            return 1;
    return 0;
}

static int in_list(char **l, const char *m) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(l); i++)
        if (same_address(l[i], m))
            return 1;
    return 0;
}

static void free_list(char **l) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(l); i++)
        free(l[i]);
    arrfree(l);
}

/* a decoded header split into mailboxes; empty when absent */
static char **header_mailboxes(Hdr *h, const char *name) {
    char *v = mime_header_get(h, name), *d, **l = NULL;

    if (!v)
        return NULL;
    d = mime_decode_header(v);
    mime_split_addresses(d, &l);
    free(d);
    free(v);
    return l;
}

static void print_address_list(const char *name, char **l) {
    ptrdiff_t i;

    if (!arrlen(l))
        return;
    printf("%s: ", name);
    for (i = 0; i < arrlen(l); i++)
        printf("%s%s", i ? ", " : "", l[i]);
    putchar('\n');
}

int reply_main(int argc, char **argv) {
    SOpts o = {"text", "sender", 0, 0, 0};
    Query c;
    sqlite3 *db;
    Hit *hs;
    Hdr *h = NULL;
    Walk w = {WText, 0, 0, 0, 1, 0, NULL, NULL};
    char *q, *err = NULL, dberr[256], *buf, *v, *from, *subject, *date, *mid, *me = NULL, **to = NULL, **cc = NULL, **orig, **l, *text;
    const char *acct = NULL, *p;
    size_t n;
    ptrdiff_t i;
    int all, k;

    if (!(q = parse_show_options(argc, argv, &o, "reply")))
        return 2;
    if (!*q) {
        fputs("usage: hml reply [--reply-to=sender|all] [--] <query>\n", stderr);
        arrfree(q);
        return 2;
    }
    all = !strcmp(o.replyto, "all");
    if (query_compile(q, &c, &err) < 0) {
        arrfree(q);
        return fail("reply", err);
    }
    arrfree(q);
    if (!(db = db_open(dberr, sizeof dberr))) {
        query_free(&c);
        return fail("reply", strdup(dberr));
    }
    hs = find_hits(db, &c, 1, &err); /* newest first: reply to the latest */
    query_free(&c);
    sqlite3_close(db);
    if (err)
        return fail("reply", err);
    if (!arrlen(hs)) {
        free_hits(hs);
        return fail("reply", strdup("no messages match"));
    }
    if (read_file(hs[0].path, &buf, &n) < 0) {
        free_hits(hs);
        return fail("reply", strdup("cannot read message file"));
    }
    mime_parse_headers(buf, n, &h);

    /* the account the message lives in is the one replying */
    for (k = 0; k < naccounts; k++)
        if (!strncmp(hs[0].box, accounts[k].name, strlen(accounts[k].name)) && hs[0].box[strlen(accounts[k].name)] == '/')
            acct = accounts[k].user;
    if (!acct && naccounts)
        acct = accounts[0].user;

    /* From: our address as the original addressed it (keeps the display
     * name the sender used for us), else bare */
    orig = header_mailboxes(h, "To");
    l = header_mailboxes(h, "Cc");
    for (i = 0; i < arrlen(l); i++)
        arrput(orig, l[i]);
    arrfree(l);
    for (i = 0; i < arrlen(orig); i++)
        if (acct && same_address(orig[i], acct)) {
            me = strdup(orig[i]);
            break;
        }
    if (!me)
        me = strdup(acct ? acct : "");

    /* To: the sender (Reply-To wins); all: plus everyone else, minus us */
    l = header_mailboxes(h, "Reply-To");
    if (!arrlen(l)) {
        free_list(l);
        l = header_mailboxes(h, "From");
    }
    for (i = 0; i < arrlen(l); i++)
        if (!in_list(to, l[i]))
            arrput(to, strdup(l[i]));
    free_list(l);
    if (all) {
        for (i = 0; i < arrlen(orig); i++)
            if (!is_own_address(orig[i]) && !in_list(to, orig[i]) && !in_list(cc, orig[i]))
                arrput(cc, strdup(orig[i]));
    }
    free_list(orig);

    from = (v = mime_header_get(h, "From")) ? mime_decode_header(v) : strdup("");
    free(v);
    subject = (v = mime_header_get(h, "Subject")) ? mime_decode_header(v) : strdup("");
    free(v);
    date = mime_header_get(h, "Date");
    mid = mime_header_get(h, "Message-ID");

    printf("From: %s\n", me);
    p = subject;
    while (isspace((unsigned char)*p))
        p++;
    printf("Subject: %s%s\n", strncasecmp(p, "re:", 3) ? "Re: " : "", p);
    print_address_list("To", to);
    print_address_list("Cc", cc);
    if (mid && *mid)
        printf("In-Reply-To: %s\n", mid);
    v = mime_header_get(h, "References");
    if (!v || !*v) {
        free(v);
        v = mime_header_get(h, "In-Reply-To");
    }
    if ((v && *v) || (mid && *mid))
        printf("References: %s%s%s\n", v && *v ? v : "", v && *v && mid && *mid ? " " : "", mid && *mid ? mid : "");
    free(v);
    putchar('\n');

    /* the quoted text: plain parts, else the html stripped to text */
    walk_entity(&w, buf, n);
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
    free_list(to);
    free_list(cc);
    free(me);
    free(from);
    free(subject);
    free(date);
    free(mid);
    arrfree(h);
    arrfree(buf);
    free_hits(hs);
    return 0;
}

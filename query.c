/* query.c - "hml search", "hml count", "hml tags": notmuch-style queries
 * compiled to SQL over the index index.c maintains. Terms: bare words
 * (any text), subject: from: to: attachment: body: (full text), tag: id:
 * thread: path: date:; and/or/not, parentheses, implicit and. */
#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <stb_ds.h>

#include "hml.h"

enum { NAnd, NOr, NNot, NTerm };

typedef struct Node {
    int op;
    struct Node *l, *r;
    char *prefix; /* NULL for a bare word */
    char *val;
} Node;

typedef struct {
    const char *s; /* query text, cursor */
    char *err;     /* first parse/compile error, malloc'd */
} Parser;

typedef struct {
    char *sql;     /* stb char array, the WHERE expression */
    char **params; /* stb array of bound strings, in ? order */
    char *err;
} Comp;

static void sadd(char **b, const char *t) {
    memcpy(arraddnptr(*b, strlen(t)), t, strlen(t));
}

static void seterr(char **err, const char *fmt, const char *arg) {
    char buf[256];

    if (*err)
        return;
    snprintf(buf, sizeof buf, fmt, arg);
    *err = strdup(buf);
}

/* --- parsing ----------------------------------------------------------- */

/* one token: "(", ")", or a word (prefix:value, quotes kept); NULL at end */
static char *token(Parser *p) {
    const char *s = p->s, *st;
    char *t;
    int q = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    if (!*s) {
        p->s = s;
        return NULL;
    }
    st = s;
    if (*s == '(' || *s == ')')
        s++;
    else
        for (; *s; s++) {
            if (*s == '"')
                q = !q;
            else if (!q && (*s == ' ' || *s == '\t' || *s == '\n' ||
                            *s == '(' || *s == ')'))
                break;
        }
    p->s = s;
    t = malloc((size_t)(s - st) + 1);
    memcpy(t, st, (size_t)(s - st));
    t[s - st] = '\0';
    return t;
}

static char *peek(Parser *p) {
    const char *save = p->s;
    char *t = token(p);

    p->s = save;
    return t;
}

static int iskw(const char *t, const char *kw) {
    return t && !strcasecmp(t, kw);
}

static Node *node(int op, Node *l, Node *r) {
    Node *n = calloc(1, sizeof *n);

    n->op = op;
    n->l = l;
    n->r = r;
    return n;
}

static Node *term(char *t) {
    Node *n = node(NTerm, NULL, NULL);
    char *colon = strchr(t, ':');

    /* a colon before any quote splits prefix from value */
    if (colon && (!strchr(t, '"') || colon < strchr(t, '"')) && colon > t) {
        *colon = '\0';
        n->prefix = t;
        n->val = strdup(colon + 1);
    } else
        n->val = t;
    return n;
}

static Node *parseor(Parser *p);

static Node *parsenot(Parser *p) {
    char *t = peek(p);
    Node *n;

    if (!t) {
        seterr(&p->err, "unexpected end of query", "");
        return NULL;
    }
    if (iskw(t, "not")) {
        free(t);
        free(token(p));
        n = parsenot(p);
        return n ? node(NNot, n, NULL) : NULL;
    }
    if (!strcmp(t, "(")) {
        free(t);
        free(token(p));
        n = parseor(p);
        t = token(p);
        if (!t || strcmp(t, ")")) {
            seterr(&p->err, "missing )", "");
            free(t);
            return n;
        }
        free(t);
        return n;
    }
    if (!strcmp(t, ")") || iskw(t, "and") || iskw(t, "or")) {
        seterr(&p->err, "unexpected '%s'", t);
        free(t);
        return NULL;
    }
    free(t);
    return term(token(p));
}

static Node *parseand(Parser *p) {
    Node *l = parsenot(p), *r;
    char *t;

    while (l && (t = peek(p))) {
        if (!strcmp(t, ")") || iskw(t, "or")) {
            free(t);
            break;
        }
        if (iskw(t, "and"))
            free(token(p));
        free(t);
        if (!(r = parsenot(p)))
            break;
        l = node(NAnd, l, r);
    }
    return l;
}

static Node *parseor(Parser *p) {
    Node *l = parseand(p), *r;
    char *t;

    while (l && (t = peek(p)) && iskw(t, "or")) {
        free(t);
        free(token(p));
        if (!(r = parseand(p)))
            break;
        l = node(NOr, l, r);
    }
    return l;
}

static Node *parse(const char *q, char **err) {
    Parser p = {q, NULL};
    Node *n = parseor(&p);
    char *t;

    if (!p.err && (t = token(&p))) {
        seterr(&p.err, "unexpected '%s'", t);
        free(t);
    }
    *err = p.err;
    return n;
}

static void freenode(Node *n) {
    if (!n)
        return;
    freenode(n->l);
    freenode(n->r);
    free(n->prefix);
    free(n->val);
    free(n);
}

/* --- dates ------------------------------------------------------------- */

/* one end of a date: range; end=1 asks for the last second of the period */
static int datepoint(const char *s, int end, long *out) {
    time_t now = time(NULL);
    struct tm tm;
    long n;
    char *e;
    int y, m, d;

    localtime_r(&now, &tm);
    if (!*s || !strcmp(s, "now")) {
        *out = end ? (long)now : (*s ? (long)now : 0);
        return 0;
    }
    if (!strcmp(s, "today") || !strcmp(s, "yesterday")) {
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        if (s[0] == 'y')
            tm.tm_mday--;
        tm.tm_isdst = -1;
        *out = (long)mktime(&tm) + (end ? 86399 : 0);
        return 0;
    }
    n = strtol(s, &e, 10);
    if (e != s && *e && !isdigit((unsigned char)*e) && *e != '-') {
        long unit;
        if (!strcmp(e, "h") || !strcmp(e, "hour") || !strcmp(e, "hours"))
            unit = 3600;
        else if (!strcmp(e, "d") || !strcmp(e, "day") || !strcmp(e, "days"))
            unit = 86400;
        else if (!strcmp(e, "w") || !strcmp(e, "week") || !strcmp(e, "weeks"))
            unit = 7 * 86400;
        else if (!strcmp(e, "m") || !strcmp(e, "month") || !strcmp(e, "months"))
            unit = 30 * 86400;
        else if (!strcmp(e, "y") || !strcmp(e, "year") || !strcmp(e, "years"))
            unit = 365 * 86400;
        else
            return -1;
        *out = (long)now - n * unit;
        return 0;
    }
    /* YYYY, YYYY-MM, YYYY-MM-DD */
    y = (int)n;
    m = d = 0;
    if (*e == '-') {
        m = (int)strtol(e + 1, &e, 10);
        if (*e == '-')
            d = (int)strtol(e + 1, &e, 10);
    }
    if (*e || y < 1970 || m < 0 || m > 12 || d < 0 || d > 31)
        return -1;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = m ? m - 1 : 0;
    tm.tm_mday = d ? d : 1;
    tm.tm_isdst = -1;
    if (end) { /* first second of the next period, minus one */
        if (d)
            tm.tm_mday++;
        else if (m)
            tm.tm_mon++;
        else
            tm.tm_year++;
    }
    *out = (long)mktime(&tm) - (end ? 1 : 0);
    return 0;
}

static int daterange(const char *v, long *from, long *to) {
    const char *dots = strstr(v, "..");
    char a[64], b[64];

    if (dots) {
        snprintf(a, sizeof a, "%.*s", (int)(dots - v), v);
        snprintf(b, sizeof b, "%s", dots + 2);
    } else {
        snprintf(a, sizeof a, "%s", v);
        snprintf(b, sizeof b, "%s", v);
        if (isdigit((unsigned char)*v) &&
            !isdigit((unsigned char)v[strlen(v) - 1]))
            b[0] = '\0'; /* "7d" alone means 7d..now */
    }
    return datepoint(a, 0, from) < 0 || datepoint(b, 1, to) < 0 ? -1 : 0;
}

/* --- compiling --------------------------------------------------------- */

/* an FTS5 MATCH expression for one term: col : "phrase" [*] */
static char *ftsmatch(const char *col, const char *v) {
    char *m = NULL;
    size_t n = strlen(v);
    int prefix = 0;

    if (n && v[n - 1] == '*') {
        prefix = 1;
        n--;
    }
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        v++;
        n -= 2;
    }
    if (col) {
        sadd(&m, col);
        sadd(&m, " : ");
    }
    arrput(m, '"');
    for (; n; n--, v++) {
        if (*v == '"')
            arrput(m, '"');
        arrput(m, *v);
    }
    arrput(m, '"');
    if (prefix)
        arrput(m, '*');
    arrput(m, '\0');
    return m;
}

static void bindparam(Comp *c, char *v) { /* takes ownership of v */
    arrput(c->params, v);
    sadd(&c->sql, "?");
}

static void compterm(Comp *c, const Node *n) {
    const char *p = n->prefix, *v = n->val;
    char buf[64];
    long a, b;

    if (!p && !strcmp(v, "*")) {
        sadd(&c->sql, "1");
    } else if (!p || !strcmp(p, "subject") || !strcmp(p, "from") ||
               !strcmp(p, "to") || !strcmp(p, "attachment") ||
               !strcmp(p, "body")) {
        const char *col = !p                         ? NULL
                          : !strcmp(p, "from")       ? "sender"
                          : !strcmp(p, "to")         ? "rcpt"
                          : !strcmp(p, "attachment") ? "attach"
                                                     : p;
        char *m = ftsmatch(col, v);
        if (!strcmp(m, "\"\"") || strstr(m, ": \"\"")) {
            seterr(&c->err, "empty search term", "");
            arrfree(m);
            return;
        }
        sadd(&c->sql, "id IN (SELECT rowid FROM fts WHERE fts MATCH ");
        bindparam(c, strdup(m));
        sadd(&c->sql, ")");
        arrfree(m);
    } else if (!strcmp(p, "tag")) {
        sadd(&c->sql, "id IN (SELECT msg FROM tag WHERE name=");
        bindparam(c, strdup(v));
        sadd(&c->sql, ")");
    } else if (!strcmp(p, "id")) {
        size_t k = strlen(v);
        char *m = strdup(v);
        if (k >= 2 && v[0] == '<' && v[k - 1] == '>') {
            memmove(m, m + 1, k - 2);
            m[k - 2] = '\0';
        }
        sadd(&c->sql, "mid=");
        bindparam(c, m);
    } else if (!strcmp(p, "thread")) {
        snprintf(buf, sizeof buf, "thread=%llu",
                 (unsigned long long)strtoull(v, NULL, 16));
        sadd(&c->sql, buf);
    } else if (!strcmp(p, "path") || !strcmp(p, "folder")) {
        size_t k = strlen(v);
        if (!strcmp(v, "**") || !strcmp(v, "*")) {
            sadd(&c->sql, "1");
        } else if (k > 3 && !strcmp(v + k - 3, "/**")) {
            char *box = strdup(v), *glob = malloc(k);
            box[k - 3] = '\0';
            snprintf(glob, k, "%s/*", box);
            sadd(&c->sql, "id IN (SELECT msg FROM file WHERE box=");
            bindparam(c, box);
            sadd(&c->sql, " OR box GLOB ");
            bindparam(c, glob);
            sadd(&c->sql, ")");
        } else {
            sadd(&c->sql, "id IN (SELECT msg FROM file WHERE box=");
            bindparam(c, strdup(v));
            sadd(&c->sql, ")");
        }
    } else if (!strcmp(p, "date")) {
        if (daterange(v, &a, &b) < 0) {
            seterr(&c->err, "bad date '%s'", v);
            return;
        }
        snprintf(buf, sizeof buf, "date BETWEEN %ld AND %ld", a, b);
        sadd(&c->sql, buf);
    } else
        seterr(&c->err, "unknown prefix '%s:'", p);
}

static void compile(Comp *c, const Node *n) {
    switch (n->op) {
    case NTerm:
        compterm(c, n);
        break;
    case NNot:
        sadd(&c->sql, "NOT (");
        compile(c, n->l);
        sadd(&c->sql, ")");
        break;
    default:
        sadd(&c->sql, "(");
        compile(c, n->l);
        sadd(&c->sql, n->op == NAnd ? " AND " : " OR ");
        compile(c, n->r);
        sadd(&c->sql, ")");
    }
}

/* query text -> WHERE expression over msg; NULL with a message on error */
static int compilequery(const char *q, Comp *c, char **err) {
    Node *n = parse(q, err);

    memset(c, 0, sizeof *c);
    if (!n || *err) {
        if (!*err)
            *err = strdup("empty query");
        freenode(n);
        return -1;
    }
    compile(c, n);
    freenode(n);
    arrput(c->sql, '\0');
    if (c->err) {
        *err = c->err;
        c->err = NULL;
        return -1;
    }
    return 0;
}

static void compfree(Comp *c) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(c->params); i++)
        free(c->params[i]);
    arrfree(c->params);
    arrfree(c->sql);
}

/* prepare sql with the query's WHERE spliced in at "%s", params bound */
static sqlite3_stmt *prepq(sqlite3 *db, const char *fmt, const Comp *c,
                           const char *tail, char **err) {
    char *sql = NULL;
    sqlite3_stmt *st;
    const char *pct = strstr(fmt, "%s");
    ptrdiff_t i;

    memcpy(arraddnptr(sql, pct - fmt), fmt, (size_t)(pct - fmt));
    sadd(&sql, c->sql);
    sadd(&sql, pct + 2);
    sadd(&sql, tail);
    arrput(sql, '\0');
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        seterr(err, "%s", sqlite3_errmsg(db));
        arrfree(sql);
        return NULL;
    }
    arrfree(sql);
    for (i = 0; i < arrlen(c->params); i++)
        sqlite3_bind_text(st, (int)i + 1, c->params[i], -1, SQLITE_STATIC);
    return st;
}

/* --- output helpers ---------------------------------------------------- */

static void jsonstr(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

/* the display name of a From header, or its address */
static void dispname(const char *from, char *out, size_t cap) {
    const char *lt = strchr(from, '<'), *s = from, *e;
    size_t n;

    if (lt && lt > from) {
        e = lt;
        while (e > s && (e[-1] == ' ' || e[-1] == '"'))
            e--;
        while (s < e && (*s == ' ' || *s == '"'))
            s++;
        if (e > s) {
            n = (size_t)(e - s);
            snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), s);
            return;
        }
    }
    if (lt) {
        s = lt + 1;
        e = strchr(s, '>');
        n = e ? (size_t)(e - s) : strlen(s);
        snprintf(out, cap, "%.*s", (int)n, s);
        return;
    }
    snprintf(out, cap, "%s", from);
}

/* notmuch's relative dates: "Today 10:12", "Yest. 21:12", "Mon. 10:12",
 * "August 21", "2025-08-21" */
static void reldate(long t, char *out, size_t cap) {
    time_t now = time(NULL), tt = t;
    struct tm tm, tn;
    char day[16];

    localtime_r(&tt, &tm);
    localtime_r(&now, &tn);
    if (t > now + 60 || tm.tm_year != tn.tm_year) {
        strftime(out, cap, "%Y-%m-%d", &tm);
        return;
    }
    if (tm.tm_yday == tn.tm_yday)
        strftime(out, cap, "Today %H:%M", &tm);
    else if (tm.tm_yday == tn.tm_yday - 1)
        strftime(out, cap, "Yest. %H:%M", &tm);
    else if (now - t < 7 * 86400)
        strftime(out, cap, "%a. %H:%M", &tm);
    else {
        strftime(day, sizeof day, "%B", &tm);
        snprintf(out, cap, "%s %d", day, tm.tm_mday);
    }
}

/* --- search ------------------------------------------------------------ */

typedef struct {
    sqlite3_int64 thread;
    long newest, oldest;
    int matched;
    char *authors; /* stb char array */
    char *subject;
    sqlite3_int64 *ids; /* matched message ids */
} Thread;

typedef struct {
    const char *output, *format;
    long limit, offset;
    int oldest;
} Opts;

static int cmpnewest(const void *a, const void *b) {
    const Thread *x = a, *y = b;
    return x->newest < y->newest ? 1 : x->newest > y->newest ? -1 : 0;
}

static int cmpoldest(const void *a, const void *b) {
    const Thread *x = a, *y = b;
    return x->oldest > y->oldest ? 1 : x->oldest < y->oldest ? -1 : 0;
}

static void addauthor(Thread *t, const char *from) {
    char name[128];
    size_t n = strlen(t->authors ? t->authors : "");

    dispname(from, name, sizeof name);
    if (n) { /* already listed? */
        const char *p = t->authors;
        size_t k = strlen(name);
        while ((p = strstr(p, name))) {
            if ((p == t->authors || !strncmp(p - 2, ", ", 2)) &&
                (p[k] == '\0' || !strncmp(p + k, ", ", 2)))
                return;
            p++;
        }
        arrsetlen(t->authors, arrlen(t->authors) - 1);
        sadd(&t->authors, ", ");
    }
    sadd(&t->authors, name);
    arrput(t->authors, '\0');
}

static void threadtags(sqlite3 *db, const Thread *t, char ***tags) {
    sqlite3_stmt *st;
    ptrdiff_t i, k;

    sqlite3_prepare_v2(db, "SELECT name FROM tag WHERE msg=? ORDER BY name", -1,
                       &st, NULL);
    for (i = 0; i < arrlen(t->ids); i++) {
        sqlite3_bind_int64(st, 1, t->ids[i]);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *n = (const char *)sqlite3_column_text(st, 0);
            for (k = 0; k < arrlen(*tags); k++)
                if (!strcmp((*tags)[k], n))
                    break;
            if (k == arrlen(*tags))
                arrput(*tags, strdup(n));
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int summary(sqlite3 *db, const Comp *c, const Opts *o, char **err) {
    sqlite3_stmt *st, *total;
    Thread *ts = NULL, *t;
    ptrdiff_t i, k, n;
    int json = !strcmp(o->format, "json"), threadsonly, rc;
    char date[32];

    if (!(st = prepq(db,
                     "SELECT id,thread,date,sender,subject FROM msg WHERE %s",
                     c, " ORDER BY thread,date", err)))
        return -1;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        sqlite3_int64 th = sqlite3_column_int64(st, 1);
        long d = (long)sqlite3_column_int64(st, 2);
        const char *from = (const char *)sqlite3_column_text(st, 3);
        const char *subj = (const char *)sqlite3_column_text(st, 4);
        if (!arrlen(ts) || arrlast(ts).thread != th) {
            Thread nt = {th, d, d, 0, NULL, NULL, NULL};
            arrput(ts, nt);
        }
        t = &arrlast(ts);
        t->matched++;
        arrput(t->ids, id);
        addauthor(t, from);
        /* the subject shown is the message the sort order puts first */
        if (d >= t->newest || !t->subject) {
            t->newest = d;
            if (!o->oldest || !t->subject) {
                free(t->subject);
                t->subject = strdup(subj);
            }
        }
        if (d < t->oldest)
            t->oldest = d;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        seterr(err, "%s", sqlite3_errmsg(db));
        return -1;
    }
    qsort(ts, (size_t)arrlen(ts), sizeof *ts,
          o->oldest ? cmpoldest : cmpnewest);
    n = arrlen(ts);
    if (o->offset < n)
        n -= o->offset;
    else
        n = 0;
    if (o->limit >= 0 && o->limit < n)
        n = o->limit;
    threadsonly = !strcmp(o->output, "threads");
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM msg WHERE thread=?", -1,
                       &total, NULL);
    if (json)
        putchar('[');
    for (i = 0; i < n; i++) {
        char **tags = NULL;
        int tot;
        t = &ts[o->offset + i];
        if (threadsonly) {
            if (json) {
                printf(i ? ",\n" : "\n");
                printf("\"%016llx\"", (unsigned long long)t->thread);
            } else
                printf("thread:%016llx\n", (unsigned long long)t->thread);
            continue;
        }
        sqlite3_bind_int64(total, 1, t->thread);
        tot = sqlite3_step(total) == SQLITE_ROW ? sqlite3_column_int(total, 0)
                                                : t->matched;
        sqlite3_reset(total);
        threadtags(db, t, &tags);
        qsort(tags, (size_t)arrlen(tags), sizeof *tags, cmpstr);
        reldate(o->oldest ? t->oldest : t->newest, date, sizeof date);
        if (json) {
            printf(i ? ",\n{" : "\n{");
            printf("\"thread\": \"%016llx\", \"timestamp\": %ld, "
                   "\"date_relative\": ",
                   (unsigned long long)t->thread,
                   o->oldest ? t->oldest : t->newest);
            jsonstr(date);
            printf(", \"matched\": %d, \"total\": %d, \"authors\": ",
                   t->matched, tot);
            jsonstr(t->authors);
            printf(", \"subject\": ");
            jsonstr(t->subject);
            printf(", \"tags\": [");
            for (k = 0; k < arrlen(tags); k++) {
                printf(k ? ", " : "");
                jsonstr(tags[k]);
            }
            printf("]}");
        } else {
            printf("thread:%016llx %12s [%d/%d] %s; %s (",
                   (unsigned long long)t->thread, date, t->matched, tot,
                   t->authors, t->subject);
            for (k = 0; k < arrlen(tags); k++)
                printf("%s%s", k ? " " : "", tags[k]);
            puts(")");
        }
        for (k = 0; k < arrlen(tags); k++)
            free(tags[k]);
        arrfree(tags);
    }
    if (json)
        puts(n ? "\n]" : "]");
    sqlite3_finalize(total);
    for (i = 0; i < arrlen(ts); i++) {
        arrfree(ts[i].authors);
        arrfree(ts[i].ids);
        free(ts[i].subject);
    }
    arrfree(ts);
    return 0;
}

/* messages / files / tags: one string per line, or a JSON array */
static int listing(sqlite3 *db, const Comp *c, const Opts *o, char **err) {
    sqlite3_stmt *st;
    char tail[128], line[8192];
    const char *sql;
    int json = !strcmp(o->format, "json"), files = 0, rc;
    long i = 0;

    snprintf(tail, sizeof tail, " LIMIT %ld OFFSET %ld",
             o->limit < 0 ? -1L : o->limit, o->offset);
    if (!strcmp(o->output, "messages")) {
        sql = o->oldest ? "SELECT mid FROM msg WHERE %s ORDER BY date"
                        : "SELECT mid FROM msg WHERE %s ORDER BY date DESC";
    } else if (!strcmp(o->output, "files")) {
        sql = o->oldest ? "SELECT box,sub,name FROM file JOIN msg ON msg.id="
                          "file.msg WHERE %s ORDER BY date,box"
                        : "SELECT box,sub,name FROM file JOIN msg ON msg.id="
                          "file.msg WHERE %s ORDER BY date DESC,box";
        files = 1;
    } else if (!strcmp(o->output, "tags")) {
        sql = "SELECT DISTINCT name FROM tag WHERE msg IN (SELECT id FROM msg"
              " WHERE %s) ORDER BY name";
    } else {
        seterr(err, "unknown output '%s'", o->output);
        return -1;
    }
    if (!(st = prepq(db, sql, c, tail, err)))
        return -1;
    if (json)
        putchar('[');
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (files) {
            const char *box = s, *slash = strchr(box, '/');
            char root[4096];
            int a;
            for (a = 0; a < naccounts; a++)
                if (slash &&
                    (size_t)(slash - box) == strlen(accounts[a].name) &&
                    !strncmp(box, accounts[a].name, (size_t)(slash - box)))
                    break;
            if (a == naccounts)
                continue; /* box of an account no longer configured */
            expand(accounts[a].maildir, root, sizeof root);
            snprintf(line, sizeof line, "%s/%s/%s/%s", root, slash + 1,
                     sqlite3_column_text(st, 1), sqlite3_column_text(st, 2));
            s = line;
        } else if (!strcmp(o->output, "messages") && !json) {
            snprintf(line, sizeof line, "id:%s", s);
            s = line;
        }
        if (json) {
            printf(i ? ",\n" : "\n");
            jsonstr(s);
        } else
            puts(s);
        i++;
    }
    sqlite3_finalize(st);
    if (json)
        puts(i ? "\n]" : "]");
    if (rc != SQLITE_DONE) {
        seterr(err, "%s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* --output=X / --limit=N style options; the rest joined is the query */
static char *getopts(int argc, char **argv, Opts *o, const char *cmd) {
    char *q = NULL;
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--output=", 9))
            o->output = a + 9;
        else if (!strncmp(a, "--format=", 9))
            o->format = a + 9;
        else if (!strncmp(a, "--limit=", 8))
            o->limit = atol(a + 8);
        else if (!strncmp(a, "--offset=", 9))
            o->offset = atol(a + 9);
        else if (!strncmp(a, "--sort=", 7))
            o->oldest = !strcmp(a + 7, "oldest-first");
        else if (!strncmp(a, "--", 2)) {
            fprintf(stderr, "hml %s: unknown option %s\n", cmd, a);
            return NULL;
        } else {
            if (q)
                sadd(&q, " ");
            sadd(&q, a);
        }
    }
    if (!q)
        arrput(q, '\0');
    else
        arrput(q, '\0');
    return q;
}

static int fail(const char *cmd, char *err) {
    fprintf(stderr, "hml %s: %s\n", cmd, err ? err : "error");
    free(err);
    return 2;
}

int searchmain(int argc, char **argv) {
    Opts o = {"summary", "text", -1, 0, 0};
    Comp c;
    sqlite3 *db;
    char *q, *err = NULL, dberr[256];
    int rc;

    if (!(q = getopts(argc, argv, &o, "search")))
        return 2;
    if (!*q) {
        fputs("usage: hml search [--output=summary|threads|messages|files|"
              "tags]\n           [--format=text|json] [--limit=N] [--offset=N]"
              "\n           [--sort=newest-first|oldest-first] <query>\n",
              stderr);
        arrfree(q);
        return 2;
    }
    if (compilequery(q, &c, &err) < 0) {
        arrfree(q);
        return fail("search", err);
    }
    arrfree(q);
    if (!(db = dbopen(dberr, sizeof dberr))) {
        compfree(&c);
        return fail("search", strdup(dberr));
    }
    if (!strcmp(o.output, "summary") || !strcmp(o.output, "threads"))
        rc = summary(db, &c, &o, &err);
    else
        rc = listing(db, &c, &o, &err);
    compfree(&c);
    sqlite3_close(db);
    return rc < 0 ? fail("search", err) : 0;
}

int countmain(int argc, char **argv) {
    Opts o = {"messages", "text", -1, 0, 0};
    Comp c;
    sqlite3 *db;
    sqlite3_stmt *st;
    const char *sql;
    char *q, *err = NULL, dberr[256];

    if (!(q = getopts(argc, argv, &o, "count")))
        return 2;
    if (!*q) {
        arrfree(q);
        q = NULL;
        sadd(&q, "*");
        arrput(q, '\0');
    }
    if (compilequery(q, &c, &err) < 0) {
        arrfree(q);
        return fail("count", err);
    }
    arrfree(q);
    if (!strcmp(o.output, "threads"))
        sql = "SELECT COUNT(DISTINCT thread) FROM msg WHERE %s";
    else if (!strcmp(o.output, "files"))
        sql = "SELECT COUNT(*) FROM file WHERE msg IN (SELECT id FROM msg"
              " WHERE %s)";
    else
        sql = "SELECT COUNT(*) FROM msg WHERE %s";
    if (!(db = dbopen(dberr, sizeof dberr))) {
        compfree(&c);
        return fail("count", strdup(dberr));
    }
    if (!(st = prepq(db, sql, &c, "", &err)) ||
        sqlite3_step(st) != SQLITE_ROW) {
        seterr(&err, "%s", sqlite3_errmsg(db));
        compfree(&c);
        sqlite3_close(db);
        return fail("count", err);
    }
    printf("%lld\n", (long long)sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    compfree(&c);
    sqlite3_close(db);
    return 0;
}

/* every tag in the index, or those of the messages matching a query */
int tagsmain(int argc, char **argv) {
    Opts o = {"tags", "text", -1, 0, 0};
    Comp c;
    sqlite3 *db;
    char *q, *err = NULL, dberr[256];
    int rc;

    if (!(q = getopts(argc, argv, &o, "tags")))
        return 2;
    o.output = "tags";
    if (!*q) {
        arrfree(q);
        q = NULL;
        sadd(&q, "*");
        arrput(q, '\0');
    }
    if (compilequery(q, &c, &err) < 0) {
        arrfree(q);
        return fail("tags", err);
    }
    arrfree(q);
    if (!(db = dbopen(dberr, sizeof dberr))) {
        compfree(&c);
        return fail("tags", strdup(dberr));
    }
    rc = listing(db, &c, &o, &err);
    compfree(&c);
    sqlite3_close(db);
    return rc < 0 ? fail("tags", err) : 0;
}

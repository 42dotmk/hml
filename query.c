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

static void append_string(char **b, const char *t) {
    size_t n = strlen(t);

    if (n) /* memcpy(NULL, …, 0) is UB the optimizer builds on */
        memcpy(arraddnptr(*b, n), t, n);
}

static void set_error(char **err, const char *fmt, const char *arg) {
    char buf[256];

    if (*err)
        return;
    snprintf(buf, sizeof buf, fmt, arg);
    *err = strdup(buf);
}

/* --- parsing ----------------------------------------------------------- */

/* one token: "(", ")", or a word (prefix:value, quotes kept); NULL at end */
static char *next_token(Parser *p) {
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
            else if (!q && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '(' || *s == ')'))
                break;
        }
    p->s = s;
    t = malloc((size_t)(s - st) + 1);
    memcpy(t, st, (size_t)(s - st));
    t[s - st] = '\0';
    return t;
}

static char *peek_token(Parser *p) {
    const char *save = p->s;
    char *t = next_token(p);

    p->s = save;
    return t;
}

static int is_keyword(const char *t, const char *kw) { return t && !strcasecmp(t, kw); }

static Node *new_node(int op, Node *l, Node *r) {
    Node *n = calloc(1, sizeof *n);

    n->op = op;
    n->l = l;
    n->r = r;
    return n;
}

static Node *new_term(char *t) {
    Node *n = new_node(NTerm, NULL, NULL);
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

static Node *parse_or(Parser *p);

static Node *parse_not(Parser *p) {
    char *t = peek_token(p);
    Node *n;

    if (!t) {
        set_error(&p->err, "unexpected end of query", "");
        return NULL;
    }
    if (is_keyword(t, "not")) {
        free(t);
        free(next_token(p));
        n = parse_not(p);
        return n ? new_node(NNot, n, NULL) : NULL;
    }
    if (!strcmp(t, "(")) {
        free(t);
        free(next_token(p));
        n = parse_or(p);
        t = next_token(p);
        if (!t || strcmp(t, ")")) {
            set_error(&p->err, "missing )", "");
            free(t);
            return n;
        }
        free(t);
        return n;
    }
    if (!strcmp(t, ")") || is_keyword(t, "and") || is_keyword(t, "or")) {
        set_error(&p->err, "unexpected '%s'", t);
        free(t);
        return NULL;
    }
    free(t);
    return new_term(next_token(p));
}

static Node *parse_and(Parser *p) {
    Node *l = parse_not(p), *r;
    char *t;

    while (l && (t = peek_token(p))) {
        if (!strcmp(t, ")") || is_keyword(t, "or")) {
            free(t);
            break;
        }
        if (is_keyword(t, "and"))
            free(next_token(p));
        free(t);
        if (!(r = parse_not(p)))
            break;
        l = new_node(NAnd, l, r);
    }
    return l;
}

static Node *parse_or(Parser *p) {
    Node *l = parse_and(p), *r;
    char *t;

    while (l && (t = peek_token(p)) && is_keyword(t, "or")) {
        free(t);
        free(next_token(p));
        if (!(r = parse_and(p)))
            break;
        l = new_node(NOr, l, r);
    }
    return l;
}

static Node *parse_query(const char *q, char **err) {
    Parser p = {q, NULL};
    Node *n = parse_or(&p);
    char *t;

    if (!p.err && (t = next_token(&p))) {
        set_error(&p.err, "unexpected '%s'", t);
        free(t);
    }
    *err = p.err;
    return n;
}

static void free_node(Node *n) {
    if (!n)
        return;
    free_node(n->l);
    free_node(n->r);
    free(n->prefix);
    free(n->val);
    free(n);
}

/* --- dates ------------------------------------------------------------- */

/* one end of a date: range; end=1 asks for the last second of the period */
static int parse_date_point(const char *s, int end, long *out) {
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

static int parse_date_range(const char *v, long *from, long *to) {
    const char *dots = strstr(v, "..");
    char a[64], b[64];

    if (dots) {
        snprintf(a, sizeof a, "%.*s", (int)(dots - v), v);
        snprintf(b, sizeof b, "%s", dots + 2);
    } else {
        snprintf(a, sizeof a, "%s", v);
        snprintf(b, sizeof b, "%s", v);
        if (isdigit((unsigned char)*v) && !isdigit((unsigned char)v[strlen(v) - 1]))
            b[0] = '\0'; /* "7d" alone means 7d..now */
    }
    return parse_date_point(a, 0, from) < 0 || parse_date_point(b, 1, to) < 0 ? -1 : 0;
}

/* --- compiling --------------------------------------------------------- */

/* an FTS5 MATCH expression for one term: col : "phrase" [*] */
static char *fts_match_expression(const char *col, const char *v) {
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
        append_string(&m, col);
        append_string(&m, " : ");
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

static void bind_parameter(Query *c, char *v) { /* takes ownership of v */
    arrput(c->params, v);
    append_string(&c->sql, "?");
}

/* an exact-match value may be written quoted (tag:"to do", notmuch's
 * habit for names with spaces): the quotes are not part of the value */
static char *strip_quotes(char *v) {
    size_t k = strlen(v);

    if (k >= 2 && v[0] == '"' && v[k - 1] == '"') {
        memmove(v, v + 1, k - 2);
        v[k - 2] = '\0';
    }
    return v;
}

static void compile_term(Query *c, const Node *n) {
    const char *p = n->prefix, *v = n->val;
    char buf[64];
    long a, b;

    if (p && (!strcmp(p, "tag") || !strcmp(p, "id") || !strcmp(p, "thread") || !strcmp(p, "path") || !strcmp(p, "folder")))
        v = strip_quotes(n->val);

    if (!p && !strcmp(v, "*")) {
        append_string(&c->sql, "1");
    } else if (!p || !strcmp(p, "subject") || !strcmp(p, "from") || !strcmp(p, "to") || !strcmp(p, "attachment") || !strcmp(p, "body")) {
        const char *col = !p ? NULL : !strcmp(p, "from") ? "sender" : !strcmp(p, "to") ? "rcpt" : !strcmp(p, "attachment") ? "attach" : p;
        char *m = fts_match_expression(col, v);
        if (!strcmp(m, "\"\"") || strstr(m, ": \"\"")) {
            set_error(&c->err, "empty search term", "");
            arrfree(m);
            return;
        }
        append_string(&c->sql, "id IN (SELECT rowid FROM fts WHERE fts MATCH ");
        bind_parameter(c, strdup(m));
        append_string(&c->sql, ")");
        arrfree(m);
    } else if (!strcmp(p, "tag")) {
        append_string(&c->sql, "id IN (SELECT msg FROM tag WHERE name=");
        bind_parameter(c, strdup(v));
        append_string(&c->sql, ")");
    } else if (!strcmp(p, "id")) {
        size_t k = strlen(v);
        char *m = strdup(v);
        if (k >= 2 && v[0] == '<' && v[k - 1] == '>') {
            memmove(m, m + 1, k - 2);
            m[k - 2] = '\0';
        }
        append_string(&c->sql, "mid=");
        bind_parameter(c, m);
    } else if (!strcmp(p, "thread")) {
        snprintf(buf, sizeof buf, "thread=%llu", (unsigned long long)strtoull(v, NULL, 16));
        append_string(&c->sql, buf);
    } else if (!strcmp(p, "path") || !strcmp(p, "folder")) {
        size_t k = strlen(v);
        if (!strcmp(v, "**") || !strcmp(v, "*")) {
            append_string(&c->sql, "1");
        } else if (k > 2 && !strcmp(v + k - 2, "/*")) {
            /* one level down, not the whole subtree: the boxes
             * directly inside it, none of their own children */
            append_string(&c->sql, "id IN (SELECT msg FROM file WHERE box GLOB ");
            bind_parameter(c, strdup(v));
            snprintf(buf, sizeof buf, " AND instr(substr(box,%zu),'/')=0)", k);
            append_string(&c->sql, buf);
        } else if (k > 3 && !strcmp(v + k - 3, "/**")) {
            char *box = strdup(v), *glob = malloc(k);
            box[k - 3] = '\0';
            snprintf(glob, k, "%s/*", box);
            append_string(&c->sql, "id IN (SELECT msg FROM file WHERE box=");
            bind_parameter(c, box);
            append_string(&c->sql, " OR box GLOB ");
            bind_parameter(c, glob);
            append_string(&c->sql, ")");
        } else {
            append_string(&c->sql, "id IN (SELECT msg FROM file WHERE box=");
            bind_parameter(c, strdup(v));
            append_string(&c->sql, ")");
        }
    } else if (!strcmp(p, "date")) {
        if (parse_date_range(v, &a, &b) < 0) {
            set_error(&c->err, "bad date '%s'", v);
            return;
        }
        snprintf(buf, sizeof buf, "date BETWEEN %ld AND %ld", a, b);
        append_string(&c->sql, buf);
    } else
        set_error(&c->err, "unknown prefix '%s:'", p);
}

static void compile_node(Query *c, const Node *n) {
    switch (n->op) {
    case NTerm:
        compile_term(c, n);
        break;
    case NNot:
        append_string(&c->sql, "NOT (");
        compile_node(c, n->l);
        append_string(&c->sql, ")");
        break;
    default:
        append_string(&c->sql, "(");
        compile_node(c, n->l);
        append_string(&c->sql, n->op == NAnd ? " AND " : " OR ");
        compile_node(c, n->r);
        append_string(&c->sql, ")");
    }
}

/* query text -> WHERE expression over msg; -1 with a message on error */
int query_compile(const char *q, Query *c, char **err) {
    Node *n = parse_query(q, err);

    memset(c, 0, sizeof *c);
    if (!n || *err) {
        if (!*err)
            *err = strdup("empty query");
        free_node(n);
        return -1;
    }
    compile_node(c, n);
    free_node(n);
    arrput(c->sql, '\0');
    if (c->err) {
        *err = c->err;
        c->err = NULL;
        return -1;
    }
    return 0;
}

void query_free(Query *c) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(c->params); i++)
        free(c->params[i]);
    arrfree(c->params);
    arrfree(c->sql);
}

/* prepare sql with the query's WHERE spliced in at "%s", params bound */
sqlite3_stmt *query_prepare(sqlite3 *db, const char *fmt, const Query *c, const char *tail, char **err) {
    char *sql = NULL;
    sqlite3_stmt *st;
    const char *pct = strstr(fmt, "%s");
    ptrdiff_t i;

    memcpy(arraddnptr(sql, pct - fmt), fmt, (size_t)(pct - fmt));
    append_string(&sql, c->sql);
    append_string(&sql, pct + 2);
    append_string(&sql, tail);
    arrput(sql, '\0');
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        set_error(err, "%s", sqlite3_errmsg(db));
        arrfree(sql);
        return NULL;
    }
    arrfree(sql);
    for (i = 0; i < arrlen(c->params); i++)
        sqlite3_bind_text(st, (int)i + 1, c->params[i], -1, SQLITE_STATIC);
    return st;
}

/* --- output helpers ---------------------------------------------------- */

static void print_json_string(const char *s) {
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
void display_name(const char *from, char *out, size_t cap) {
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
void relative_date(long t, char *out, size_t cap) {
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
    int batch; /* count: one query per stdin line */
} Opts;

static int compare_newest(const void *a, const void *b) {
    const Thread *x = a, *y = b;
    return x->newest < y->newest ? 1 : x->newest > y->newest ? -1 : 0;
}

static int compare_oldest(const void *a, const void *b) {
    const Thread *x = a, *y = b;
    return x->oldest > y->oldest ? 1 : x->oldest < y->oldest ? -1 : 0;
}

static void add_author(Thread *t, const char *from) {
    char name[128];
    size_t n = strlen(t->authors ? t->authors : "");

    display_name(from, name, sizeof name);
    if (n) { /* already listed? */
        const char *p = t->authors;
        size_t k = strlen(name);
        while ((p = strstr(p, name))) {
            if ((p == t->authors || !strncmp(p - 2, ", ", 2)) && (p[k] == '\0' || !strncmp(p + k, ", ", 2)))
                return;
            p++;
        }
        arrsetlen(t->authors, arrlen(t->authors) - 1);
        append_string(&t->authors, ", ");
    }
    append_string(&t->authors, name);
    arrput(t->authors, '\0');
}

static void thread_tags(sqlite3 *db, const Thread *t, char ***tags) {
    sqlite3_stmt *st;
    ptrdiff_t i, k;

    sqlite3_prepare_v2(db, "SELECT name FROM tag WHERE msg=? ORDER BY name", -1, &st, NULL);
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

static int compare_strings(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int print_summary(sqlite3 *db, const Query *c, const Opts *o, char **err) {
    sqlite3_stmt *st, *total;
    Thread *ts = NULL, *t;
    ptrdiff_t i, k, n;
    int json = !strcmp(o->format, "json"), threadsonly, rc;
    char date[32];

    if (!(st = query_prepare(db, "SELECT id,thread,date,sender,subject FROM msg WHERE %s", c, " ORDER BY thread,date", err)))
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
        add_author(t, from);
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
        set_error(err, "%s", sqlite3_errmsg(db));
        return -1;
    }
    /* never qsort an empty stb array: its NULL base is a nonnull argument
     * and GCC then deletes arrlen's own NULL check downstream (-O2) */
    if (arrlen(ts) > 1)
        qsort(ts, (size_t)arrlen(ts), sizeof *ts, o->oldest ? compare_oldest : compare_newest);
    n = arrlen(ts);
    if (o->offset < n)
        n -= o->offset;
    else
        n = 0;
    if (o->limit >= 0 && o->limit < n)
        n = o->limit;
    threadsonly = !strcmp(o->output, "threads");
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM msg WHERE thread=?", -1, &total, NULL);
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
        tot = sqlite3_step(total) == SQLITE_ROW ? sqlite3_column_int(total, 0) : t->matched;
        sqlite3_reset(total);
        thread_tags(db, t, &tags);
        if (arrlen(tags) > 1)
            qsort(tags, (size_t)arrlen(tags), sizeof *tags, compare_strings);
        relative_date(o->oldest ? t->oldest : t->newest, date, sizeof date);
        if (json) {
            printf(i ? ",\n{" : "\n{");
            printf("\"thread\": \"%016llx\", \"timestamp\": %ld, "
                   "\"date_relative\": ",
                   (unsigned long long)t->thread, o->oldest ? t->oldest : t->newest);
            print_json_string(date);
            printf(", \"matched\": %d, \"total\": %d, \"authors\": ", t->matched, tot);
            print_json_string(t->authors);
            printf(", \"subject\": ");
            print_json_string(t->subject);
            printf(", \"tags\": [");
            for (k = 0; k < arrlen(tags); k++) {
                printf(k ? ", " : "");
                print_json_string(tags[k]);
            }
            printf("]}");
        } else {
            printf("thread:%016llx %12s [%d/%d] %s; %s (", (unsigned long long)t->thread, date, t->matched, tot, t->authors, t->subject);
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

/* the absolute path of a maildir file the index knows as (box, sub, name);
 * 0 when the box belongs to an account that is no longer configured */
int file_path(const char *box, const char *sub, const char *name, char *out, size_t cap) {
    char dir[4096];

    if (!box_directory(box, dir, sizeof dir))
        return 0;
    snprintf(out, cap, "%s/%s/%s", dir, sub, name);
    return 1;
}

/* messages / files / tags: one string per line, or a JSON array */
static int print_listing(sqlite3 *db, const Query *c, const Opts *o, char **err) {
    sqlite3_stmt *st;
    char tail[128], line[8192];
    const char *sql;
    int json = !strcmp(o->format, "json"), files = 0, rc;
    long i = 0;

    snprintf(tail, sizeof tail, " LIMIT %ld OFFSET %ld", o->limit < 0 ? -1L : o->limit, o->offset);
    if (!strcmp(o->output, "messages")) {
        sql = o->oldest ? "SELECT mid FROM msg WHERE %s ORDER BY date" : "SELECT mid FROM msg WHERE %s ORDER BY date DESC";
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
        set_error(err, "unknown output '%s'", o->output);
        return -1;
    }
    if (!(st = query_prepare(db, sql, c, tail, err)))
        return -1;
    if (json)
        putchar('[');
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (files) {
            if (!file_path(s, (const char *)sqlite3_column_text(st, 1), (const char *)sqlite3_column_text(st, 2), line, sizeof line))
                continue; /* box of an account no longer configured */
            s = line;
        } else if (!strcmp(o->output, "messages") && !json) {
            snprintf(line, sizeof line, "id:%s", s);
            s = line;
        }
        if (json) {
            printf(i ? ",\n" : "\n");
            print_json_string(s);
        } else
            puts(s);
        i++;
    }
    sqlite3_finalize(st);
    if (json)
        puts(i ? "\n]" : "]");
    if (rc != SQLITE_DONE) {
        set_error(err, "%s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* --output=X / --limit=N style options; the rest joined is the query */
static char *parse_options(int argc, char **argv, Opts *o, const char *cmd) {
    char *q = NULL;
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { /* end of options: the rest is the query */
            for (i++; i < argc; i++) {
                if (q)
                    append_string(&q, " ");
                append_string(&q, argv[i]);
            }
            break;
        }
        if (!strcmp(a, "--batch"))
            o->batch = 1;
        else if (!strncmp(a, "--output=", 9))
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
                append_string(&q, " ");
            append_string(&q, a);
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

int search_main(int argc, char **argv) {
    Opts o = {"summary", "text", -1, 0, 0, 0};
    Query c;
    sqlite3 *db;
    char *q, *err = NULL, dberr[256];
    int rc;

    if (!(q = parse_options(argc, argv, &o, "search")))
        return 2;
    if (!*q) {
        fputs("usage: hml search [--output=summary|threads|messages|files|"
              "tags]\n           [--format=text|json] [--limit=N] [--offset=N]"
              "\n           [--sort=newest-first|oldest-first] <query>\n",
              stderr);
        arrfree(q);
        return 2;
    }
    if (query_compile(q, &c, &err) < 0) {
        arrfree(q);
        return fail("search", err);
    }
    arrfree(q);
    if (!(db = db_open(dberr, sizeof dberr))) {
        query_free(&c);
        return fail("search", strdup(dberr));
    }
    if (!strcmp(o.output, "summary") || !strcmp(o.output, "threads"))
        rc = print_summary(db, &c, &o, &err);
    else
        rc = print_listing(db, &c, &o, &err);
    query_free(&c);
    sqlite3_close(db);
    return rc < 0 ? fail("search", err) : 0;
}

/* one count for query q on stdout; -1 with err set on failure */
static int print_count(sqlite3 *db, const char *sql, const char *q, char **err) {
    Query c;
    sqlite3_stmt *st;

    if (query_compile(q, &c, err) < 0)
        return -1;
    if (!(st = query_prepare(db, sql, &c, "", err)) || sqlite3_step(st) != SQLITE_ROW) {
        set_error(err, "%s", sqlite3_errmsg(db));
        query_free(&c);
        return -1;
    }
    printf("%lld\n", (long long)sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    query_free(&c);
    return 0;
}

/* hml count [--batch] [--output=...] <query>; with --batch every stdin
 * line is a query and gets its own count line (notmuch's --batch) */
int count_main(int argc, char **argv) {
    Opts o = {"messages", "text", -1, 0, 0, 0};
    sqlite3 *db;
    const char *sql;
    char *q, *err = NULL, dberr[256], line[8192];

    if (!(q = parse_options(argc, argv, &o, "count")))
        return 2;
    if (!*q) {
        arrfree(q);
        q = NULL;
        append_string(&q, "*");
        arrput(q, '\0');
    }
    if (!strcmp(o.output, "threads"))
        sql = "SELECT COUNT(DISTINCT thread) FROM msg WHERE %s";
    else if (!strcmp(o.output, "files"))
        sql = "SELECT COUNT(*) FROM file WHERE msg IN (SELECT id FROM msg"
              " WHERE %s)";
    else
        sql = "SELECT COUNT(*) FROM msg WHERE %s";
    if (!(db = db_open(dberr, sizeof dberr))) {
        arrfree(q);
        return fail("count", strdup(dberr));
    }
    if (o.batch) {
        while (fgets(line, sizeof line, stdin)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            if (print_count(db, sql, n ? line : "*", &err) < 0) {
                arrfree(q);
                sqlite3_close(db);
                return fail("count", err);
            }
        }
    } else if (print_count(db, sql, q, &err) < 0) {
        arrfree(q);
        sqlite3_close(db);
        return fail("count", err);
    }
    arrfree(q);
    sqlite3_close(db);
    return 0;
}

/* every tag in the index, or those of the messages matching a query */
int tags_main(int argc, char **argv) {
    Opts o = {"tags", "text", -1, 0, 0, 0};
    Query c;
    sqlite3 *db;
    char *q, *err = NULL, dberr[256];
    int rc;

    if (!(q = parse_options(argc, argv, &o, "tags")))
        return 2;
    o.output = "tags";
    if (!*q) {
        arrfree(q);
        q = NULL;
        append_string(&q, "*");
        arrput(q, '\0');
    }
    if (query_compile(q, &c, &err) < 0) {
        arrfree(q);
        return fail("tags", err);
    }
    arrfree(q);
    if (!(db = db_open(dberr, sizeof dberr))) {
        query_free(&c);
        return fail("tags", strdup(dberr));
    }
    rc = print_listing(db, &c, &o, &err);
    query_free(&c);
    sqlite3_close(db);
    return rc < 0 ? fail("tags", err) : 0;
}

/* --- hml address -------------------------------------------------------- */

typedef struct {
    char *name; /* newest non-empty display name, malloc'd */
    long sent;  /* messages we sent to it */
    long seen;  /* messages it sent us, or was a co-recipient on */
    long last;  /* newest date */
    long named; /* date of the message the name came from */
} Addr;

typedef struct {
    char *key; /* bare address, lowercased */
    Addr value;
} AddrKV;

/* runs of letters/digits (any non-ASCII byte counts), lowercased — the
 * words FTS5's unicode61 tokenizer would make of the typed text */
static char **split_words(char *s) {
    char **w = NULL, *p;

    for (p = s; *p;) {
        char *b;
        while (*p && !isalnum((unsigned char)*p) && !((unsigned char)*p & 0x80))
            p++;
        if (!*p)
            break;
        for (b = p; *p && (isalnum((unsigned char)*p) || (unsigned char)*p & 0x80); p++)
            *p = (char)tolower((unsigned char)*p);
        if (*p)
            *p++ = '\0';
        arrput(w, b);
    }
    return w;
}

/* the display name of a mailbox, quotes stripped; "" when there is none */
static void mailbox_name(const char *m, char *out, size_t cap) {
    const char *lt = strchr(m, '<'), *e = lt ? lt : m;
    size_t n = 0;

    out[0] = '\0';
    if (!lt)
        return;
    while (m < e && (isspace((unsigned char)*m) || *m == '"' || *m == '\''))
        m++;
    while (e > m && (isspace((unsigned char)e[-1]) || e[-1] == '"' || e[-1] == '\''))
        e--;
    for (; m < e && n + 1 < cap; m++)
        if (*m != '\\')
            out[n++] = *m;
    out[n] = '\0';
}

/* addresses no person reads: bounce handlers, no-reply senders and
 * notification relays (whose display name is whoever triggered one) */
static int is_machine_address(const char *bare) {
    static const char *const bad[] = {"bounce", "noreply", "no-reply", "no_reply", "donotreply", "do-not-reply", "mailer-daemon", "notification"};
    const char *at = strchr(bare, '@');
    char local[256];
    size_t k;

    snprintf(local, sizeof local, "%.*s", (int)(at - bare), bare);
    for (k = 0; k < sizeof bad / sizeof *bad; k++)
        if (strstr(local, bad[k]))
            return 1;
    return 0;
}

static void tally_addresses(AddrKV **h, char **ws, const char *list, int sent, long date) {
    char **l = NULL, bare[256], name[256], hay[520];
    ptrdiff_t i, j;
    int k;

    mime_split_addresses(list, &l);
    for (i = 0; i < arrlen(l); i++) {
        Addr *a;
        mime_bare_address(l[i], bare, sizeof bare);
        mailbox_name(l[i], name, sizeof name);
        free(l[i]);
        if (!strchr(bare, '@') || strpbrk(bare, " \t,;<>\"") || is_machine_address(bare))
            continue;
        for (k = 0; k < naccounts && strcasecmp(bare, accounts[k].user); k++)
            ;
        if (k < naccounts) /* ourselves */
            continue;
        snprintf(hay, sizeof hay, "%s %s", name, bare);
        for (j = 0; hay[j]; j++)
            hay[j] = (char)tolower((unsigned char)hay[j]);
        for (j = 0; j < arrlen(ws) && strstr(hay, ws[j]); j++)
            ;
        if (j < arrlen(ws))
            continue;
        if (shgeti(*h, bare) < 0) {
            Addr z = {NULL, 0, 0, 0, 0};
            shput(*h, bare, z);
        }
        a = &(*h)[shgeti(*h, bare)].value;
        *(sent ? &a->sent : &a->seen) += 1;
        if (date > a->last)
            a->last = date;
        if (*name && (!a->name || date >= a->named)) {
            free(a->name);
            a->name = strdup(name);
            a->named = date;
        }
    }
    arrfree(l);
}

/* people we wrote to first (most often, then most recently), then the
 * ones who only wrote to us */
static int compare_addresses(const void *x, const void *y) {
    const Addr *a = &((const AddrKV *)x)->value, *b = &((const AddrKV *)y)->value;

    if ((a->sent > 0) != (b->sent > 0))
        return a->sent > 0 ? -1 : 1;
    if (a->sent + a->seen != b->sent + b->seen)
        return a->sent + a->seen > b->sent + b->seen ? -1 : 1;
    return a->last > b->last ? -1 : a->last < b->last;
}

/* hml address [--limit=N] [--] [words]: the addresses we correspond with
 * whose name or address contains every word, as ready-to-paste
 * mailboxes; no words = everyone we ever sent to */
int address_main(int argc, char **argv) {
    AddrKV *h = NULL;
    sqlite3 *db;
    sqlite3_stmt *st;
    char *text = NULL, *match = NULL, **ws, dberr[256];
    long limit = 20;
    ptrdiff_t i;
    int k;

    for (k = 0; k < argc && argv[k][0] == '-'; k++) {
        if (!strcmp(argv[k], "--")) {
            k++;
            break;
        } else if (!strncmp(argv[k], "--limit=", 8))
            limit = atol(argv[k] + 8);
        else {
            fputs("usage: hml address [--limit=N] [--] [words]\n", stderr);
            return 2;
        }
    }
    for (; k < argc; k++) {
        append_string(&text, argv[k]);
        arrput(text, ' ');
    }
    arrput(text, '\0');
    ws = split_words(text);
    if (arrlen(ws)) {
        append_string(&match, "{sender rcpt} : (");
        for (i = 0; i < arrlen(ws); i++) {
            append_string(&match, i ? " \"" : "\"");
            append_string(&match, ws[i]);
            append_string(&match, "\"*");
        }
        append_string(&match, ")");
    }
    arrput(match, '\0');
    if (!(db = db_open(dberr, sizeof dberr)))
        return fail("address", strdup(dberr));
    if (sqlite3_prepare_v2(db,
                           arrlen(ws) ? "SELECT sender,rcpt,date,id IN (SELECT msg FROM tag"
                                        " WHERE name='sent') FROM msg WHERE id IN (SELECT"
                                        " rowid FROM fts WHERE fts MATCH ?)"
                                      : "SELECT sender,rcpt,date,1 FROM msg WHERE id IN"
                                        " (SELECT msg FROM tag WHERE name='sent')",
                           -1, &st, NULL) != SQLITE_OK) {
        fail("address", strdup(sqlite3_errmsg(db)));
        sqlite3_close(db);
        return 2;
    }
    if (arrlen(ws))
        sqlite3_bind_text(st, 1, match, -1, SQLITE_STATIC);
    sh_new_strdup(h);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int sent = sqlite3_column_int(st, 3);
        long date = (long)sqlite3_column_int64(st, 2);
        /* what we sent counts its recipients; what we got counts its
         * sender and the others it went to */
        if (!sent)
            tally_addresses(&h, ws, (const char *)sqlite3_column_text(st, 0), 0, date);
        tally_addresses(&h, ws, (const char *)sqlite3_column_text(st, 1), sent, date);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    qsort(h, (size_t)shlen(h), sizeof *h, compare_addresses);
    for (i = 0; i < shlen(h); i++) {
        const char *n = h[i].value.name;
        if (i < limit || limit < 0) {
            if (!n || !*n)
                printf("%s\n", h[i].key);
            else if (strpbrk(n, ",;:<>@()[]\""))
                printf("\"%s\" <%s>\n", n, h[i].key);
            else
                printf("%s <%s>\n", n, h[i].key);
        }
        free(h[i].value.name);
    }
    shfree(h);
    arrfree(ws);
    arrfree(text);
    arrfree(match);
    return 0;
}

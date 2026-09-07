/* index.c - "hml new": the search index, SQLite + FTS5 at
 * <mailroot>/.hml.db. One row per message (Message-ID), one per maildir
 * file, tags derived from flags and folders, threads from References. The
 * index is an hml-only cache: deleting it costs a rebuild, nothing else.
 *
 * "hml tag": user tags are overrides on the derived set, keyed by
 * Message-ID so they survive a message leaving and re-entering the store.
 * Their source of truth is the append-only log <mailroot>/.htags (one
 * "mid TAB +tag TAB -tag" line per operation); the utag table mirrors it
 * and the meta table remembers how much of the log has been applied, so
 * a rebuilt index replays the rest. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <stb_ds.h>

#include "hml.h"

static const char *schema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS dir(path TEXT PRIMARY KEY, mtime INTEGER);"
    "CREATE TABLE IF NOT EXISTS msg(id INTEGER PRIMARY KEY,"
    " mid TEXT NOT NULL UNIQUE, thread INTEGER NOT NULL, date INTEGER NOT NULL,"
    " subject TEXT NOT NULL, sender TEXT NOT NULL,"
    " attach INTEGER NOT NULL DEFAULT 0, intent TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX IF NOT EXISTS msg_thread ON msg(thread);"
    "CREATE INDEX IF NOT EXISTS msg_date ON msg(date);"
    "CREATE TABLE IF NOT EXISTS file(box TEXT NOT NULL, base TEXT NOT NULL,"
    " sub TEXT NOT NULL, name TEXT NOT NULL, flags TEXT NOT NULL,"
    " msg INTEGER NOT NULL, PRIMARY KEY(box, base));"
    "CREATE INDEX IF NOT EXISTS file_msg ON file(msg);"
    "CREATE TABLE IF NOT EXISTS ref(msg INTEGER NOT NULL, mid TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS ref_mid ON ref(mid);"
    "CREATE INDEX IF NOT EXISTS ref_msg ON ref(msg);"
    "CREATE TABLE IF NOT EXISTS tag(msg INTEGER NOT NULL, name TEXT NOT NULL,"
    " PRIMARY KEY(msg, name)) WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS tag_name ON tag(name, msg);"
    "CREATE TABLE IF NOT EXISTS utag(mid TEXT NOT NULL, name TEXT NOT NULL,"
    " val INTEGER NOT NULL, PRIMARY KEY(mid, name)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, val);"
    "CREATE TEMP TABLE IF NOT EXISTS newmsg(id INTEGER PRIMARY KEY);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(subject, sender, rcpt,"
    " attach, body, content='', contentless_delete=1,"
    " tokenize='unicode61 remove_diacritics 2');";

/* one code point as UTF-8, appended */
static void putcp(char *out, size_t cap, unsigned cp) {
    size_t n = strlen(out);
    char u[5] = {0};

    if (cp < 0x80)
        u[0] = (char)cp;
    else if (cp < 0x800) {
        u[0] = (char)(0xC0 | cp >> 6);
        u[1] = (char)(0x80 | (cp & 0x3F));
    } else {
        u[0] = (char)(0xE0 | cp >> 12);
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
    }
    snprintf(out + n, cap - n, "%s", u);
}

/* msg.attach arrived after the first indexes were built. Add it and
 * backfill without re-parsing a single file: the FTS index already holds
 * the attachment-name tokens, so "every message with an attach token" is
 * one prefix query over every initial letter/digit the tokenizer can
 * produce (Latin, digits, Cyrillic, Greek — diacritics are folded). */
static int readfile(const char *path, char **buf, size_t *len);

/* the refinement: the FTS backfill marks every message with a *named*
 * part, which includes the Content-ID logos of every newsletter; only a
 * parse tells a real attachment apart, so the candidates are parsed on all
 * cores (a few seconds for tens of thousands) and the rest un-marked */
typedef struct {
    sqlite3_int64 id;
    char *path;
    int keep;
    int marked; /* msg.attach as it stands */
} Cand;

static struct {
    Cand *c;
    long next;
    pthread_mutex_t mtx;
} refine_q = {NULL, 0, PTHREAD_MUTEX_INITIALIZER};

static void *refiner(void *arg) {
    (void)arg;
    for (;;) {
        char *buf;
        size_t len;
        long i;
        Mail m;
        pthread_mutex_lock(&refine_q.mtx);
        i = refine_q.next++;
        pthread_mutex_unlock(&refine_q.mtx);
        if (i >= arrlen(refine_q.c))
            return NULL;
        if (readfile(refine_q.c[i].path, &buf, &len) < 0) {
            refine_q.c[i].keep = refine_q.c[i].marked; /* gone: no opinion */
            continue;
        }
        mailparse(buf, len, &m);
        refine_q.c[i].keep = m.hasatt;
        mailfree(&m);
        free(buf);
    }
}

static void refine(sqlite3 *db) {
    sqlite3_stmt *st, *unmark, *untag;
    pthread_t tid[32];
    char path[4608];
    long n, i, dropped = 0;

    /* candidates: everything marked, plus every message with more than one
     * file — the FTS row came from the first copy indexed, and a duplicate
     * delivery may be the one carrying the attachment */
    sqlite3_prepare_v2(db,
                       "SELECT msg.id,file.box,file.sub,file.name,msg.attach"
                       " FROM msg JOIN file ON file.msg=msg.id WHERE"
                       " msg.attach=1 OR msg.id IN (SELECT msg FROM file"
                       " GROUP BY msg HAVING COUNT(*)>1)",
                       -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Cand c = {sqlite3_column_int64(st, 0), NULL, 0,
                  sqlite3_column_int(st, 4)};
        if (!filepath((const char *)sqlite3_column_text(st, 1),
                      (const char *)sqlite3_column_text(st, 2),
                      (const char *)sqlite3_column_text(st, 3), path,
                      sizeof path))
            continue;
        c.path = strdup(path);
        arrput(refine_q.c, c);
    }
    sqlite3_finalize(st);
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    if (n > 32)
        n = 32;
    if (n > arrlen(refine_q.c))
        n = arrlen(refine_q.c);
    for (i = 0; i < n; i++)
        pthread_create(&tid[i], NULL, refiner, NULL);
    for (i = 0; i < n; i++)
        pthread_join(tid[i], NULL);
    /* a message is an attachment carrier if ANY of its files is; set the
     * mark (and the tag row) to match, whichever way it currently stands */
    {
        struct {
            sqlite3_int64 key;
            int value;
        } *keep = NULL, *seen = NULL;
        sqlite3_stmt *mark, *tag;
        long total = 0, added = 0;
        for (i = 0; i < arrlen(refine_q.c); i++)
            if (refine_q.c[i].keep)
                hmput(keep, refine_q.c[i].id, 1);
        sqlite3_prepare_v2(db, "UPDATE msg SET attach=0 WHERE id=?", -1,
                           &unmark, NULL);
        sqlite3_prepare_v2(db,
                           "DELETE FROM tag WHERE msg=? AND name='attachment'",
                           -1, &untag, NULL);
        sqlite3_prepare_v2(db, "UPDATE msg SET attach=1 WHERE id=?", -1, &mark,
                           NULL);
        sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO tag(msg,name) VALUES"
                           "(?,'attachment')",
                           -1, &tag, NULL);
        for (i = 0; i < arrlen(refine_q.c); i++) {
            sqlite3_int64 id = refine_q.c[i].id;
            if (hmgeti(seen, id) < 0) {
                int want = hmgeti(keep, id) >= 0;
                hmput(seen, id, 1);
                total++;
                if (want && !refine_q.c[i].marked) {
                    sqlite3_bind_int64(mark, 1, id);
                    sqlite3_step(mark);
                    sqlite3_reset(mark);
                    sqlite3_bind_int64(tag, 1, id);
                    sqlite3_step(tag);
                    sqlite3_reset(tag);
                    added++;
                } else if (!want && refine_q.c[i].marked) {
                    sqlite3_bind_int64(unmark, 1, id);
                    sqlite3_step(unmark);
                    sqlite3_reset(unmark);
                    sqlite3_bind_int64(untag, 1, id);
                    sqlite3_step(untag);
                    sqlite3_reset(untag);
                    dropped++;
                }
            }
            free(refine_q.c[i].path);
        }
        sqlite3_finalize(unmark);
        sqlite3_finalize(untag);
        sqlite3_finalize(mark);
        sqlite3_finalize(tag);
        fprintf(stderr,
                "hml: attachment tag: %ld candidates parsed, %ld"
                " marked, %ld un-marked\n",
                total, added, dropped);
        hmfree(keep);
        hmfree(seen);
    }
    arrfree(refine_q.c);
}

/* msg.intent (hai's session messages) arrived later too; no backfill:
 * the boxes that carry it did not exist before the column */
static void ensureintent(sqlite3 *db) {
    sqlite3_stmt *st;
    int have = 0;

    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM pragma_table_info('msg')"
                           " WHERE name='intent'",
                           -1, &st, NULL) == SQLITE_OK) {
        have = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    if (!have)
        sqlite3_exec(db,
                     "ALTER TABLE msg ADD COLUMN intent TEXT NOT NULL"
                     " DEFAULT ''",
                     NULL, NULL, NULL);
}

static void migrate(sqlite3 *db) {
    sqlite3_stmt *st;
    char q[4096] = "UPDATE msg SET attach=1 WHERE id IN (SELECT rowid FROM fts"
                   " WHERE fts MATCH 'attach : (";
    unsigned cp;
    int have = 0, strict = 0, first = 1;

    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM pragma_table_info('msg')"
                           " WHERE name='attach'",
                           -1, &st, NULL) == SQLITE_OK) {
        have = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    if (have &&
        sqlite3_prepare_v2(db, "SELECT 1 FROM meta WHERE key='attachstrict'",
                           -1, &st, NULL) == SQLITE_OK) {
        strict = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    if (have && strict)
        return;
    for (cp = 0; cp < 0x460; cp++) {
        if (!((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
              (cp >= 0x430 && cp <= 0x45F) || (cp >= 0x3B1 && cp <= 0x3C9)))
            continue;
        if (strlen(q) + 16 >= sizeof q)
            break;
        strcat(q, first ? "" : " OR ");
        putcp(q, sizeof q, cp);
        strcat(q, "*");
        first = 0;
    }
    strcat(q, ")')");
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if ((have || sqlite3_exec(db,
                              "ALTER TABLE msg ADD COLUMN attach INTEGER NOT"
                              " NULL DEFAULT 0",
                              NULL, NULL, NULL) == SQLITE_OK) &&
        sqlite3_exec(db, q, NULL, NULL, NULL) == SQLITE_OK)
        sqlite3_exec(db,
                     "INSERT OR IGNORE INTO tag(msg,name) SELECT id,"
                     "'attachment' FROM msg WHERE attach=1",
                     NULL, NULL, NULL);
    else
        fputs("hml: attachment backfill failed — rebuild the index "
              "(delete .hml.db*, run hml new)\n",
              stderr);
    refine(db);
    sqlite3_exec(db,
                 "INSERT OR REPLACE INTO meta(key,val) VALUES"
                 "('attachstrict',1); COMMIT",
                 NULL, NULL, NULL);
}

sqlite3 *dbopen(char *err, size_t errlen) {
    char path[4096], *e = NULL;
    sqlite3 *db;
    size_t n;

    expand(mailroot, path, sizeof path);
    n = strlen(path);
    snprintf(path + n, sizeof path - n, "/.hml.db");
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 30000);
    if (sqlite3_exec(db, schema, NULL, NULL, &e) != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", path, e);
        sqlite3_free(e);
        sqlite3_close(db);
        return NULL;
    }
    migrate(db);
    ensureintent(db);
    return db;
}

/* --- statements -------------------------------------------------------- */

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *msgbymid, *insmsg, *setthread, *merge, *insfts, *delfts,
        *insref, *delref, *insfile, *mvfile, *delfile, *filemsg, *nfiles,
        *delmsg, *deltag, *instag, *msgfiles, *boxfiles, *dirget, *dirset,
        *midof, *utagget, *utagset, *utagdel, *filesof, *attachof, *setattach,
        *intentof, *metaget, *metaset, *insnew;
    long added, moved, removed, newmsgs;
} Db;

static const char *cmdname = "new";

static void die(Db *d, const char *what) {
    fprintf(stderr, "hml %s: %s: %s\n", cmdname, what, sqlite3_errmsg(d->db));
    exit(2);
}

static sqlite3_stmt *prep(Db *d, const char *sql) {
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(d->db, sql, -1, &st, NULL) != SQLITE_OK)
        die(d, sql);
    return st;
}

static void prepall(Db *d) {
    d->msgbymid = prep(d, "SELECT id FROM msg WHERE mid=?");
    d->insmsg = prep(d, "INSERT INTO msg(mid,thread,date,subject,sender,"
                        "attach,intent) VALUES(?,?,?,?,?,?,?)");
    d->attachof = prep(d, "SELECT attach FROM msg WHERE id=?");
    d->intentof = prep(d, "SELECT intent FROM msg WHERE id=?");
    d->setattach = prep(d, "UPDATE msg SET attach=1 WHERE id=? AND attach=0");
    d->setthread = prep(d, "UPDATE msg SET thread=? WHERE id=?");
    d->merge = prep(d, "UPDATE msg SET thread=? WHERE thread=?");
    d->insfts = prep(d, "INSERT INTO fts(rowid,subject,sender,rcpt,attach,"
                        "body) VALUES(?,?,?,?,?,?)");
    d->delfts = prep(d, "DELETE FROM fts WHERE rowid=?");
    d->insref = prep(d, "INSERT INTO ref(msg,mid) VALUES(?,?)");
    d->delref = prep(d, "DELETE FROM ref WHERE msg=?");
    d->insfile = prep(d, "INSERT OR REPLACE INTO file(box,base,sub,name,flags,"
                         "msg) VALUES(?,?,?,?,?,?)");
    d->mvfile =
        prep(d, "UPDATE file SET sub=?,name=?,flags=? WHERE box=? AND base=?");
    d->delfile = prep(d, "DELETE FROM file WHERE box=? AND base=?");
    d->filemsg = prep(d, "SELECT msg FROM file WHERE box=? AND base=?");
    d->nfiles = prep(d, "SELECT COUNT(*) FROM file WHERE msg=?");
    d->delmsg = prep(d, "DELETE FROM msg WHERE id=?");
    d->deltag = prep(d, "DELETE FROM tag WHERE msg=?");
    d->instag = prep(d, "INSERT OR IGNORE INTO tag(msg,name) VALUES(?,?)");
    d->msgfiles = prep(d, "SELECT box,flags FROM file WHERE msg=?");
    d->boxfiles =
        prep(d, "SELECT base,sub,name,flags,msg FROM file WHERE box=?");
    d->dirget = prep(d, "SELECT mtime FROM dir WHERE path=?");
    d->dirset = prep(d, "INSERT OR REPLACE INTO dir(path,mtime) VALUES(?,?)");
    d->midof = prep(d, "SELECT mid FROM msg WHERE id=?");
    d->utagget = prep(d, "SELECT name,val FROM utag WHERE mid=?");
    d->utagset =
        prep(d, "INSERT OR REPLACE INTO utag(mid,name,val) VALUES(?,?,?)");
    d->utagdel = prep(d, "DELETE FROM utag WHERE mid=? AND name=?");
    d->filesof =
        prep(d, "SELECT box,base,sub,name,flags FROM file WHERE msg=?");
    d->metaget = prep(d, "SELECT val FROM meta WHERE key=?");
    d->metaset = prep(d, "INSERT OR REPLACE INTO meta(key,val) VALUES(?,?)");
    d->insnew = prep(d, "INSERT OR IGNORE INTO newmsg(id) VALUES(?)");
}

/* run a statement to completion; returns the first column of the first
 * row as an integer, or -1 when there is no row */
static sqlite3_int64 step1(Db *d, sqlite3_stmt *st) {
    sqlite3_int64 r = -1;
    int rc = sqlite3_step(st);

    if (rc == SQLITE_ROW)
        r = sqlite3_column_int64(st, 0);
    else if (rc != SQLITE_DONE)
        die(d, sqlite3_sql(st));
    sqlite3_reset(st);
    return r;
}

static void bindtext(sqlite3_stmt *st, int i, const char *s) {
    sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
}

static void sadd(char **s, const char *t) {
    memcpy(arraddnptr(*s, strlen(t)), t, strlen(t));
}

static void exec(Db *d, const char *sql) {
    if (sqlite3_exec(d->db, sql, NULL, NULL, NULL) != SQLITE_OK)
        die(d, sql);
}

/* --- tags -------------------------------------------------------------- */

/* recompute the tags of one message: derived from its files' flags and
 * folders, then the user's overrides applied on top */
static void retag(Db *d, sqlite3_int64 id) {
    char *tags[32], flags[64] = "", *mid = NULL;
    int nt = 0, folder = 0, i, k, rc;

    sqlite3_bind_int64(d->msgfiles, 1, id);
    while ((rc = sqlite3_step(d->msgfiles)) == SQLITE_ROW) {
        const char *box = (const char *)sqlite3_column_text(d->msgfiles, 0);
        const char *fl = (const char *)sqlite3_column_text(d->msgfiles, 1);
        const char *near = strchr(box, '/');
        near = near ? near + 1 : box;
        for (; *fl; fl++)
            if (!strchr(flags, *fl) && strlen(flags) < sizeof flags - 1)
                strncat(flags, fl, 1);
        for (i = 0; i < nfoldertags; i++) {
            if (strcmp(near, foldertags[i].folder))
                continue;
            folder = 1;
            for (k = 0; k < nt; k++)
                if (!strcmp(tags[k], foldertags[i].tag))
                    break;
            if (k == nt && nt < 8)
                tags[nt++] = strdup(foldertags[i].tag);
        }
    }
    if (rc != SQLITE_DONE)
        die(d, "msgfiles");
    sqlite3_reset(d->msgfiles);
    if (!folder)
        tags[nt++] = strdup("inbox");
    if (!strchr(flags, 'S'))
        tags[nt++] = strdup("unread");
    if (strchr(flags, 'F'))
        tags[nt++] = strdup("flagged");
    if (strchr(flags, 'R'))
        tags[nt++] = strdup("replied");
    if (strchr(flags, 'D'))
        tags[nt++] = strdup("draft");
    if (strchr(flags, 'P'))
        tags[nt++] = strdup("passed");
    sqlite3_bind_int64(d->attachof, 1, id);
    if (step1(d, d->attachof) > 0)
        tags[nt++] = strdup("attachment");
    sqlite3_reset(d->attachof);
    /* a hai session message: hai:<intent>, so the plumbing can be hidden */
    sqlite3_bind_int64(d->intentof, 1, id);
    if (sqlite3_step(d->intentof) == SQLITE_ROW) {
        const char *in = (const char *)sqlite3_column_text(d->intentof, 0);
        if (in && *in && nt < 30) {
            char t[64];
            snprintf(t, sizeof t, "hai:%.50s", in);
            tags[nt++] = strdup(t);
        }
    }
    sqlite3_reset(d->intentof);
    /* overrides: +tag adds, -tag removes, latest write per name wins */
    sqlite3_bind_int64(d->midof, 1, id);
    if (sqlite3_step(d->midof) == SQLITE_ROW)
        mid = strdup((const char *)sqlite3_column_text(d->midof, 0));
    sqlite3_reset(d->midof);
    if (mid) {
        bindtext(d->utagget, 1, mid);
        while ((rc = sqlite3_step(d->utagget)) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(d->utagget, 0);
            int on = sqlite3_column_int(d->utagget, 1);
            for (k = 0; k < nt; k++)
                if (!strcmp(tags[k], name))
                    break;
            if (on && k == nt && nt < (int)(sizeof tags / sizeof *tags))
                tags[nt++] = strdup(name);
            else if (!on && k < nt) {
                free(tags[k]);
                tags[k] = tags[--nt];
            }
        }
        if (rc != SQLITE_DONE)
            die(d, "utagget");
        sqlite3_reset(d->utagget);
        free(mid);
    }
    sqlite3_bind_int64(d->deltag, 1, id);
    step1(d, d->deltag);
    for (i = 0; i < nt; i++) {
        sqlite3_bind_int64(d->instag, 1, id);
        bindtext(d->instag, 2, tags[i]);
        step1(d, d->instag);
        free(tags[i]);
    }
}

/* --- user tags --------------------------------------------------------- */

typedef struct {
    char *name;
    int on; /* 1 = +name, 0 = -name */
} Op;

/* "+a -b ..." (space, tab or argv-separated) -> ops; -1 on a bad token */
static int parseops(const char *spec, Op **ops) {
    const char *p = spec, *e;
    Op o;

    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return 0;
        for (e = p; *e && *e != ' ' && *e != '\t'; e++)
            ;
        if ((*p != '+' && *p != '-') || e - p < 2)
            return -1;
        o.on = *p == '+';
        o.name = malloc((size_t)(e - p));
        memcpy(o.name, p + 1, (size_t)(e - p - 1));
        o.name[e - p - 1] = '\0';
        arrput(*ops, o);
        p = e;
    }
}

static void freeops(Op *ops) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(ops); i++)
        free(ops[i].name);
    arrfree(ops);
}

/* the tags that ARE maildir flags: unread (Seen, inverted), flagged,
 * replied, passed. They are never overrides — `hml tag` renames the
 * files instead, so the tag, the flag and (after `hml recv`) the server
 * all agree. Returns the flag bit, 0 for an ordinary tag. */
static unsigned flagof(const char *name, int *inverted) {
    *inverted = 0;
    if (!strcmp(name, "unread")) {
        *inverted = 1;
        return FSeen;
    }
    if (!strcmp(name, "flagged"))
        return FFlagged;
    if (!strcmp(name, "replied"))
        return FAnswered;
    if (!strcmp(name, "passed"))
        return FPassed;
    return 0;
}

/* apply the flag ops to every file of a message: rename in the maildir
 * (seen mail graduates new/ -> cur/), mirror the row, drop any stale
 * override of the same name, then recompute the tags */
static void mirrorflags(Db *d, sqlite3_int64 id, const char *mid,
                        const Op *ops) {
    typedef struct {
        char *box, *base, *sub, *name, *flags;
    } Row;
    Row *rows = NULL, r;
    ptrdiff_t i, k;
    int rc, inv;

    sqlite3_bind_int64(d->filesof, 1, id);
    while ((rc = sqlite3_step(d->filesof)) == SQLITE_ROW) {
        r.box = strdup((const char *)sqlite3_column_text(d->filesof, 0));
        r.base = strdup((const char *)sqlite3_column_text(d->filesof, 1));
        r.sub = strdup((const char *)sqlite3_column_text(d->filesof, 2));
        r.name = strdup((const char *)sqlite3_column_text(d->filesof, 3));
        r.flags = strdup((const char *)sqlite3_column_text(d->filesof, 4));
        arrput(rows, r);
    }
    if (rc != SQLITE_DONE)
        die(d, "filesof");
    sqlite3_reset(d->filesof);
    for (i = 0; i < arrlen(rows); i++) {
        unsigned f = letterflags(rows[i].flags), nf = f, bit;
        char boxdir[4160], name[512], fl[8], err[256];
        Local m;
        for (k = 0; k < arrlen(ops); k++) {
            if (!(bit = flagof(ops[k].name, &inv)))
                continue;
            if (ops[k].on != inv)
                nf |= bit;
            else
                nf &= ~bit;
        }
        if (nf == f || !boxroot(rows[i].box, boxdir, sizeof boxdir))
            continue;
        m.uid = 0;
        m.flags = f;
        m.name = rows[i].name;
        m.indir = !strcmp(rows[i].sub, "new");
        if (mdsetflags(boxdir, &m, nf, err, sizeof err) < 0) {
            fprintf(stderr, "hml %s: %s/%s: %s\n", cmdname, rows[i].box,
                    rows[i].name, err);
            continue;
        }
        /* the row follows the rename: the same name mdsetflags built */
        flagletters(nf, fl);
        snprintf(name, sizeof name, "%.*s:2,%s",
                 (int)(strstr(rows[i].name, ":2,")
                           ? strstr(rows[i].name, ":2,") - rows[i].name
                           : (long)strlen(rows[i].name)),
                 rows[i].name, fl);
        bindtext(d->mvfile, 1, (nf & FSeen) ? "cur" : rows[i].sub);
        bindtext(d->mvfile, 2, name);
        bindtext(d->mvfile, 3, fl);
        bindtext(d->mvfile, 4, rows[i].box);
        bindtext(d->mvfile, 5, rows[i].base);
        step1(d, d->mvfile);
    }
    for (k = 0; k < arrlen(ops); k++) { /* a flag is never an override */
        bindtext(d->utagdel, 1, mid);
        bindtext(d->utagdel, 2, ops[k].name);
        step1(d, d->utagdel);
    }
    for (i = 0; i < arrlen(rows); i++) {
        free(rows[i].box);
        free(rows[i].base);
        free(rows[i].sub);
        free(rows[i].name);
        free(rows[i].flags);
    }
    arrfree(rows);
    retag(d, id);
}

/* record the overrides for one message and refresh its tags if indexed;
 * flag tags are skipped here (they are files, not overrides — see
 * mirrorflags), which also makes old log lines naming them inert */
static void applyops(Db *d, const char *mid, const Op *ops) {
    ptrdiff_t i;
    sqlite3_int64 id;
    int inv;

    for (i = 0; i < arrlen(ops); i++) {
        if (flagof(ops[i].name, &inv))
            continue;
        bindtext(d->utagset, 1, mid);
        bindtext(d->utagset, 2, ops[i].name);
        sqlite3_bind_int(d->utagset, 3, ops[i].on);
        step1(d, d->utagset);
    }
    bindtext(d->msgbymid, 1, mid);
    if ((id = step1(d, d->msgbymid)) >= 0)
        retag(d, id);
}

static void taglogpath(char *dst, size_t cap) {
    size_t n;

    expand(mailroot, dst, cap);
    n = strlen(dst);
    snprintf(dst + n, cap - n, "/.htags");
}

/* one log line per message: mid TAB +a TAB -b */
static void logline(char **buf, const char *mid, const Op *ops) {
    ptrdiff_t i;

    sadd(buf, mid);
    for (i = 0; i < arrlen(ops); i++) {
        sadd(buf, ops[i].on ? "\t+" : "\t-");
        sadd(buf, ops[i].name);
    }
    sadd(buf, "\n");
}

/* append lines to the log, durably; returns the log size afterwards, or
 * -1. Callers hold the database write lock, which serializes writers */
static long taglogappend(const char *buf, size_t n) {
    char path[4096];
    int fd;
    long size;

    taglogpath(path, sizeof path);
    if ((fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600)) < 0)
        return -1;
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            close(fd);
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    fsync(fd);
    size = (long)lseek(fd, 0, SEEK_END);
    close(fd);
    return size;
}

static long metaint(Db *d, const char *key) {
    long v;

    bindtext(d->metaget, 1, key);
    v = (long)step1(d, d->metaget);
    return v < 0 ? 0 : v;
}

static void metaset(Db *d, const char *key, long v) {
    bindtext(d->metaset, 1, key);
    sqlite3_bind_int64(d->metaset, 2, v);
    step1(d, d->metaset);
}

/* apply whatever the log holds beyond what this index has seen; only
 * complete lines count, a writer may be mid-line */
static void taglogreplay(Db *d) {
    char path[4096], *buf, *line, *nl, *tab;
    long off = metaint(d, "taglog"), size, done;
    FILE *f;
    size_t n;
    Op *ops;

    taglogpath(path, sizeof path);
    if (!(f = fopen(path, "rb")))
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size <= off) {
        fclose(f);
        return;
    }
    fseek(f, off, SEEK_SET);
    buf = malloc((size_t)(size - off) + 1);
    n = fread(buf, 1, (size_t)(size - off), f);
    fclose(f);
    buf[n] = '\0';
    done = off;
    for (line = buf; (nl = strchr(line, '\n')); line = nl + 1) {
        *nl = '\0';
        done = off + (nl - buf) + 1;
        if (!(tab = strchr(line, '\t')))
            continue;
        *tab = '\0';
        ops = NULL;
        if (parseops(tab + 1, &ops) == 0)
            applyops(d, line, ops);
        freeops(ops);
    }
    free(buf);
    metaset(d, "taglog", done);
}

/* config.h rules against the messages this run indexed */
static void applyrules(Db *d) {
    Query q;
    sqlite3_stmt *st;
    char *err = NULL;
    Op *ops;
    int i, rc;

    for (i = 0; i < ntagrules; i++) {
        ops = NULL;
        if (parseops(tagrules[i].tags, &ops) < 0 ||
            querycompile(tagrules[i].query, &q, &err) < 0) {
            fprintf(stderr, "hml new: tag rule %d: %s\n", i,
                    err ? err : "bad tag list");
            free(err);
            err = NULL;
            freeops(ops);
            continue;
        }
        st = queryprep(d->db,
                       "SELECT mid FROM msg WHERE id IN (SELECT id FROM "
                       "newmsg) AND (%s)",
                       &q, "", &err);
        if (!st) {
            fprintf(stderr, "hml new: tag rule %d: %s\n", i, err);
            free(err);
            err = NULL;
        } else {
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
                applyops(d, (const char *)sqlite3_column_text(st, 0), ops);
            if (rc != SQLITE_DONE)
                die(d, "rule");
            sqlite3_finalize(st);
        }
        queryfree(&q);
        freeops(ops);
    }
}

/* --- messages ---------------------------------------------------------- */

/* the thread a new message joins: that of any message it references, or
 * any message referencing it or sharing a reference (JWZ connectivity);
 * several such threads are merged into the lowest. 0 = starts its own */
static sqlite3_int64 threadfor(Db *d, const Mail *m) {
    char *sql = NULL;
    sqlite3_stmt *st;
    sqlite3_int64 first = 0, t;
    ptrdiff_t i, n = arrlen(m->refs);
    int k = 1, rc;

    if (n) {
        sadd(&sql, "SELECT thread FROM msg WHERE mid IN (");
        for (i = 0; i < n; i++)
            sadd(&sql, i ? ",?" : "?");
        sadd(&sql, ") UNION ");
    }
    sadd(&sql, "SELECT msg.thread FROM ref JOIN msg ON msg.id=ref.msg"
               " WHERE ref.mid IN (");
    for (i = 0; i <= n; i++)
        sadd(&sql, i ? ",?" : "?");
    sadd(&sql, ") ORDER BY 1");
    arrput(sql, '\0');
    st = prep(d, sql);
    arrfree(sql);
    for (i = 0; i < n; i++)
        bindtext(st, k++, m->refs[i]);
    for (i = 0; i < n; i++)
        bindtext(st, k++, m->refs[i]);
    bindtext(st, k++, m->mid);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        t = sqlite3_column_int64(st, 0);
        if (!first)
            first = t;
        else if (t != first) {
            sqlite3_bind_int64(d->merge, 1, first);
            sqlite3_bind_int64(d->merge, 2, t);
            step1(d, d->merge);
        }
    }
    if (rc != SQLITE_DONE)
        die(d, "threadfor");
    sqlite3_finalize(st);
    return first;
}

/* --- files ------------------------------------------------------------- */

typedef struct {
    const char *box;    /* "cc/All" */
    const char *boxdir; /* its filesystem path */
    char *sub;          /* "cur" or "new" */
    char *name;         /* maildir file name */
} Job;

static size_t baselen(const char *name) {
    const char *c = strstr(name, ":2,");

    return c ? (size_t)(c - name) : strlen(name);
}

static const char *flagsof(const char *name) {
    const char *c = strstr(name, ":2,");

    return c ? c + 3 : "";
}

static void addfile(Db *d, const Job *j, const Mail *m) {
    sqlite3_int64 id, thread;
    char base[512];
    ptrdiff_t i;

    bindtext(d->msgbymid, 1, m->mid);
    id = step1(d, d->msgbymid);
    if (id < 0) {
        thread = threadfor(d, m);
        bindtext(d->insmsg, 1, m->mid);
        sqlite3_bind_int64(d->insmsg, 2, thread);
        sqlite3_bind_int64(d->insmsg, 3, m->date);
        bindtext(d->insmsg, 4, m->subject);
        bindtext(d->insmsg, 5, m->from);
        sqlite3_bind_int(d->insmsg, 6, m->hasatt);
        bindtext(d->insmsg, 7, m->intent ? m->intent : "");
        step1(d, d->insmsg);
        id = sqlite3_last_insert_rowid(d->db);
        if (!thread) {
            sqlite3_bind_int64(d->setthread, 1, id);
            sqlite3_bind_int64(d->setthread, 2, id);
            step1(d, d->setthread);
        }
        sqlite3_bind_int64(d->insfts, 1, id);
        bindtext(d->insfts, 2, m->subject);
        bindtext(d->insfts, 3, m->from);
        bindtext(d->insfts, 4, m->to);
        bindtext(d->insfts, 5, m->attach);
        bindtext(d->insfts, 6, m->body);
        step1(d, d->insfts);
        for (i = 0; i < arrlen(m->refs); i++) {
            sqlite3_bind_int64(d->insref, 1, id);
            bindtext(d->insref, 2, m->refs[i]);
            step1(d, d->insref);
        }
        sqlite3_bind_int64(d->insnew, 1, id);
        step1(d, d->insnew);
        d->newmsgs++;
    } else if (m->hasatt) {
        /* another file of a known message: it may carry the attachment
         * the first delivery lacked (duplicate list deliveries do) */
        sqlite3_bind_int64(d->setattach, 1, id);
        step1(d, d->setattach);
    }
    snprintf(base, sizeof base, "%.*s", (int)baselen(j->name), j->name);
    bindtext(d->insfile, 1, j->box);
    bindtext(d->insfile, 2, base);
    bindtext(d->insfile, 3, j->sub);
    bindtext(d->insfile, 4, j->name);
    bindtext(d->insfile, 5, flagsof(j->name));
    sqlite3_bind_int64(d->insfile, 6, id);
    step1(d, d->insfile);
    retag(d, id);
    d->added++;
}

static void dropfile(Db *d, const char *box, const char *base,
                     sqlite3_int64 id) {
    bindtext(d->delfile, 1, box);
    bindtext(d->delfile, 2, base);
    step1(d, d->delfile);
    sqlite3_bind_int64(d->nfiles, 1, id);
    if (step1(d, d->nfiles) > 0) {
        retag(d, id);
    } else {
        sqlite3_stmt *by[] = {d->delmsg, d->delfts, d->delref, d->deltag};
        size_t i;
        for (i = 0; i < sizeof by / sizeof *by; i++) {
            sqlite3_bind_int64(by[i], 1, id);
            step1(d, by[i]);
        }
    }
    d->removed++;
}

/* --- scanning ---------------------------------------------------------- */

typedef struct {
    char *key; /* base name */
    char *sub, *name, *flags;
    sqlite3_int64 msg;
    int seen;
} Known;

typedef struct {
    char *path;
    long mtime;
} Dir;

typedef struct {
    Job *jobs;
    Dir *dirs;    /* mtimes to record once every job is in */
    char **boxes; /* every box the scan visited, for prunegone */
    long now;
} Scan;

static int listdir(const char *path, char ***names) {
    DIR *dp;
    struct dirent *e;

    if (!(dp = opendir(path)))
        return -1;
    while ((e = readdir(dp)))
        if (e->d_name[0] != '.')
            arrput(*names, strdup(e->d_name));
    closedir(dp);
    return 0;
}

/* diff one maildir against the index: moves and removals are applied on
 * the spot, new files become jobs for the parsers */
static void scanbox(Db *d, Scan *sc, const char *box, const char *boxdir,
                    int force) {
    static const char *subs[] = {"cur", "new"};
    char path[4160], dirkey[256];
    struct stat st;
    long mtime[2];
    int changed = force, k, rc;
    ptrdiff_t i;
    char **names[2] = {NULL, NULL};
    Known *known = NULL;
    Known *kn;

    arrput(sc->boxes, (char *)box);
    for (k = 0; k < 2; k++) {
        snprintf(path, sizeof path, "%s/%s", boxdir, subs[k]);
        snprintf(dirkey, sizeof dirkey, "%s/%s", box, subs[k]);
        mtime[k] = stat(path, &st) < 0 ? -1 : (long)st.st_mtime;
        bindtext(d->dirget, 1, dirkey);
        if (mtime[k] < 0 || step1(d, d->dirget) != mtime[k])
            changed = 1;
    }
    if (!changed)
        return;
    for (k = 0; k < 2; k++) {
        snprintf(path, sizeof path, "%s/%s", boxdir, subs[k]);
        if (listdir(path, &names[k]) < 0) {
            if (errno != ENOENT)
                fprintf(stderr, "hml new: %s: %s\n", path, strerror(errno));
            continue;
        }
        snprintf(dirkey, sizeof dirkey, "%s/%s", box, subs[k]);
        /* a directory touched this very second may still be changing;
         * leave it unrecorded so the next run looks again */
        if (mtime[k] >= 0 && mtime[k] < sc->now) {
            Dir dd = {strdup(dirkey), mtime[k]};
            arrput(sc->dirs, dd);
        }
    }
    sh_new_strdup(known);
    bindtext(d->boxfiles, 1, box);
    while ((rc = sqlite3_step(d->boxfiles)) == SQLITE_ROW) {
        Known e = {NULL, NULL, NULL, NULL, 0, 0};
        e.key = (char *)sqlite3_column_text(d->boxfiles, 0);
        e.sub = strdup((const char *)sqlite3_column_text(d->boxfiles, 1));
        e.name = strdup((const char *)sqlite3_column_text(d->boxfiles, 2));
        e.flags = strdup((const char *)sqlite3_column_text(d->boxfiles, 3));
        e.msg = sqlite3_column_int64(d->boxfiles, 4);
        shputs(known, e);
    }
    if (rc != SQLITE_DONE)
        die(d, "boxfiles");
    sqlite3_reset(d->boxfiles);
    for (k = 0; k < 2; k++) {
        for (i = 0; i < arrlen(names[k]); i++) {
            char *name = names[k][i], base[512];
            snprintf(base, sizeof base, "%.*s", (int)baselen(name), name);
            if ((kn = shgetp_null(known, base))) {
                kn->seen = 1;
                if (strcmp(kn->name, name) || strcmp(kn->sub, subs[k])) {
                    bindtext(d->mvfile, 1, subs[k]);
                    bindtext(d->mvfile, 2, name);
                    bindtext(d->mvfile, 3, flagsof(name));
                    bindtext(d->mvfile, 4, box);
                    bindtext(d->mvfile, 5, base);
                    step1(d, d->mvfile);
                    retag(d, kn->msg);
                    d->moved++;
                }
                free(name);
            } else {
                Job j = {box, boxdir, (char *)subs[k], name};
                arrput(sc->jobs, j);
            }
        }
        arrfree(names[k]);
    }
    for (i = 0; i < shlen(known); i++) {
        if (!known[i].seen)
            dropfile(d, box, known[i].key, known[i].msg);
        free(known[i].sub);
        free(known[i].name);
        free(known[i].flags);
    }
    shfree(known);
}

/* --- parallel parsing -------------------------------------------------- */

typedef struct {
    long job; /* index into Scan.jobs */
    Mail m;
    int ok;
} Res;

typedef struct {
    Scan *sc;
    long next; /* next job to hand out */
    Res ring[128];
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t notfull, notempty;
} Pool;

static int readfile(const char *path, char **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    struct stat st;
    size_t n;

    if (!f)
        return -1;
    if (fstat(fileno(f), &st) < 0 || !(*buf = malloc((size_t)st.st_size + 1))) {
        fclose(f);
        return -1;
    }
    n = fread(*buf, 1, (size_t)st.st_size, f);
    fclose(f);
    (*buf)[n] = '\0';
    *len = n;
    return 0;
}

static void *worker(void *arg) {
    Pool *p = arg;
    char path[4160], *buf;
    size_t len;
    Res r;

    for (;;) {
        pthread_mutex_lock(&p->mtx);
        r.job = p->next++;
        pthread_mutex_unlock(&p->mtx);
        if (r.job >= arrlen(p->sc->jobs))
            return NULL;
        Job *j = &p->sc->jobs[r.job];
        snprintf(path, sizeof path, "%s/%s/%s", j->boxdir, j->sub, j->name);
        r.ok = 0;
        memset(&r.m, 0, sizeof r.m);
        if (readfile(path, &buf, &len) == 0) {
            mailparse(buf, len, &r.m);
            free(buf);
            r.ok = 1;
        }
        pthread_mutex_lock(&p->mtx);
        while (p->count == (int)(sizeof p->ring / sizeof *p->ring))
            pthread_cond_wait(&p->notfull, &p->mtx);
        p->ring[p->tail] = r;
        p->tail = (p->tail + 1) % (int)(sizeof p->ring / sizeof *p->ring);
        p->count++;
        pthread_cond_signal(&p->notempty);
        pthread_mutex_unlock(&p->mtx);
    }
}

static void index_(Db *d, Scan *sc) {
    Pool p;
    pthread_t tid[32];
    long n = arrlen(sc->jobs), done = 0, nthreads, i;
    int tty = isatty(2);

    memset(&p, 0, sizeof p);
    p.sc = sc;
    pthread_mutex_init(&p.mtx, NULL);
    pthread_cond_init(&p.notfull, NULL);
    pthread_cond_init(&p.notempty, NULL);
    nthreads = sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1)
        nthreads = 1;
    if (nthreads > 32)
        nthreads = 32;
    if (nthreads > n)
        nthreads = n;
    for (i = 0; i < nthreads; i++)
        pthread_create(&tid[i], NULL, worker, &p);
    while (done < n) {
        Res r;
        pthread_mutex_lock(&p.mtx);
        while (!p.count)
            pthread_cond_wait(&p.notempty, &p.mtx);
        r = p.ring[p.head];
        p.head = (p.head + 1) % (int)(sizeof p.ring / sizeof *p.ring);
        p.count--;
        pthread_cond_signal(&p.notfull);
        pthread_mutex_unlock(&p.mtx);
        if (r.ok) {
            addfile(d, &sc->jobs[r.job], &r.m);
            mailfree(&r.m);
        } else
            fprintf(stderr, "hml new: cannot read %s/%s/%s\n",
                    sc->jobs[r.job].box, sc->jobs[r.job].sub,
                    sc->jobs[r.job].name);
        done++;
        if (done % 2000 == 0) {
            exec(d, "COMMIT");
            exec(d, "BEGIN IMMEDIATE");
        }
        if (tty && done % 500 == 0)
            fprintf(stderr, "\r%ld/%ld", done, n);
    }
    if (tty && n >= 500)
        fputs("\r\033[K", stderr);
    for (i = 0; i < nthreads; i++)
        pthread_join(tid[i], NULL);
}

static void scanlocal(Db *d, Scan *sc, const char *root, const char *rel,
                      int depth, int force) {
    char dir[4200], sub[4200], cur[4300], box[512], **names = NULL;
    struct stat st;
    ptrdiff_t i;

    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    if (listdir(dir, &names) < 0) {
        if (errno != ENOENT)
            fprintf(stderr, "hml new: %s: %s\n", dir, strerror(errno));
        return;
    }
    for (i = 0; i < arrlen(names); i++) {
        if (strcmp(names[i], "cur") && strcmp(names[i], "new") &&
            strcmp(names[i], "tmp")) {
            snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", names[i]);
            snprintf(cur, sizeof cur, "%s/%s/cur", root, sub);
            if (stat(cur, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(box, sizeof box, "%s/%.400s", localdomain, sub);
                snprintf(cur, sizeof cur, "%s/%s", root, sub);
                scanbox(d, sc, strdup(box), strdup(cur), force);
            }
            if (depth < 2)
                scanlocal(d, sc, root, sub, depth + 1, force);
        }
        free(names[i]);
    }
    arrfree(names);
}

/* a box whose maildir is gone (renamed, deleted) is never visited by
 * scanbox, so its rows would outlive it: drop every file of every box the
 * scan did not see, and the dir mtimes that would hide it on a rebuild */
static void prunegone(Db *d, Scan *sc) {
    sqlite3_stmt *boxes = prep(d, "SELECT DISTINCT box FROM file");
    sqlite3_stmt *dirdel = prep(d, "DELETE FROM dir WHERE path=? OR path=?");
    char **gone = NULL, key[2][256];
    ptrdiff_t i, j;
    int rc;

    while ((rc = sqlite3_step(boxes)) == SQLITE_ROW) {
        const char *box = (const char *)sqlite3_column_text(boxes, 0);
        for (i = 0; i < arrlen(sc->boxes) && strcmp(sc->boxes[i], box); i++)
            ;
        if (i == arrlen(sc->boxes))
            arrput(gone, strdup(box));
    }
    if (rc != SQLITE_DONE)
        die(d, "boxes");
    sqlite3_finalize(boxes);
    for (j = 0; j < arrlen(gone); j++) {
        Known *known = NULL;
        bindtext(d->boxfiles, 1, gone[j]);
        while ((rc = sqlite3_step(d->boxfiles)) == SQLITE_ROW) {
            Known e = {NULL, NULL, NULL, NULL, 0, 0};
            e.key = strdup((const char *)sqlite3_column_text(d->boxfiles, 0));
            e.msg = sqlite3_column_int64(d->boxfiles, 4);
            arrput(known, e);
        }
        if (rc != SQLITE_DONE)
            die(d, "boxfiles");
        sqlite3_reset(d->boxfiles);
        for (i = 0; i < arrlen(known); i++) {
            dropfile(d, gone[j], known[i].key, known[i].msg);
            free(known[i].key);
        }
        arrfree(known);
        snprintf(key[0], sizeof key[0], "%s/cur", gone[j]);
        snprintf(key[1], sizeof key[1], "%s/new", gone[j]);
        bindtext(dirdel, 1, key[0]);
        bindtext(dirdel, 2, key[1]);
        step1(d, dirdel);
        free(gone[j]);
    }
    arrfree(gone);
    sqlite3_finalize(dirdel);
}

int newmain(int argc, char **argv) {
    Db d;
    Scan sc = {NULL, NULL, NULL, 0};
    char err[256], root[4096], boxdir[4160], box[256];
    struct timespec t0, t1;
    int i, k, force = 0;
    ptrdiff_t j;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-d"))
            force = 1;
        else {
            fputs("usage: hml new [-d]\n", stderr);
            return 2;
        }
    }
    memset(&d, 0, sizeof d);
    if (!(d.db = dbopen(err, sizeof err))) {
        fprintf(stderr, "hml new: %s\n", err);
        return 2;
    }
    prepall(&d);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sc.now = (long)time(NULL);
    exec(&d, "BEGIN IMMEDIATE");
    for (i = 0; i < naccounts; i++) {
        expand(accounts[i].maildir, root, sizeof root);
        for (k = 0; k < accounts[i].nchannels; k++) {
            snprintf(box, sizeof box, "%s/%s", accounts[i].name,
                     accounts[i].channels[k].near);
            snprintf(boxdir, sizeof boxdir, "%s/%s", root,
                     accounts[i].channels[k].near);
            scanbox(&d, &sc, strdup(box), strdup(boxdir), force);
        }
    }
    /* the local boxes: every maildir under localbox, up to three deep
     * (hai/main, hai/s/<session>), named by their relative path */
    expand(localbox, root, sizeof root);
    scanlocal(&d, &sc, root, "", 0, force);
    prunegone(&d, &sc);
    exec(&d, "COMMIT");
    if (arrlen(sc.jobs)) {
        exec(&d, "BEGIN IMMEDIATE");
        index_(&d, &sc);
        exec(&d, "COMMIT");
    }
    exec(&d, "BEGIN IMMEDIATE");
    if (d.newmsgs)
        applyrules(&d);
    taglogreplay(&d); /* after the rules: manual edits win on a rebuild */
    for (j = 0; j < arrlen(sc.dirs); j++) {
        bindtext(d.dirset, 1, sc.dirs[j].path);
        sqlite3_bind_int64(d.dirset, 2, sc.dirs[j].mtime);
        step1(&d, d.dirset);
    }
    exec(&d, "COMMIT");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (d.added || d.moved || d.removed)
        printf("hml new: %ld files added (%ld new messages), %ld moved, "
               "%ld removed in %.1fs\n",
               d.added, d.newmsgs, d.moved, d.removed,
               (double)(t1.tv_sec - t0.tv_sec) +
                   (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    sqlite3_close_v2(d.db);
    return 0;
}

int tagmain(int argc, char **argv) {
    Db d;
    Query q;
    sqlite3_stmt *st;
    Op *ops = NULL, *flagops = NULL, *tagops = NULL;
    char *spec = NULL, *query = NULL, *log = NULL, *err = NULL, dberr[256];
    long size;
    int i, rc;

    cmdname = "tag";
    for (i = 0; i < argc && (argv[i][0] == '+' || argv[i][0] == '-'); i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        sadd(&spec, argv[i]);
        sadd(&spec, " ");
    }
    for (; i < argc; i++) {
        if (query)
            sadd(&query, " ");
        sadd(&query, argv[i]);
    }
    if (spec)
        arrput(spec, '\0');
    if (query)
        arrput(query, '\0');
    if (!spec || !query || parseops(spec, &ops) < 0 || !arrlen(ops)) {
        fputs("usage: hml tag +tag|-tag ... [--] <query>\n", stderr);
        arrfree(spec);
        arrfree(query);
        freeops(ops);
        return 2;
    }
    for (i = 0; i < arrlen(ops); i++)
        if (strpbrk(ops[i].name, " \t\n")) {
            fprintf(stderr, "hml tag: bad tag '%s'\n", ops[i].name);
            freeops(ops);
            return 2;
        }
    if (querycompile(query, &q, &err) < 0) {
        fprintf(stderr, "hml tag: %s\n", err);
        free(err);
        freeops(ops);
        return 2;
    }
    memset(&d, 0, sizeof d);
    if (!(d.db = dbopen(dberr, sizeof dberr))) {
        fprintf(stderr, "hml tag: %s\n", dberr);
        return 2;
    }
    /* flag tags act on the files, the rest are logged overrides */
    for (i = 0; i < arrlen(ops); i++) {
        int inv;
        if (flagof(ops[i].name, &inv))
            arrput(flagops, ops[i]);
        else
            arrput(tagops, ops[i]);
    }
    prepall(&d);
    exec(&d, "BEGIN IMMEDIATE");
    /* overrides of flag names from before flags were mirrored are inert
     * now; drop them so they can never shadow the real maildir state */
    exec(&d, "DELETE FROM utag WHERE name IN "
             "('unread','flagged','replied','passed')");
    if (!(st = queryprep(d.db, "SELECT id,mid FROM msg WHERE %s", &q, "",
                         &err))) {
        fprintf(stderr, "hml tag: %s\n", err);
        return 2;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        const char *mid = (const char *)sqlite3_column_text(st, 1);
        if (strpbrk(mid, "\t\n"))
            continue; /* cannot be logged faithfully; never seen */
        if (arrlen(flagops))
            mirrorflags(&d, id, mid, flagops);
        if (arrlen(tagops)) {
            logline(&log, mid, tagops);
            applyops(&d, mid, tagops);
        }
    }
    if (rc != SQLITE_DONE)
        die(&d, "tag");
    sqlite3_finalize(st);
    if (arrlen(log)) {
        if ((size = taglogappend(log, arrlenu(log))) < 0) {
            fprintf(stderr, "hml tag: cannot write tag log: %s\n",
                    strerror(errno));
            exec(&d, "ROLLBACK");
            return 2;
        }
        metaset(&d, "taglog", size);
    }
    exec(&d, "COMMIT");
    sqlite3_close_v2(d.db);
    arrfree(log);
    queryfree(&q);
    arrfree(flagops); /* the names belong to ops */
    arrfree(tagops);
    freeops(ops);
    arrfree(spec);
    arrfree(query);
    return 0;
}

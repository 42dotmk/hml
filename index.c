/* index.c - "hml new": the search index, SQLite + FTS5 at
 * <mailroot>/.hml.db. One row per message (Message-ID), one per maildir
 * file, tags derived from flags and folders, threads from References. The
 * index is an hml-only cache: deleting it costs a rebuild, nothing else. */
#include <dirent.h>
#include <errno.h>
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
    " subject TEXT NOT NULL, sender TEXT NOT NULL);"
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
    "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(subject, sender, rcpt,"
    " attach, body, content='', contentless_delete=1,"
    " tokenize='unicode61 remove_diacritics 2');";

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
    return db;
}

/* --- statements -------------------------------------------------------- */

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *msgbymid, *insmsg, *setthread, *merge, *insfts, *delfts,
        *insref, *delref, *insfile, *mvfile, *delfile, *filemsg, *nfiles,
        *delmsg, *deltag, *instag, *msgfiles, *boxfiles, *dirget, *dirset;
    long added, moved, removed, newmsgs;
} Db;

static void die(Db *d, const char *what) {
    fprintf(stderr, "hml new: %s: %s\n", what, sqlite3_errmsg(d->db));
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
    d->insmsg = prep(d, "INSERT INTO msg(mid,thread,date,subject,sender)"
                        " VALUES(?,?,?,?,?)");
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

static void exec(Db *d, const char *sql) {
    if (sqlite3_exec(d->db, sql, NULL, NULL, NULL) != SQLITE_OK)
        die(d, sql);
}

/* --- tags -------------------------------------------------------------- */

/* recompute the derived tags of one message from its files */
static void retag(Db *d, sqlite3_int64 id) {
    const char *tags[16];
    char flags[64] = "";
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
                tags[nt++] = foldertags[i].tag;
        }
    }
    if (rc != SQLITE_DONE)
        die(d, "msgfiles");
    sqlite3_reset(d->msgfiles);
    if (!folder)
        tags[nt++] = "inbox";
    if (!strchr(flags, 'S'))
        tags[nt++] = "unread";
    if (strchr(flags, 'F'))
        tags[nt++] = "flagged";
    if (strchr(flags, 'R'))
        tags[nt++] = "replied";
    if (strchr(flags, 'D'))
        tags[nt++] = "draft";
    if (strchr(flags, 'P'))
        tags[nt++] = "passed";
    sqlite3_bind_int64(d->deltag, 1, id);
    step1(d, d->deltag);
    for (i = 0; i < nt; i++) {
        sqlite3_bind_int64(d->instag, 1, id);
        bindtext(d->instag, 2, tags[i]);
        step1(d, d->instag);
    }
}

/* --- messages ---------------------------------------------------------- */

/* the thread a new message joins: that of any message it references, or
 * any message referencing it or sharing a reference (JWZ connectivity);
 * several such threads are merged into the lowest. 0 = starts its own */
static void sadd(char **s, const char *t) {
    memcpy(arraddnptr(*s, strlen(t)), t, strlen(t));
}

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
        d->newmsgs++;
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
    Dir *dirs; /* mtimes to record once every job is in */
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
            exec(d, "BEGIN");
        }
        if (tty && done % 500 == 0)
            fprintf(stderr, "\r%ld/%ld", done, n);
    }
    if (tty && n >= 500)
        fputs("\r\033[K", stderr);
    for (i = 0; i < nthreads; i++)
        pthread_join(tid[i], NULL);
}

int newmain(int argc, char **argv) {
    Db d;
    Scan sc = {NULL, NULL, 0};
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
    exec(&d, "BEGIN");
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
    exec(&d, "COMMIT");
    if (arrlen(sc.jobs)) {
        exec(&d, "BEGIN");
        index_(&d, &sc);
        exec(&d, "COMMIT");
    }
    exec(&d, "BEGIN");
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

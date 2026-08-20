#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <stb_ds.h>

#include "hml.h"

static pthread_mutex_t seqmtx = PTHREAD_MUTEX_INITIALIZER;
static int seq;

static int nextseq(void) {
    int s;

    pthread_mutex_lock(&seqmtx);
    s = ++seq;
    pthread_mutex_unlock(&seqmtx);
    return s;
}

static const char *shorthost(void) {
    static char host[64];

    if (!host[0]) {
        char *dot;
        if (gethostname(host, sizeof host - 1) < 0)
            strcpy(host, "localhost");
        host[sizeof host - 1] = '\0';
        if ((dot = strchr(host, '.'))) /* dots would break maildir parsing */
            *dot = '\0';
    }
    return host;
}

/* filenames look like "1778851759.23580_1.host,U=123:2,S" - mbsync stores
 * the server uid after ,U= and maildir keeps flag letters after :2, */
static int scanone(const char *boxdir, const char *sub, int indir, Box *box,
                   char *err, size_t errlen) {
    char path[4160];
    DIR *d;
    struct dirent *e;

    snprintf(path, sizeof path, "%s/%s", boxdir, sub);
    if (!(d = opendir(path))) {
        snprintf(err, errlen, "opendir %s: %s", path, strerror(errno));
        return -1;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        Local m = {0, 0, NULL, indir};
        const char *u = strstr(e->d_name, ",U=");
        const char *fl = strstr(e->d_name, ":2,");
        if (u)
            m.uid = (uint32_t)strtoul(u + 3, NULL, 10);
        if (!m.uid)
            box->nouid++;
        if (fl)
            m.flags = letterflags(fl + 3);
        m.name = strdup(e->d_name);
        arrput(box->msgs, m);
    }
    closedir(d);
    return 0;
}

/* create the maildir and its cur/new/tmp, parents included; 0700 like
 * mbsync, existing directories (whatever their mode) are left alone */
int mdensure(const char *boxdir, char *err, size_t errlen) {
    char p[4160];
    size_t i, n = strlen(boxdir);
    static const char *sub[] = {"cur", "new", "tmp"};
    int k;

    if (n == 0 || n >= sizeof p - 5) {
        snprintf(err, errlen, "bad maildir path");
        return -1;
    }
    memcpy(p, boxdir, n + 1);
    for (i = 1; i <= n; i++) {
        if (i < n && p[i] != '/')
            continue;
        p[i] = '\0';
        if (mkdir(p, 0700) < 0 && errno != EEXIST) {
            snprintf(err, errlen, "mkdir %s: %s", p, strerror(errno));
            return -1;
        }
        if (i < n)
            p[i] = '/';
    }
    for (k = 0; k < 3; k++) {
        snprintf(p, sizeof p, "%s/%s", boxdir, sub[k]);
        if (mkdir(p, 0700) < 0 && errno != EEXIST) {
            snprintf(err, errlen, "mkdir %s: %s", p, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int boxscan(const char *boxdir, Box *box, char *err, size_t errlen) {
    memset(box, 0, sizeof *box);
    if (scanone(boxdir, "cur", 0, box, err, errlen) < 0 ||
        scanone(boxdir, "new", 1, box, err, errlen) < 0)
        return -1;
    return 0;
}

void boxfree(Box *box) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(box->msgs); i++)
        free(box->msgs[i].name);
    arrfree(box->msgs);
}

int mdtmp(char *dst, size_t cap, const char *boxdir) {
    snprintf(dst, cap, "%s/tmp/hml.%d.%d", boxdir, (int)getpid(), nextseq());
    return 0;
}

/* move a downloaded message from tmp/ into the maildir; unseen mail goes to
 * new/, everything else to cur/, matching what mbsync produces */
int mdplace(const char *boxdir, uint32_t nuid, unsigned flags,
            const char *tmppath, char *err, size_t errlen) {
    char path[4160], fl[8];

    flagletters(flags, fl);
    snprintf(path, sizeof path, "%s/%s/%ld.%d_%d.%s,U=%u:2,%s", boxdir,
             (flags & FSeen) ? "cur" : "new", (long)time(NULL), (int)getpid(),
             nextseq(), shorthost(), nuid, fl);
    if (rename(tmppath, path) < 0) {
        snprintf(err, errlen, "rename into %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* strip the ":2,..." info suffix; the result is the immutable base name */
static size_t basename_len(const char *name) {
    const char *colon = strstr(name, ":2,");

    return colon ? (size_t)(colon - name) : strlen(name);
}

static int mdrename(const char *boxdir, const Local *m, const char *newsub,
                    const char *newname, char *err, size_t errlen) {
    char oldp[4160], newp[4160];

    snprintf(oldp, sizeof oldp, "%s/%s/%s", boxdir, m->indir ? "new" : "cur",
             m->name);
    snprintf(newp, sizeof newp, "%s/%s/%s", boxdir, newsub, newname);
    if (rename(oldp, newp) < 0) {
        snprintf(err, errlen, "rename %s: %s", m->name, strerror(errno));
        return -1;
    }
    return 0;
}

int mdsetflags(const char *boxdir, const Local *m, unsigned flags, char *err,
               size_t errlen) {
    char name[512], fl[8];
    size_t blen = basename_len(m->name);
    const char *sub;

    if (blen + 12 > sizeof name) {
        snprintf(err, errlen, "filename too long: %s", m->name);
        return -1;
    }
    flagletters(flags, fl);
    snprintf(name, sizeof name, "%.*s:2,%s", (int)blen, m->name, fl);
    /* seen mail graduates from new/ to cur/; it never moves back */
    sub = (flags & FSeen) ? "cur" : (m->indir ? "new" : "cur");
    return mdrename(boxdir, m, sub, name, err, errlen);
}

/* give a locally-new message its near uid after a successful push */
int mdassignuid(const char *boxdir, const Local *m, uint32_t nuid, char *err,
                size_t errlen) {
    char name[512], fl[8];
    size_t blen = basename_len(m->name);

    if (blen + 24 > sizeof name) {
        snprintf(err, errlen, "filename too long: %s", m->name);
        return -1;
    }
    flagletters(m->flags, fl);
    snprintf(name, sizeof name, "%.*s,U=%u:2,%s", (int)blen, m->name, nuid, fl);
    return mdrename(boxdir, m, m->indir ? "new" : "cur", name, err, errlen);
}

int mddelete(const char *boxdir, const Local *m, char *err, size_t errlen) {
    char path[4160];

    snprintf(path, sizeof path, "%s/%s/%s", boxdir, m->indir ? "new" : "cur",
             m->name);
    if (unlink(path) < 0 && errno != ENOENT) {
        snprintf(err, errlen, "unlink %s: %s", m->name, strerror(errno));
        return -1;
    }
    return 0;
}

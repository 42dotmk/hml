/* sync.c - the engine behind status, dry-run and sync. Computes the exact
 * three-way difference between the IMAP mailbox, the maildir and mbsync's
 * sync state, then reports or applies it. The .hmlstate cache (hml-only,
 * always safe to lose) lets an unchanged folder be skipped after a single
 * SELECT round-trip - the fast path mbsync never had. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <stb_ds.h>

#include "hml.h"

/* \Deleted is deliberately NOT part of flag sync: mbsync treats it as
 * deletion state, handled by the gone/expunge machinery, and observably
 * leaves T drift alone in no-Expunge channels. We must match. */
#define SYNCFLAGS (FSeen | FAnswered | FFlagged | FDraft | FPassed)
#define BATCH 32  /* state rewrites during long pull/push runs */
#define CHUNK 40  /* uids per STORE/EXPUNGE command */
#define DRYMAX 12 /* itemized dry-run lines per box */

typedef struct {
  uint32_t key;
  unsigned value;
} Uidmap;

typedef struct {
  uint32_t exists, uidvalidity, uidnext;
  uint64_t modseq;
} Sel;

typedef struct { /* .hmlstate: cheap skip-everything baseline */
  int valid;
  uint32_t uidval, uidnext, exists;
  uint64_t modseq;
  long ghosts, nlocal;
  long curs, curn, news, newn; /* cur/ and new/ mtime sec+nsec */
} Cache;

typedef struct {
  uint32_t uid;
  unsigned flags;
} NewMsg;

typedef struct { /* far-side flag change to STORE */
  uint32_t uid;
  unsigned add, rem;
} Fact;

typedef struct { /* near-side flag change to apply by rename */
  int idx;
  unsigned flags;
} Nact;

static void addf(char *d, size_t cap, const char *fmt, ...) {
  size_t len = strlen(d);
  va_list ap;

  if (len && len + 2 < cap) {
    d[len++] = ',';
    d[len++] = ' ';
    d[len] = '\0';
  }
  va_start(ap, fmt);
  vsnprintf(d + len, cap - len, fmt, ap);
  va_end(ap);
}

static void expand(const char *path, char *dst, size_t cap) {
  if (path[0] == '~')
    snprintf(dst, cap, "%s%s", getenv("HOME"), path + 1);
  else
    snprintf(dst, cap, "%s", path);
}

/* same lock mbsync takes, so the two can never run on a folder at once */
static int lockstate(const char *boxdir) {
  char path[4160];
  struct flock fl;
  int fd;

  snprintf(path, sizeof path, "%s/.mbsyncstate.lock", boxdir);
  if ((fd = open(path, O_WRONLY | O_CREAT, 0666)) < 0)
    return -1;
  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (fcntl(fd, F_SETLK, &fl) < 0) {
    close(fd);
    return -2;
  }
  return fd;
}

static void selcb(const char *l, void *ud) {
  Sel *s = ud;
  uint32_t u;
  uint64_t m;
  char word[16];

  if (sscanf(l, "* %u %15s", &u, word) == 2 && !strcmp(word, "EXISTS"))
    s->exists = u;
  else if (sscanf(l, "* OK [UIDVALIDITY %u]", &u) == 1)
    s->uidvalidity = u;
  else if (sscanf(l, "* OK [UIDNEXT %u]", &u) == 1)
    s->uidnext = u;
  else if (sscanf(l, "* OK [HIGHESTMODSEQ %" SCNu64 "]", &m) == 1)
    s->modseq = m;
}

/* "* n FETCH (UID u FLAGS (...))" -> map uid to flags */
static void mapcb(const char *l, void *ud) {
  Uidmap **m = ud;
  const char *u, *fl;

  if (!strstr(l, " FETCH ") || !(u = strstr(l, "UID ")))
    return;
  fl = strstr(l, "FLAGS (");
  hmput(*m, (uint32_t)strtoul(u + 4, NULL, 10), fl ? imapflags(fl) : 0);
}

/* "* SEARCH n n n" -> set of uids */
static void setcb(const char *l, void *ud) {
  Uidmap **m = ud;
  const char *p;
  char *end;

  if (strncmp(l, "* SEARCH", 8) != 0)
    return;
  for (p = l + 8;; p = end) {
    unsigned long u = strtoul(p, &end, 10);
    if (end == p)
      break;
    hmput(*m, (uint32_t)u, 1);
  }
}

typedef struct {
  uint32_t min;
  NewMsg *arr;
  long ghostnew; /* already \Deleted on arrival: a ghost, not new mail */
} NewCtx;

static void newcb(const char *l, void *ud) {
  NewCtx *c = ud;
  const char *u, *fl;
  uint32_t uid;
  unsigned f;

  if (!strstr(l, " FETCH ") || !(u = strstr(l, "UID ")))
    return;
  uid = (uint32_t)strtoul(u + 4, NULL, 10);
  if (uid <= c->min) /* the n:* quirk echoes the last message back */
    return;
  fl = strstr(l, "FLAGS (");
  f = fl ? imapflags(fl) : 0;
  if (f & FDeleted) {
    c->ghostnew++;
  } else {
    NewMsg m;
    m.uid = uid;
    m.flags = f;
    arrput(c->arr, m);
  }
}

static int cacheload(const char *boxdir, Cache *c) {
  char path[4160], line[128];
  FILE *f;
  int ver = 0;

  memset(c, 0, sizeof *c);
  snprintf(path, sizeof path, "%s/.hmlstate", boxdir);
  if (!(f = fopen(path, "r")))
    return 0;
  while (fgets(line, sizeof line, f)) {
    sscanf(line, "HmlState %d", &ver);
    sscanf(line, "UidValidity %u", &c->uidval);
    sscanf(line, "UidNext %u", &c->uidnext);
    sscanf(line, "Exists %u", &c->exists);
    sscanf(line, "Modseq %" SCNu64, &c->modseq);
    sscanf(line, "Ghosts %ld", &c->ghosts);
    sscanf(line, "Local %ld", &c->nlocal);
    sscanf(line, "CurMtime %ld %ld", &c->curs, &c->curn);
    sscanf(line, "NewMtime %ld %ld", &c->news, &c->newn);
  }
  fclose(f);
  c->valid = (ver == 2);
  return 0;
}

static void cachesave(const char *boxdir, const Cache *c) {
  char tmp[4160], path[4160];
  FILE *f;

  snprintf(tmp, sizeof tmp, "%s/.hmlstate.tmp", boxdir);
  snprintf(path, sizeof path, "%s/.hmlstate", boxdir);
  if (!(f = fopen(tmp, "w")))
    return; /* cache is optional; losing it only costs speed */
  fprintf(f,
          "HmlState 2\nUidValidity %u\nUidNext %u\nExists %u\n"
          "Modseq %" PRIu64 "\nGhosts %ld\nLocal %ld\nCurMtime %ld %ld\n"
          "NewMtime %ld %ld\n",
          c->uidval, c->uidnext, c->exists, c->modseq, c->ghosts, c->nlocal,
          c->curs, c->curn, c->news, c->newn);
  fclose(f);
  rename(tmp, path);
}

static void statmtimes(const char *boxdir, Cache *c) {
  char path[4160];
  struct stat sb;

  snprintf(path, sizeof path, "%s/cur", boxdir);
  if (stat(path, &sb) == 0) {
    c->curs = (long)sb.st_mtim.tv_sec;
    c->curn = sb.st_mtim.tv_nsec;
  }
  snprintf(path, sizeof path, "%s/new", boxdir);
  if (stat(path, &sb) == 0) {
    c->news = (long)sb.st_mtim.tv_sec;
    c->newn = sb.st_mtim.tv_nsec;
  }
}

/* build a comma-separated uid list; advances *i, returns how many fit */
static int uidstr(char *dst, size_t cap, const uint32_t *uids, long n,
                  long *i) {
  int cnt = 0, l;
  size_t len = 0;

  dst[0] = '\0';
  while (*i < n && cnt < CHUNK) {
    char one[16];
    l = snprintf(one, sizeof one, "%s%u", cnt ? "," : "", uids[*i]);
    if (len + (size_t)l + 1 >= cap)
      break;
    memcpy(dst + len, one, (size_t)l + 1);
    len += (size_t)l;
    (*i)++;
    cnt++;
  }
  return cnt;
}

/* group identical flag deltas and STORE them in batched commands */
static int storeflags(Imap *im, Fact *facts, int rem, char *err,
                      size_t errlen) {
  long i, j, k;

  for (i = 0; i < arrlen(facts); i++) {
    unsigned m = rem ? facts[i].rem : facts[i].add;
    uint32_t *uids = NULL;
    char fs[96], list[720];
    if (!m)
      continue;
    for (j = i; j < arrlen(facts); j++) {
      unsigned mj = rem ? facts[j].rem : facts[j].add;
      if (mj == m) {
        arrput(uids, facts[j].uid);
        if (rem)
          facts[j].rem = 0;
        else
          facts[j].add = 0;
      }
    }
    imapflagstr(m, fs, sizeof fs);
    k = 0;
    while (k < arrlen(uids)) {
      if (!uidstr(list, sizeof list, uids, arrlen(uids), &k))
        break;
      if (imapexec(im, NULL, NULL, err, errlen, "UID STORE %s %cFLAGS.SILENT (%s)",
                   list, rem ? '-' : '+', fs) < 0) {
        arrfree(uids);
        return -1;
      }
    }
    arrfree(uids);
  }
  return 0;
}

int syncbox(Imap *im, const Account *a, const Channel *ch, int mode,
            int force) {
  char boxdir[4096], root[2048], label[64], qfar[256], err[256], jpath[4160];
  char d[512] = "";
  State st;
  Box box;
  Sel sel;
  Cache cache, ncache;
  Uidmap *farflags = NULL, *vanished = NULL, *stfarset = NULL,
         *stnearset = NULL, *locbyn = NULL;
  NewMsg *news = NULL;
  Fact *facts = NULL;
  Nact *nacts = NULL;
  int *ndel = NULL, *pushi = NULL;
  uint32_t *expu = NULL;
  uint32_t uvval = 0, uvlast = 0;
  long i, alive = 0, vgone = 0, lgone = 0, tdrift = 0, untracked = 0;
  long remuntracked = 0, ghostsnow = 0;
  long npulled = 0, npushed = 0, dryn = 0;
  int lfd, rc = 0, scanned = 0, farfull = 0, journal, mutated = 0;
  int stchanged = 0; /* pair structure needs persisting even without acts */

  memset(&st, 0, sizeof st);
  memset(&box, 0, sizeof box);
  snprintf(label, sizeof label, "%s/%s", a->name, ch->near);
  expand(a->maildir, root, sizeof root);
  snprintf(boxdir, sizeof boxdir, "%s/%s", root, ch->near);
  imapquote(qfar, sizeof qfar, ch->far);

  /* from scratch: recv bootstraps the maildir tree, then the empty state
   * below makes everything on the server "new" - a full clone */
  if (mode == MSync) {
    if (mdensure(boxdir, err, sizeof err) < 0) {
      report(label, "error: %s", err);
      return 2;
    }
  } else {
    char curp[4160];
    struct stat sb;
    snprintf(curp, sizeof curp, "%s/cur", boxdir);
    if (stat(curp, &sb) < 0) {
      report(label, "no maildir yet (hml recv creates it and clones)");
      return 1;
    }
  }

  if ((lfd = lockstate(boxdir)) == -2) {
    report(label, "locked (mbsync running?), skipped");
    return 1;
  }
  if (lfd < 0) {
    report(label, "error: cannot create lock in %s", boxdir);
    return 2;
  }
  snprintf(jpath, sizeof jpath, "%s/.mbsyncstate.journal", boxdir);
  journal = access(jpath, F_OK) == 0;
  if (journal && mode == MSync) {
    report(label, "error: mbsync journal present (interrupted sync); "
                  "run mbsync on this channel to recover first");
    close(lfd);
    return 2;
  }

  if (stateload(boxdir, &st, err, sizeof err) < 0 ||
      uvload(boxdir, &uvval, &uvlast, err, sizeof err) < 0) {
    report(label, "error: %s", err);
    close(lfd);
    return 2;
  }
  if (st.present && uvval && uvval != st.nuidval) {
    report(label, "error: .uidvalidity/.mbsyncstate disagree (%u vs %u)",
           uvval, st.nuidval);
    close(lfd);
    return 2;
  }
  cacheload(boxdir, &cache);
  if (force)
    cache.valid = 0;

  memset(&sel, 0, sizeof sel);
  if (imapexec(im, selcb, &sel, err, sizeof err, "%s %s (CONDSTORE)",
               mode == MSync ? "SELECT" : "EXAMINE", qfar) < 0 &&
      imapexec(im, selcb, &sel, err, sizeof err, "%s %s",
               mode == MSync ? "SELECT" : "EXAMINE", qfar) < 0) {
    report(label, "error: cannot open mailbox: %s", err);
    close(lfd);
    statefree(&st);
    return 2;
  }
  if (st.present && st.fuidval && sel.uidvalidity != st.fuidval) {
    report(label, "UIDVALIDITY MISMATCH (state %u, server %u), refusing",
           st.fuidval, sel.uidvalidity);
    close(lfd);
    statefree(&st);
    return 2;
  }
  if (cache.valid && cache.uidval != sel.uidvalidity)
    cache.valid = 0;

  /* fast path: server counters and local mtimes both untouched */
  statmtimes(boxdir, &ncache);
  if (cache.valid && sel.uidnext == cache.uidnext &&
      sel.exists == cache.exists && sel.modseq == cache.modseq &&
      ncache.curs == cache.curs && ncache.curn == cache.curn &&
      ncache.news == cache.news && ncache.newn == cache.newn) {
    report(label, "remote %6u  local %6ld  in sync (fast)", sel.exists,
           cache.nlocal);
    close(lfd);
    statefree(&st);
    return 0;
  }

  if (boxscan(boxdir, &box, err, sizeof err) < 0) {
    report(label, "error: %s", err);
    close(lfd);
    statefree(&st);
    return 2;
  }
  scanned = 1;

  if (!st.present) {
    st.fuidval = sel.uidvalidity;
    st.nuidval = uvval ? uvval : (uint32_t)time(NULL);
  }
  if (!uvval)
    uvval = st.nuidval;

  for (i = 0; i < arrlen(st.pairs); i++) {
    if (st.pairs[i].fuid) {
      hmput(stfarset, st.pairs[i].fuid, 1);
      alive++;
    }
    if (st.pairs[i].nuid)
      hmput(stnearset, st.pairs[i].nuid, 1);
  }
  for (i = 0; i < arrlen(box.msgs); i++) {
    if (!box.msgs[i].uid)
      continue;
    hmput(locbyn, box.msgs[i].uid, (unsigned)i);
    if (hmgeti(stnearset, box.msgs[i].uid) < 0)
      untracked++;
  }

  /* far side: new messages first */
  if (sel.exists && (!st.present || sel.uidnext > st.maxpulled + 1)) {
    NewCtx nc = {st.maxpulled, NULL, 0};
    if (imapexec(im, newcb, &nc, err, sizeof err, "UID FETCH %u:* (UID FLAGS)",
                 st.maxpulled + 1) < 0) {
      report(label, "error: FETCH new: %s", err);
      rc = 2;
      goto out;
    }
    news = nc.arr;
    ghostsnow = nc.ghostnew;
  }

  /* far side: flag changes and vanished messages */
  if (st.present && st.maxpulled && sel.exists) {
    if (cache.valid && cache.modseq && sel.modseq &&
        sel.modseq != cache.modseq) {
      if (imapexec(im, mapcb, &farflags, err, sizeof err,
                   "UID FETCH 1:%u (UID FLAGS) (CHANGEDSINCE %" PRIu64 ")",
                   st.maxpulled, cache.modseq) < 0) {
        report(label, "error: FETCH changed: %s", err);
        rc = 2;
        goto out;
      }
    } else if (!cache.valid) {
      /* no baseline yet: one full listing, exactly what mbsync does on
       * every run - hml pays it once and caches */
      if (imapexec(im, mapcb, &farflags, err, sizeof err,
                   "UID FETCH 1:%u (UID FLAGS)", st.maxpulled) < 0) {
        report(label, "error: FETCH all: %s", err);
        rc = 2;
        goto out;
      }
      farfull = 1;
    }
  }
  if (farfull) {
    for (i = 0; i < arrlen(st.pairs); i++)
      if (st.pairs[i].fuid && hmgeti(farflags, st.pairs[i].fuid) < 0)
        hmput(vanished, st.pairs[i].fuid, 1);
    for (i = 0; i < hmlen(farflags); i++)
      if (hmgeti(stfarset, farflags[i].key) < 0) {
        if (farflags[i].value & FDeleted)
          ghostsnow++;
        else
          remuntracked++;
      }
  } else if (st.present) {
    long expected;
    ghostsnow += cache.valid ? cache.ghosts : 0;
    expected = alive + arrlen(news) + ghostsnow;
    if ((long)sel.exists != expected) {
      /* counts disagree: recount the \Deleted ghosts, and if that still
       * does not explain it, list the mailbox to find what vanished */
      Uidmap *delset = NULL;
      if (imapexec(im, setcb, &delset, err, sizeof err,
                   "UID SEARCH DELETED") < 0) {
        report(label, "error: SEARCH: %s", err);
        rc = 2;
        goto out;
      }
      ghostsnow = 0;
      for (i = 0; i < hmlen(delset); i++)
        if (hmgeti(stfarset, delset[i].key) < 0)
          ghostsnow++;
      hmfree(delset);
      expected = alive + arrlen(news) + ghostsnow;
      if ((long)sel.exists != expected) {
        Uidmap *server = NULL;
        long van = 0;
        if (imapexec(im, setcb, &server, err, sizeof err,
                     "UID SEARCH ALL") < 0) {
          report(label, "error: SEARCH: %s", err);
          rc = 2;
          goto out;
        }
        for (i = 0; i < arrlen(st.pairs); i++)
          if (st.pairs[i].fuid &&
              hmgeti(server, st.pairs[i].fuid) < 0) {
            hmput(vanished, st.pairs[i].fuid, 1);
            van++;
          }
        ghostsnow = hmlen(server) - (alive - van) - arrlen(news);
        if (ghostsnow < 0)
          ghostsnow = 0;
        hmfree(server);
      }
    }
  }

  /* the merge: walk every pair, decide its fate */
  for (i = 0; i < arrlen(st.pairs); i++) {
    Pair *p = &st.pairs[i];
    long k = p->nuid ? hmgeti(locbyn, p->nuid) : -1;
    int hasnear = k >= 0;
    int midx = hasnear ? (int)locbyn[k].value : -1;
    unsigned nearf = hasnear ? box.msgs[midx].flags : 0;
    int hasfar = p->fuid && (!vanished || hmgeti(vanished, p->fuid) < 0);
    unsigned farf = p->flags;
    unsigned stf, ff, nf, cf, cn, merged;
    long q;

    if (p->dead)
      continue;
    if (hasfar && farflags && (q = hmgeti(farflags, p->fuid)) >= 0)
      farf = farflags[q].value;

    if (p->fuid && !hasfar && (!p->nuid || !hasnear)) {
      vgone++;
      p->dead = 1;
      stchanged = 1;
      continue;
    }
    if (p->nuid && !hasnear && !p->fuid) {
      p->dead = 1;
      stchanged = 1;
      continue;
    }
    if (p->fuid && !hasfar) { /* far vanished, near still here */
      vgone++;
      stchanged = 1;
      if (ch->expunge) {
        arrput(ndel, midx);
        p->dead = 1;
      } else {
        if (!(nearf & FDeleted)) {
          Nact na = {midx, nearf | FDeleted};
          arrput(nacts, na);
        }
        p->fuid = 0;
        p->flags = nearf | FDeleted;
      }
      continue;
    }
    if (p->nuid && !hasnear) { /* near gone, far still here */
      lgone++;
      stchanged = 1;
      if (!(farf & FDeleted)) {
        Fact fa = {p->fuid, FDeleted, 0};
        arrput(facts, fa);
      }
      if (ch->expunge) {
        arrput(expu, p->fuid);
        p->dead = 1;
      } else {
        p->nuid = 0;
        p->flags = farf | FDeleted;
      }
      continue;
    }
    if (!p->fuid || !p->nuid) /* half-dead from a no-expunge past */
      continue;

    if (ch->expunge && ((nearf | farf) & FDeleted)) {
      /* marked for death on either side: Expunge Both removes it */
      arrput(ndel, midx);
      if (!(farf & FDeleted)) {
        Fact fa = {p->fuid, FDeleted, 0};
        arrput(facts, fa);
      }
      arrput(expu, p->fuid);
      p->dead = 1;
      stchanged = 1;
      continue;
    }
    if (!ch->expunge && ((nearf ^ farf) & FDeleted))
      tdrift++;

    stf = p->flags & SYNCFLAGS;
    ff = farf & SYNCFLAGS;
    nf = nearf & SYNCFLAGS;
    cf = ff ^ stf;
    cn = nf ^ stf;
    merged = (ff & cf) | (nf & cn & ~cf) | (stf & ~cf & ~cn);
    if (merged != nf) {
      Nact na = {midx, merged | (nearf & FDeleted)};
      arrput(nacts, na);
    }
    if (merged != ff) {
      Fact fa = {p->fuid, merged & ~ff, ff & ~merged};
      arrput(facts, fa);
    }
    p->flags = merged;
  }
  for (i = 0; i < arrlen(box.msgs); i++)
    if (!box.msgs[i].uid)
      arrput(pushi, (int)i);

  {
    long acts = arrlen(news) + arrlen(pushi) + arrlen(facts) +
                arrlen(nacts) + arrlen(ndel) + arrlen(expu);

    if (mode == MDry) {
      for (i = 0; i < arrlen(news) && dryn < DRYMAX; i++, dryn++)
        report(label, "would pull uid %u", news[i].uid);
      for (i = 0; i < arrlen(pushi) && dryn < DRYMAX; i++, dryn++)
        report(label, "would push %s", box.msgs[pushi[i]].name);
      for (i = 0; i < arrlen(facts) && dryn < DRYMAX; i++, dryn++)
        report(label, "would store flags on remote uid %u", facts[i].uid);
      for (i = 0; i < arrlen(nacts) && dryn < DRYMAX; i++, dryn++) {
        char fl[8];
        flagletters(nacts[i].flags, fl);
        report(label, "would set flags [%s] on %s", fl,
               box.msgs[nacts[i].idx].name);
      }
      for (i = 0; i < arrlen(ndel) && dryn < DRYMAX; i++, dryn++)
        report(label, "would delete %s", box.msgs[ndel[i]].name);
      for (i = 0; i < arrlen(expu) && dryn < DRYMAX; i++, dryn++)
        report(label, "would expunge remote uid %u", expu[i]);
      if (acts > dryn)
        report(label, "... and %ld more actions", acts - dryn);
    }

    if (mode == MSync && (acts || stchanged)) {
      uint32_t nextuid = uvlast;
      long batch = 0;
      /* reserve near uids up front so a crash can never reuse one */
      if (arrlen(news) + arrlen(pushi) > 0) {
        uvlast += (uint32_t)(arrlen(news) + arrlen(pushi));
        if (uvwrite(boxdir, uvval, uvlast, err, sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
      }
      for (i = 0; i < arrlen(news); i++) { /* pull */
        char tmp[4160];
        FILE *f;
        unsigned pf;
        Pair np;
        mdtmp(tmp, sizeof tmp, boxdir);
        if (!(f = fopen(tmp, "w"))) {
          report(label, "error: cannot create %s", tmp);
          rc = 2;
          goto finish;
        }
        if (imapfetchbody(im, news[i].uid, f, &pf, err, sizeof err) < 0) {
          fclose(f);
          unlink(tmp);
          report(label, "error: pull uid %u: %s", news[i].uid, err);
          rc = 2;
          goto finish;
        }
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        if (pf & FDeleted) { /* became a ghost while we looked */
          unlink(tmp);
          ghostsnow++;
          continue;
        }
        nextuid++;
        if (mdplace(boxdir, nextuid, pf, tmp, err, sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
        np.fuid = news[i].uid;
        np.nuid = nextuid;
        np.flags = pf & SYNCFLAGS;
        np.dead = 0;
        arrput(st.pairs, np);
        if (news[i].uid > st.maxpulled)
          st.maxpulled = news[i].uid;
        if (nextuid > st.maxpushed)
          st.maxpushed = nextuid;
        mutated = 1;
        npulled++;
        if (++batch % BATCH == 0 &&
            statewrite(boxdir, &st, err, sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
      }
      for (i = 0; i < arrlen(pushi); i++) { /* push */
        Local *m = &box.msgs[pushi[i]];
        char path[4160];
        FILE *f;
        uint32_t newfuid = 0;
        Pair np;
        snprintf(path, sizeof path, "%s/%s/%s", boxdir,
                 m->indir ? "new" : "cur", m->name);
        if (!(f = fopen(path, "r"))) {
          report(label, "error: cannot read %s", m->name);
          rc = 2;
          goto finish;
        }
        if (imapappendfile(im, qfar, m->flags & SYNCFLAGS, f, err, sizeof err,
                           &newfuid) < 0) {
          fclose(f);
          report(label, "error: push %s: %s", m->name, err);
          rc = 2;
          goto finish;
        }
        fclose(f);
        nextuid++;
        if (mdassignuid(boxdir, m, nextuid, err, sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
        np.fuid = newfuid;
        np.nuid = nextuid;
        np.flags = m->flags & SYNCFLAGS;
        np.dead = 0;
        arrput(st.pairs, np);
        if (newfuid > st.maxpulled)
          st.maxpulled = newfuid;
        if (nextuid > st.maxpushed)
          st.maxpushed = nextuid;
        mutated = 1;
        npushed++;
        if (++batch % BATCH == 0 &&
            statewrite(boxdir, &st, err, sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
      }
      if (arrlen(facts)) {
        if (storeflags(im, facts, 0, err, sizeof err) < 0 ||
            storeflags(im, facts, 1, err, sizeof err) < 0) {
          report(label, "error: STORE: %s", err);
          rc = 2;
          goto finish;
        }
        mutated = 1;
      }
      for (i = 0; i < arrlen(nacts); i++) {
        if (mdsetflags(boxdir, &box.msgs[nacts[i].idx], nacts[i].flags, err,
                       sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
        mutated = 1;
      }
      for (i = 0; i < arrlen(ndel); i++) {
        if (mddelete(boxdir, &box.msgs[ndel[i]], err, sizeof err) < 0) {
          report(label, "error: %s", err);
          rc = 2;
          goto finish;
        }
        mutated = 1;
      }
      if (arrlen(expu)) {
        long k = 0;
        char list[720];
        while (k < arrlen(expu)) {
          if (!uidstr(list, sizeof list, expu, arrlen(expu), &k))
            break;
          if (imapexec(im, NULL, NULL, err, sizeof err, "UID EXPUNGE %s",
                       list) < 0) {
            report(label, "error: EXPUNGE: %s", err);
            rc = 2;
            goto finish;
          }
        }
        ghostsnow += arrlen(expu); /* Gmail keeps them; self-corrects if not */
        mutated = 1;
      }
    finish:
      if ((mutated || stchanged) &&
          statewrite(boxdir, &st, err, sizeof err) < 0) {
        report(label, "error: %s", err);
        rc = 2;
      }
    }

    /* compose the per-folder report */
    if (mode == MSync && (acts || stchanged) && rc == 0) {
      if (npulled)
        addf(d, sizeof d, "pulled %ld", npulled);
      if (npushed)
        addf(d, sizeof d, "pushed %ld", npushed);
      if (arrlen(facts))
        addf(d, sizeof d, "%ld flag updates remote", (long)arrlen(facts));
      if (arrlen(nacts))
        addf(d, sizeof d, "%ld flag updates local", (long)arrlen(nacts));
      if (arrlen(ndel))
        addf(d, sizeof d, "deleted %ld local", (long)arrlen(ndel));
      if (arrlen(expu))
        addf(d, sizeof d, "expunged %ld remote", (long)arrlen(expu));
      if (vgone && !arrlen(ndel))
        addf(d, sizeof d, "recorded %ld gone remote", vgone);
      if (lgone && !arrlen(expu))
        addf(d, sizeof d, "recorded %ld gone local", lgone);
    } else if (rc == 0) {
      if (arrlen(news))
        addf(d, sizeof d, "%ld to pull", (long)arrlen(news));
      if (arrlen(pushi))
        addf(d, sizeof d, "%ld to push", (long)arrlen(pushi));
      if (arrlen(facts))
        addf(d, sizeof d, "%ld flag updates for remote", (long)arrlen(facts));
      if (arrlen(nacts))
        addf(d, sizeof d, "%ld flag updates for local", (long)arrlen(nacts));
      if (vgone)
        addf(d, sizeof d, "%ld gone remote", vgone);
      if (lgone)
        addf(d, sizeof d, "%ld gone local", lgone);
    }
    if (untracked)
      addf(d, sizeof d, "%ld local untracked (skipped)", untracked);
    if (remuntracked)
      addf(d, sizeof d, "%ld remote untracked (skipped)", remuntracked);
    if (tdrift)
      addf(d, sizeof d, "%ld trashed, kept (no expunge here)", tdrift);
    if (!st.present)
      addf(d, sizeof d, "no sync state");
    if (journal)
      addf(d, sizeof d, "mbsync journal present");
    if (!d[0])
      snprintf(d, sizeof d, cache.valid ? "in sync" : "in sync (verified)");
    if (ghostsnow)
      addf(d, sizeof d, "%ld ghost-deleted remote", ghostsnow);
    report(label, "remote %6u  local %6td  %s", sel.exists,
           arrlen(box.msgs), d);

    if (rc == 0 && (acts || vgone || lgone) && mode != MSync)
      rc = 1;
    /* record the baseline when disk provably matches the server view we
     * selected, or right after a successful sync. The sync case keeps the
     * PRE-sync counters: our own writes moved them, so the next run cannot
     * false-hit the fast path and instead re-verifies with a cheap
     * CHANGEDSINCE delta (which finds our applied changes already in the
     * state) - never with a full listing. A baseline can only advance past
     * drift that has been applied, so nothing gets masked. */
    if (rc == 0 && (mode == MSync ||
                    (acts == 0 && !stchanged && untracked == 0 &&
                     remuntracked == 0))) {
      ncache.valid = 1;
      ncache.uidval = sel.uidvalidity;
      ncache.uidnext = sel.uidnext;
      ncache.exists = sel.exists;
      ncache.modseq = sel.modseq;
      ncache.ghosts = ghostsnow;
      ncache.nlocal = arrlen(box.msgs) + npulled - arrlen(ndel);
      statmtimes(boxdir, &ncache);
      cachesave(boxdir, &ncache);
    }
  }

out:
  hmfree(farflags);
  hmfree(vanished);
  hmfree(stfarset);
  hmfree(stnearset);
  hmfree(locbyn);
  arrfree(news);
  arrfree(facts);
  arrfree(nacts);
  arrfree(ndel);
  arrfree(pushi);
  arrfree(expu);
  if (scanned)
    boxfree(&box);
  statefree(&st);
  close(lfd); /* releases the lock */
  return rc;
}

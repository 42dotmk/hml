#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stb_ds.h>

#include "hml.h"

unsigned letterflags(const char *s) {
  unsigned f = 0;

  for (; *s; s++) {
    switch (*s) {
    case 'S': f |= FSeen; break;
    case 'R': f |= FAnswered; break;
    case 'F': f |= FFlagged; break;
    case 'T': f |= FDeleted; break;
    case 'D': f |= FDraft; break;
    case 'P': f |= FPassed; break;
    }
  }
  return f;
}

/* mbsync writes flag letters in alphabetical order */
void flagletters(unsigned f, char *out) {
  int i = 0;

  if (f & FDraft)
    out[i++] = 'D';
  if (f & FFlagged)
    out[i++] = 'F';
  if (f & FPassed)
    out[i++] = 'P';
  if (f & FAnswered)
    out[i++] = 'R';
  if (f & FSeen)
    out[i++] = 'S';
  if (f & FDeleted)
    out[i++] = 'T';
  out[i] = '\0';
}

/* .mbsyncstate: "Key Value" header lines, blank line, then one
 * "faruid nearuid flags" entry per paired message */
int stateload(const char *boxdir, State *st, char *err, size_t errlen) {
  char path[4160], line[256], flags[64];
  FILE *fp;
  int inheader = 1;

  memset(st, 0, sizeof *st);
  snprintf(path, sizeof path, "%s/.mbsyncstate", boxdir);
  if (!(fp = fopen(path, "r")))
    return 0; /* never synced; empty state */
  st->present = 1;

  while (fgets(line, sizeof line, fp)) {
    if (inheader) {
      uint32_t v;
      if (line[0] == '\n' || line[0] == '\r') {
        inheader = 0;
      } else if (sscanf(line, "FarUidValidity %u", &v) == 1) {
        st->fuidval = v;
      } else if (sscanf(line, "NearUidValidity %u", &v) == 1) {
        st->nuidval = v;
      } else if (sscanf(line, "MaxPulledUid %u", &v) == 1) {
        st->maxpulled = v;
      } else if (sscanf(line, "MaxPushedUid %u", &v) == 1) {
        st->maxpushed = v;
      } /* other keys (MaxExpiredFarUid, ...) are irrelevant here */
      continue;
    }
    Pair p = {0, 0, 0, 0};
    int n = sscanf(line, "%u %u %63s", &p.fuid, &p.nuid, flags);
    if (n < 2)
      continue;
    if (n == 3)
      p.flags = letterflags(flags);
    arrput(st->pairs, p);
  }
  if (ferror(fp)) {
    snprintf(err, errlen, "read error on %s", path);
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

/* atomic replace: write a temp file, fsync, rename over the original, so a
 * crash can never leave a truncated state for mbsync or hml to trip on */
static int replacefile(const char *tmp, const char *path, FILE *f, char *err,
                       size_t errlen) {
  if (fflush(f) || fsync(fileno(f)) < 0 || ferror(f)) {
    snprintf(err, errlen, "write error on %s: %s", tmp, strerror(errno));
    fclose(f);
    unlink(tmp);
    return -1;
  }
  fclose(f);
  if (rename(tmp, path) < 0) {
    snprintf(err, errlen, "rename %s: %s", path, strerror(errno));
    unlink(tmp);
    return -1;
  }
  return 0;
}

int statewrite(const char *boxdir, const State *st, char *err, size_t errlen) {
  char tmp[4160], path[4160], fl[8];
  FILE *f;
  ptrdiff_t i;

  snprintf(tmp, sizeof tmp, "%s/.mbsyncstate.hml", boxdir);
  snprintf(path, sizeof path, "%s/.mbsyncstate", boxdir);
  if (!(f = fopen(tmp, "w"))) {
    snprintf(err, errlen, "cannot create %s: %s", tmp, strerror(errno));
    return -1;
  }
  fprintf(f,
          "FarUidValidity %u\nNearUidValidity %u\nMaxPulledUid %u\n"
          "MaxPushedUid %u\n\n",
          st->fuidval, st->nuidval, st->maxpulled, st->maxpushed);
  for (i = 0; i < arrlen(st->pairs); i++) {
    const Pair *p = &st->pairs[i];
    if (p->dead)
      continue;
    flagletters(p->flags, fl);
    fprintf(f, "%u %u %s\n", p->fuid, p->nuid, fl);
  }
  return replacefile(tmp, path, f, err, errlen);
}

int uvload(const char *boxdir, uint32_t *uidval, uint32_t *lastuid, char *err,
           size_t errlen) {
  char path[4160];
  FILE *f;

  *uidval = *lastuid = 0;
  snprintf(path, sizeof path, "%s/.uidvalidity", boxdir);
  if (!(f = fopen(path, "r")))
    return 0; /* absent: never synced */
  if (fscanf(f, "%u %u", uidval, lastuid) != 2) {
    snprintf(err, errlen, "cannot parse %s", path);
    fclose(f);
    return -1;
  }
  fclose(f);
  return 1;
}

int uvwrite(const char *boxdir, uint32_t uidval, uint32_t lastuid, char *err,
            size_t errlen) {
  char tmp[4160], path[4160];
  FILE *f;

  snprintf(tmp, sizeof tmp, "%s/.uidvalidity.hml", boxdir);
  snprintf(path, sizeof path, "%s/.uidvalidity", boxdir);
  if (!(f = fopen(tmp, "w"))) {
    snprintf(err, errlen, "cannot create %s: %s", tmp, strerror(errno));
    return -1;
  }
  fprintf(f, "%u\n%u\n", uidval, lastuid);
  return replacefile(tmp, path, f, err, errlen);
}

void statefree(State *st) { arrfree(st->pairs); }

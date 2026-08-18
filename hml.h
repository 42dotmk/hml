/* hml - hackable mail: mbsync-compatible IMAP/Maildir synchronizer */
#ifndef HML_H
#define HML_H

#ifndef HML_VERSION
#define HML_VERSION "dev"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* message flags, shared bitmask for IMAP, maildir info and sync state;
 * FPassed is maildir P / IMAP $Forwarded, which mbsync tracks too */
enum {
  FSeen = 1,
  FAnswered = 2,
  FFlagged = 4,
  FDeleted = 8,
  FDraft = 16,
  FPassed = 32,
};

/* run modes */
enum { MStatus, MDry, MSync };

typedef struct {
  const char *far;  /* IMAP mailbox name */
  const char *near; /* subdirectory of the account maildir */
  int expunge;      /* propagate deletions and expunge (mbsync "Expunge") */
} Channel;

typedef struct {
  const char *name;     /* short id, used in reports and as arg filter */
  const char *host;     /* IMAP */
  int port;
  const char *smtphost; /* SMTP submission */
  int smtpport;         /* 465 = implicit TLS, same transport as IMAP */
  const char *user;
  const char *passcmd;  /* shell command that prints the password */
  const char *maildir;  /* account maildir root, leading ~ is expanded */
  const Channel *channels;
  int nchannels;
} Account;

/* config.c */
extern const Account accounts[];
extern const int naccounts;

/* state.c - mbsync's on-disk sync state (.mbsyncstate), kept compatible so
 * mbsync and hml can be used interchangeably on the same store */
typedef struct {
  uint32_t fuid, nuid; /* far (server) / near (local) uid; 0 = side gone */
  unsigned flags;
  int dead;            /* pair dropped; skipped when writing */
} Pair;

typedef struct {
  int present;               /* .mbsyncstate existed */
  uint32_t fuidval, nuidval; /* FarUidValidity / NearUidValidity */
  uint32_t maxpulled, maxpushed;
  Pair *pairs;               /* stb_ds array */
} State;

int stateload(const char *boxdir, State *st, char *err, size_t errlen);
int statewrite(const char *boxdir, const State *st, char *err, size_t errlen);
void statefree(State *st);
unsigned letterflags(const char *s);   /* "RS", "PS", ... -> bitmask */
void flagletters(unsigned f, char *out); /* bitmask -> "DFPRST" subset, >=8b */

/* .uidvalidity: near-side uid validity + last assigned near uid */
int uvload(const char *boxdir, uint32_t *uidval, uint32_t *lastuid, char *err,
           size_t errlen);
int uvwrite(const char *boxdir, uint32_t uidval, uint32_t lastuid, char *err,
            size_t errlen);

/* maildir.c */
typedef struct {
  uint32_t uid;  /* from ,U= in the filename; 0 = locally new */
  unsigned flags;
  char *name;    /* filename */
  int indir;     /* 0 = cur, 1 = new */
} Local;

typedef struct {
  Local *msgs; /* stb_ds array */
  int nouid;   /* messages without a ,U= marker */
} Box;

int boxscan(const char *boxdir, Box *box, char *err, size_t errlen);
void boxfree(Box *box);
int mdensure(const char *boxdir, char *err, size_t errlen); /* mkdir -p + cur/new/tmp */
int mdtmp(char *dst, size_t cap, const char *boxdir);
int mdplace(const char *boxdir, uint32_t nuid, unsigned flags,
            const char *tmppath, char *err, size_t errlen);
int mdsetflags(const char *boxdir, const Local *m, unsigned flags, char *err,
               size_t errlen);
int mdassignuid(const char *boxdir, const Local *m, uint32_t nuid, char *err,
                size_t errlen);
int mddelete(const char *boxdir, const Local *m, char *err, size_t errlen);

/* imap.c */
typedef struct Imap Imap;
typedef void (*Linefn)(const char *line, void *ud);

Imap *tlsconnect(const char *host, int port, char *err,
                 size_t errlen); /* bare TLS conn, no greeting expected */
char *imapline(Imap *im, char *err, size_t errlen); /* one logical line */
int imapwrite(Imap *im, const char *s, size_t n);
Imap *imapconnect(const char *host, int port, char *err, size_t errlen);
int imaplogin(Imap *im, const char *user, const char *pass, char *err,
              size_t errlen);
/* send one command, feed every untagged reply line to fn, 0 on tagged OK */
int imapexec(Imap *im, Linefn fn, void *ud, char *err, size_t errlen,
             const char *fmt, ...);
/* download one message body into out (CRLF converted to LF) */
int imapfetchbody(Imap *im, uint32_t uid, FILE *out, unsigned *flags,
                  char *err, size_t errlen);
/* upload src (LF converted to CRLF); returns the new uid via APPENDUID */
int imapappendfile(Imap *im, const char *qbox, unsigned flags, FILE *src,
                   char *err, size_t errlen, uint32_t *uid);
void imapclose(Imap *im);
void imapquote(char *dst, size_t cap, const char *s);
unsigned imapflags(const char *s);            /* "(\Seen ...)" -> bitmask */
void imapflagstr(unsigned f, char *out, size_t cap); /* -> "\Seen \Deleted" */

/* sync.c - the engine behind all three modes */
int syncbox(Imap *im, const Account *a, const Channel *ch, int mode,
            int force);

/* send.c - SMTP submission ("hml send", sendmail-compatible) */
int sendmain(int argc, char **argv);

/* hml.c */
void report(const char *label, const char *fmt, ...);
char *runpasscmd(const char *cmd, char *err, size_t errlen);

#endif

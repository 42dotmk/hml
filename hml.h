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
    const char *name; /* short id, used in reports and as arg filter */
    const char *host; /* IMAP */
    int port;
    const char *smtphost; /* SMTP submission */
    int smtpport;         /* 465 = implicit TLS, same transport as IMAP */
    const char *user;
    const char *passcmd; /* shell command that prints the password */
    const char *maildir; /* account maildir root, leading ~ is expanded */
    const Channel *channels;
    int nchannels;
} Account;

typedef struct {
    const char *folder; /* channel near name, e.g. "Sent" */
    const char *tag;    /* tag every message with a file in it gets */
} FolderTag;

typedef struct {
    const char *tags;  /* "+tag -tag ..." */
    const char *query; /* applied by `hml new` to newly indexed matches */
} TagRule;

/* config.h */
extern const Account accounts[];
extern const int naccounts;
extern const char *postrecv; /* shell hook after `hml recv`, "" = none */
extern const char *postsend; /* shell hook after `hml send`, "" = none */
extern const char *mailroot; /* the index lives at <mailroot>/.hml.db */
extern const FolderTag foldertags[];
extern const int nfoldertags;
extern const TagRule tagrules[];
extern const int ntagrules;

/* state.c - mbsync's on-disk sync state (.mbsyncstate), kept compatible so
 * mbsync and hml can be used interchangeably on the same store */
typedef struct {
    uint32_t fuid, nuid; /* far (server) / near (local) uid; 0 = side gone */
    unsigned flags;
    int dead; /* pair dropped; skipped when writing */
} Pair;

typedef struct {
    int present;               /* .mbsyncstate existed */
    uint32_t fuidval, nuidval; /* FarUidValidity / NearUidValidity */
    uint32_t maxpulled, maxpushed;
    Pair *pairs; /* stb_ds array */
} State;

int stateload(const char *boxdir, State *st, char *err, size_t errlen);
int statewrite(const char *boxdir, const State *st, char *err, size_t errlen);
void statefree(State *st);
unsigned letterflags(const char *s);     /* "RS", "PS", ... -> bitmask */
void flagletters(unsigned f, char *out); /* bitmask -> "DFPRST" subset, >=8b */

/* .uidvalidity: near-side uid validity + last assigned near uid */
int uvload(const char *boxdir, uint32_t *uidval, uint32_t *lastuid, char *err,
           size_t errlen);
int uvwrite(const char *boxdir, uint32_t uidval, uint32_t lastuid, char *err,
            size_t errlen);

/* maildir.c */
typedef struct {
    uint32_t uid; /* from ,U= in the filename; 0 = locally new */
    unsigned flags;
    char *name; /* filename */
    int indir;  /* 0 = cur, 1 = new */
} Local;

typedef struct {
    Local *msgs; /* stb_ds array */
    int nouid;   /* messages without a ,U= marker */
} Box;

int boxscan(const char *boxdir, Box *box, char *err, size_t errlen);
void boxfree(Box *box);
int mdensure(const char *boxdir, char *err,
             size_t errlen); /* mkdir -p + cur/new/tmp */
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
int imapfetchbody(Imap *im, uint32_t uid, FILE *out, unsigned *flags, char *err,
                  size_t errlen);
/* upload src (LF converted to CRLF); returns the new uid via APPENDUID */
int imapappendfile(Imap *im, const char *qbox, unsigned flags, FILE *src,
                   char *err, size_t errlen, uint32_t *uid);
void imapclose(Imap *im);
void imapquote(char *dst, size_t cap, const char *s);
unsigned imapflags(const char *s); /* "(\Seen ...)" -> bitmask */
void imapflagstr(unsigned f, char *out, size_t cap); /* -> "\Seen \Deleted" */

/* sync.c - the engine behind all three modes */
int syncbox(Imap *im, const Account *a, const Channel *ch, int mode, int force);

/* send.c - SMTP submission ("hml send", sendmail-compatible) */
int sendmain(int argc, char **argv);

/* mime.c - the searchable text of one RFC 822 message, and the header/
 * MIME primitives show.c builds on */
typedef struct {
    const char *name;
    size_t nlen;
    const char *val; /* raw: still folded, leading space included */
    size_t vlen;
} Hdr;

/* collect the header block of an entity; returns the body offset */
size_t mimehdrs(const char *s, size_t n, Hdr **out);
/* first header of that name, unfolded and trimmed, malloc'd; NULL if absent */
char *mimehget(Hdr *h, const char *name);
char *mimedecode(const char *v); /* RFC 2047 words -> UTF-8, malloc'd */
void mimetype(const char *ct, char *out, size_t cap); /* "type/sub" lc */
char *mimeparam(const char *v, const char *name);    /* ;name= value */
char *mimecte(const char *cte, const char *s, size_t n); /* stb array */
void mimeutf8(char **out, const char *cs, const char *in, size_t n);
void mimehtmltext(char **out, const char *s, size_t n);
long mimedate(const char *s); /* RFC 5322 date -> epoch, 0 if hopeless */

typedef struct {
    char *mid;     /* Message-ID without brackets; synthesized if absent */
    char *subject; /* decoded to UTF-8 */
    char *from;
    char *to;     /* To, Cc and Bcc */
    char *attach; /* attachment file names, space separated */
    char *body;   /* text of every text part, HTML stripped, capped */
    char **refs;  /* stb_ds array: In-Reply-To + References, unique */
    long date;    /* epoch, 0 if unparsable */
    int hasatt;   /* carries a real attachment: a part with disposition
                     attachment, or a named non-text part that is not a
                     Content-ID image referenced from the HTML */
} Mail;

int mailparse(const char *buf, size_t n, Mail *m);
void mailfree(Mail *m);

/* index.c - the search index ("hml new") and user tags ("hml tag") */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
sqlite3 *dbopen(char *err, size_t errlen);
int newmain(int argc, char **argv);
int tagmain(int argc, char **argv);

/* query.c - notmuch-style query -> SQL; "hml search", "hml count",
 * "hml tags" */
typedef struct {
    char *sql;     /* stb char array: a WHERE expression over msg */
    char **params; /* stb array of bound strings, in ? order */
    char *err;
} Query;

int querycompile(const char *q, Query *c, char **err);
/* prepare fmt with the expression spliced in at "%s", params bound */
sqlite3_stmt *queryprep(sqlite3 *db, const char *fmt, const Query *c,
                        const char *tail, char **err);
void queryfree(Query *c);
int searchmain(int argc, char **argv);
int countmain(int argc, char **argv);
int tagsmain(int argc, char **argv);
/* box "acct/Sub" + sub + name -> absolute path; 0 if the account is gone */
int filepath(const char *box, const char *sub, const char *name, char *out,
             size_t cap);
void dispname(const char *from, char *out, size_t cap);
void reldate(long t, char *out, size_t cap);

/* show.c - "hml show" (notmuch text format / raw / --part) and
 * "hml reply" (a reply template for the newest matching message) */
int showmain(int argc, char **argv);
int replymain(int argc, char **argv);

/* hml.c */
void report(const char *label, const char *fmt, ...);
char *runpasscmd(const char *cmd, char *err, size_t errlen);
void expand(const char *path, char *dst, size_t cap); /* leading ~ */

#endif

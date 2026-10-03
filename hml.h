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

typedef struct {
    const char *query; /* selects mail from outside among what `hml new`
                          indexes; keep a from: term in it */
    const char *local; /* the bus address it is delivered to */
} Route;

/* config.h */
extern const Account accounts[];
extern const int naccounts;
extern const char *postrecv;    /* shell hook after `hml recv`, "" = none */
extern const char *postsend;    /* shell hook after `hml send`, "" = none */
extern const char *mailroot;    /* the index lives at <mailroot>/.hml.db */
extern const char *localdomain; /* mail @here never leaves the machine */
extern const char *localbox;    /* ...it lands in <localbox>/<localpart>/ */
extern const FolderTag foldertags[];
extern const int nfoldertags;
extern const TagRule tagrules[];
extern const int ntagrules;
extern const Route routes[];
extern const int nroutes;
extern const char *gateway; /* the bus address the outside talks to (the
                               user's); "" turns the gateway off */

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

int sync_state_load(const char *boxdir, State *st, char *err, size_t errlen);
int sync_state_write(const char *boxdir, const State *st, char *err, size_t errlen);
void sync_state_free(State *st);
unsigned flags_from_letters(const char *s);   /* "RS", "PS", ... -> bitmask */
void flags_to_letters(unsigned f, char *out); /* bitmask -> "DFPRST" subset, >=8b */

/* .uidvalidity: near-side uid validity + last assigned near uid */
int uidvalidity_load(const char *boxdir, uint32_t *uidval, uint32_t *lastuid, char *err, size_t errlen);
int uidvalidity_write(const char *boxdir, uint32_t uidval, uint32_t lastuid, char *err, size_t errlen);

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

int maildir_scan(const char *boxdir, Box *box, char *err, size_t errlen);
void maildir_scan_free(Box *box);
int maildir_create(const char *boxdir, char *err, size_t errlen); /* mkdir -p + cur/new/tmp */
int maildir_temp_path(char *dst, size_t cap, const char *boxdir);
int maildir_store(const char *boxdir, uint32_t nuid, unsigned flags, const char *tmppath, char *err, size_t errlen);
int maildir_set_flags(const char *boxdir, const Local *m, unsigned flags, char *err, size_t errlen);
int maildir_assign_uid(const char *boxdir, const Local *m, uint32_t nuid, char *err, size_t errlen);
int maildir_delete(const char *boxdir, const Local *m, char *err, size_t errlen);
/* a locally delivered message: tmp/ -> new/, no uid, no flags */
int maildir_deliver(const char *boxdir, const char *tmppath, char *err, size_t errlen);

/* imap.c */
typedef struct Imap Imap;
typedef void (*Linefn)(const char *line, void *ud);

Imap *tls_connect(const char *host, int port, char *err, size_t errlen); /* bare TLS conn, no greeting expected */
char *imap_read_line(Imap *im, char *err, size_t errlen);                /* one logical line */
int imap_write(Imap *im, const char *s, size_t n);
Imap *imap_connect(const char *host, int port, char *err, size_t errlen);
int imap_login(Imap *im, const char *user, const char *pass, char *err, size_t errlen);
/* send one command, feed every untagged reply line to fn, 0 on tagged OK */
int imap_execute(Imap *im, Linefn fn, void *ud, char *err, size_t errlen, const char *fmt, ...);
/* download one message body into out (CRLF converted to LF) */
int imap_fetch_body(Imap *im, uint32_t uid, FILE *out, unsigned *flags, char *err, size_t errlen);
/* upload src (LF converted to CRLF); returns the new uid via APPENDUID */
int imap_append_file(Imap *im, const char *qbox, unsigned flags, FILE *src, char *err, size_t errlen, uint32_t *uid);
void imap_close(Imap *im);
void imap_quote(char *dst, size_t cap, const char *s);
unsigned imap_parse_flags(const char *s);                  /* "(\Seen ...)" -> bitmask */
void imap_format_flags(unsigned f, char *out, size_t cap); /* -> "\Seen \Deleted" */

/* sync.c - the engine behind all three modes */
int sync_box(Imap *im, const Account *a, const Channel *ch, int mode, int force);

/* send.c - SMTP submission ("hml send", sendmail-compatible) */
int send_main(int argc, char **argv);
/* one SMTP session through the account: the message as given (Bcc
 * already stripped), CRLF and dot-stuffing added on the wire */
int smtp_submit(const Account *a, const char *envfrom, char **rcpts, const char *msg, size_t n, char *err, size_t errlen);

/* mime.c - the searchable text of one RFC 822 message, and the header/
 * MIME primitives show.c builds on */
typedef struct {
    const char *name;
    size_t nlen;
    const char *val; /* raw: still folded, leading space included */
    size_t vlen;
} Hdr;

/* collect the header block of an entity; returns the body offset */
size_t mime_parse_headers(const char *s, size_t n, Hdr **out);
/* first header of that name, unfolded and trimmed, malloc'd; NULL if absent */
char *mime_header_get(Hdr *h, const char *name);
char *mime_decode_header(const char *v);                                       /* RFC 2047 words -> UTF-8, malloc'd */
void mime_media_type(const char *ct, char *out, size_t cap);                   /* "type/sub" lc */
char *mime_parameter(const char *v, const char *name);                         /* ;name= value */
char *mime_decode_transfer_encoding(const char *cte, const char *s, size_t n); /* stb array */
void mime_convert_to_utf8(char **out, const char *cs, const char *in, size_t n);
void mime_html_to_text(char **out, const char *s, size_t n);
long mime_parse_date(const char *s); /* RFC 5322 date -> epoch, 0 if hopeless */
/* an address header split on the commas between mailboxes (not those in
 * quotes or <>), each trimmed: stb array of malloc'd strings */
void mime_split_addresses(const char *v, char ***out);
void mime_bare_address(const char *m, char *out, size_t cap); /* bare, lowercased */

typedef struct {
    char *mid;     /* Message-ID without brackets; synthesized if absent */
    char *subject; /* decoded to UTF-8 */
    char *from;
    char *to;     /* To, Cc and Bcc */
    char *attach; /* attachment file names, space separated */
    char *body;   /* text of every text part, HTML stripped, capped */
    char **refs;  /* stb_ds array: In-Reply-To + References, unique */
    long date;    /* epoch, 0 if unparsable */
    char *intent; /* Hai-Intent, "" when absent: a hai session message */
    int hasatt;   /* carries a real attachment: a part with disposition
                     attachment, or a named non-text part that is not a
                     Content-ID image referenced from the HTML */
} Mail;

int mail_parse(const char *buf, size_t n, Mail *m);
void mail_free(Mail *m);
/* the reader's text: the first text/plain leaf decoded to UTF-8, else
 * the first text/html stripped to text; malloc'd, "" when neither */
char *mime_plain_text(const char *s, size_t n);

/* index.c - the search index ("hml new") and user tags ("hml tag") */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
sqlite3 *db_open(char *err, size_t errlen);
int new_main(int argc, char **argv);
int tag_main(int argc, char **argv);

/* gateway.c - the bus and the outside: routes in `hml new`, the
 * outbound copy in `hml send`, the crossing log <mailroot>/.hroutes */
void gateway_inbound(sqlite3 *db); /* after the rules, before COMMIT */
/* a message to the gateway address: sent on to the outside party it
 * answers; 1 sent, 0 not for outside, -1 failed (err) */
int gateway_outbound(const char *msg, size_t n, char *err, size_t errlen);
int gateway_log_add(const char *mid, const char *local, const char *remote, const char *account, const char *intent);
char *gateway_message_id(const char *v); /* the id inside <...>, malloc'd */

/* query.c - notmuch-style query -> SQL; "hml search", "hml count",
 * "hml tags" */
typedef struct {
    char *sql;     /* stb char array: a WHERE expression over msg */
    char **params; /* stb array of bound strings, in ? order */
    char *err;
} Query;

int query_compile(const char *q, Query *c, char **err);
/* prepare fmt with the expression spliced in at "%s", params bound */
sqlite3_stmt *query_prepare(sqlite3 *db, const char *fmt, const Query *c, const char *tail, char **err);
void query_free(Query *c);
int search_main(int argc, char **argv);
int count_main(int argc, char **argv);
int tags_main(int argc, char **argv);
int address_main(int argc, char **argv);
/* box "acct/Sub" + sub + name -> absolute path; 0 if the account is gone */
int file_path(const char *box, const char *sub, const char *name, char *out, size_t cap);
void display_name(const char *from, char *out, size_t cap);
void relative_date(long t, char *out, size_t cap);

/* show.c - "hml show" (notmuch text format / raw / --part) and
 * "hml reply" (a reply template for the newest matching message) */
int show_main(int argc, char **argv);
int reply_main(int argc, char **argv);

/* hml.c */
void report(const char *label, const char *fmt, ...);
char *run_password_command(const char *cmd, char *err, size_t errlen);
void expand_home(const char *path, char *dst, size_t cap); /* leading ~ */
/* the maildir directory of a box the index names "acct/Sub" or
 * "<localdomain>/<name>"; 0 when no configured account owns it */
int box_directory(const char *box, char *out, size_t cap);

#endif

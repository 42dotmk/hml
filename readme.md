# hml

Hackable mail: an IMAP/Maildir synchronizer, SMTP sender and search
index in ~4,700 lines of C11. It reads and writes [mbsync](https://isync.sourceforge.io/)'s
own on-disk state, so it is a **drop-in replacement you can adopt — and
abandon — at any time**, on the same maildir, with zero migration and no
re-downloading.

```
$ hml recv
cc/All       remote 120363  local 117137  in sync (fast)
cc/Drafts    remote      2  local      2  in sync (fast)
cc/Sent      remote   5840  local   5840  in sync (fast)
cc/Trash     remote     35  local    139  in sync (fast)
km/All       remote  43777  local  43777  pulled 1
...
2.73s
```

## Why

mbsync is correct, but every run re-lists every folder. On three Gmail
accounts with ~170k messages that is **minutes** per sync. hml keeps a
tiny per-folder cache (`.hmlstate`) of `UIDVALIDITY`, `UIDNEXT`,
`HIGHESTMODSEQ`, `EXISTS` and the local `cur/`/`new/` mtimes. When
nothing moved on either side, a folder is verified and skipped after a
**single SELECT round-trip**. When something did move, flag deltas come
from `CHANGEDSINCE` (CONDSTORE) and new mail from one
`UID FETCH maxpulled+1:*` — never a full listing. Accounts sync in
parallel, one thread each.

Steady state across 3 accounts / 12 folders: **~3 seconds**, most of it
TLS handshakes. The cache is always safe to delete or find stale — hml
falls back to a full verification, which is simply what mbsync does on
every run.

Speed is not bought with trust: the state file is written atomically
(tmp + fsync + rename), near-side uids are reserved on disk *before*
use so a crash can never reuse one, and anything hml cannot reconcile —
a UIDVALIDITY change, an mbsync crash journal — stops with an error.
It never guesses.

## mbsync interop, precisely

- `<box>/.mbsyncstate` and `<box>/.uidvalidity` are read and written in
  mbsync's exact format; mbsync reports zero corrections on hml-written
  state.
- Maildir filenames keep mbsync's `,U=<uid>` marker and `:2,` flags.
- hml takes the same `fcntl` lock mbsync takes, so the two can never
  run on a folder concurrently — a live mbsync just means
  "locked, skipped".
- hml's own cache lives in a separate `.hmlstate` file mbsync ignores.

Run mbsync on Monday, hml on Tuesday, mbsync again on Wednesday. Both
sides agree.

## Commands

```
hml              read-only status report (safe to run anytime)
hml recv         sync: pull/push mail, flags and deletions
hml recv -n      dry run: list exactly what recv would do
hml recv cc km   limit to named accounts
hml send         SMTP submission, sendmail-compatible (see below)
hml new          update the search index from the maildirs
hml search       query it, notmuch-style (see below)
hml count        how many messages/threads/files match
hml tags         every tag, or the tags of the messages matching a query
hml -d           distrust caches, re-verify with a full listing
```

Exit codes: `0` in sync, `1` differences found (or folders skipped),
`2` error. Status and dry-run never write anything.

## Send

`hml send` speaks the sendmail interface — `-t` (recipients from
To/Cc/Bcc, with Bcc stripped before transmission), `-f` envelope
sender, reading the message on stdin — so any MUA configured for
msmtp or sendmail works by swapping one path:

```
# mutt / aerc / anything with a sendmail setting
set sendmail = "~/.local/bin/hml send"

# git
git config sendemail.sendmailcmd "hml send"

# by hand
hml send -t < message.eml
hml send -a work costa@example.com < message.eml
```

The account is picked by `-a name`, or matched from the From: header
against the configured accounts. AUTH PLAIN over implicit TLS
(port 465), dot-stuffing and Bcc handling included. With Gmail there is
no duplicate-Sent dance: the server files the sent copy into
`[Gmail]/Sent Mail` itself and the next `recv` picks it up.

## Search

`hml new` indexes every message into `<mailroot>/.hml.db` — SQLite
with FTS5, nothing else. A full index of 188k files takes ~40 seconds
on all cores and 525 MB; an unchanged store is verified in a few
milliseconds by directory mtimes, the same way notmuch does it. The
index is a cache: delete it and `hml new` rebuilds it.

Queries are notmuch's, so habits, scripts and MUAs port by swapping the
command:

```
hml search from:nikolaj                    # thread summaries, newest first
hml search --limit=20 tag:inbox and tag:unread
hml search 'subject:"retro notes" or (from:acme.com and date:7d..)'
hml search --output=messages to:contact@codechem.com     # id:<...> lines
hml search --output=files --format=json attachment:pdf
hml count 'date:2026-08 and not tag:sent'
hml tags                                   # every tag in the index
hml tags from:linkedin.com                 # tags across the matches
```

Terms: bare words (all fields), `subject:` `from:` `to:` `attachment:`
`body:`, `tag:`, `id:`, `thread:`, `path:cc/**` (an account) or
`path:cc/Sent` (one box), `date:2026-08-01..2026-08-21`, `date:7d..`,
`date:yesterday..today`, `date:2026-08`; `and`/`or`/`not`, parentheses,
implicit `and`; a trailing `*` makes a prefix match.

Tags are derived, never stored: `unread`, `flagged`, `replied`,
`draft`, `passed` from the maildir flags, `sent`/`draft`/`deleted` from
the folder (the `foldertags` table in `config.h`), `inbox` for anything
not in one of those folders — so what Gmail shows on your phone and what
`tag:inbox` returns are the same set. User tags and tagging rules are
next.

## Configuration

Suckless-style: the config is a C table compiled into the binary. Edit
`config.h`, run `make`. No runtime config files, no parser, nothing to
get out of sync with the code.

```c
/* channels: far (IMAP mailbox) -> near (maildir subdir).
 * expunge 0 = deletions are recorded, never propagated (mbsync's
 * no-Expunge semantics — right for Gmail's Trash) */
static const Channel gmail[] = {
    {"[Gmail]/All Mail",  "All",    1},
    {"[Gmail]/Drafts",    "Drafts", 1},
    {"[Gmail]/Sent Mail", "Sent",   1},
    {"[Gmail]/Trash",     "Trash",  0},
};

const Account accounts[] = {
    {"work",                        /* short name, used in reports and args */
     "imap.gmail.com", 993,         /* IMAP host, implicit TLS */
     "smtp.gmail.com", 465,         /* SMTP host, implicit TLS */
     "costa@example.com",           /* login (and From: match for send) */
     "pass show mail/work",         /* any shell command that prints the
                                       password; gpg, pass, secret-tool... */
     "~/.mail/work",                /* maildir root for this account */
     gmail, LEN(gmail)},
};
const int naccounts = LEN(accounts);

/* shell hooks; "" = do nothing */
const char *postrecv = "hml new";     /* after every `hml recv` */
const char *postsend = "";            /* after a successful `hml send` */

/* search index location and folder-derived tags */
const char *mailroot = "~/.mail";     /* index at <mailroot>/.hml.db */
const FolderTag foldertags[] = {
    {"Sent", "sent"}, {"Drafts", "draft"}, {"Trash", "deleted"},
};
```

Passwords are fetched at runtime from the `passcmd` shell command,
zeroed after login, and never appear in the source, the binary, or a
log line.

## Build

```
make            # C11, warning-free under -pedantic -Wall -Wextra
make install    # symlinks hml into ~/.local/bin — no sudo
```

Dependencies: OpenSSL, SQLite (with FTS5, as every distro build has)
and pthreads. That's it — the only vendored file is `stb_ds.h`.

## Design notes

- Blocking I/O, one thread per account, no event loop: an IMAP
  conversation is linear, so the code that speaks it is too.
- Layered small files: `imap.c` (TLS transport + line reader, also
  used for SMTP), `state.c`/`maildir.c` (on-disk formats), `sync.c`
  (the three-way diff/merge engine), `send.c`, `mime.c` (message text
  extraction), `index.c` (the index), `query.c` (the query language),
  `hml.c` (CLI).
- Gmail's quirks are handled, not fought: ghost messages that linger
  `\Deleted` in All Mail after an expunge, drafts appearing in both
  Drafts and All Mail, `STATUS`/`EXISTS` disagreements, the
  `UID n:*` echo. Developed and live-tested against three real Gmail
  accounts with mbsync cross-checks on every path; other IMAP servers
  should work (only CONDSTORE is optional-fast-path, nothing is
  Gmail-specific) but haven't seen the same mileage.

## Status & roadmap

In daily production use for the author's mail (synced every 5 minutes,
`hed`'s mail plugin on top). The search index is new: it agrees with
notmuch on the same store query for query, and is meant to replace it.
Next: `hml tag` with an append-only tag log and tagging rules in
`config.h`, `hml show`; then per-folder connection fan-out,
COMPRESS=DEFLATE, and an IDLE daemon — one long-lived connection per
account instead of hundreds of logins a day.

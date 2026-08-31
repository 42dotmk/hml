# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

`hml` — hackable mail. An IMAP/Maildir synchronizer being built to replace
mbsync (isync) for the user's Gmail accounts, with two goals mbsync doesn't
meet: steady-state syncs that skip unchanged folders entirely (CONDSTORE,
`UIDNEXT`/`HIGHESTMODSEQ` fast paths, parallel accounts) and code small enough
to fully understand.

**Interop contract — the most important invariant in this repo:** hml reads
and writes mbsync's own on-disk format so the two tools stay interchangeable
on the same store (`~/.mail`) with zero migration and no re-downloading:

- `<box>/.mbsyncstate` — 4-line header (`FarUidValidity`, `NearUidValidity`,
  `MaxPulledUid`, `MaxPushedUid`), blank line, then one `faruid nearuid flags`
  entry per paired message.
- `<box>/.uidvalidity` — near-side uidvalidity + last assigned near uid.
- Maildir filenames carry the server uid as `,U=<n>` and flags after `:2,`.
- `<box>/.mbsyncstate.lock` — hml takes the same `fcntl` write lock mbsync
  takes, so the two can never run on a folder concurrently (tested live:
  mbsync running → hml reports "locked, skipped").
- hml-only caches, when they appear, go in separate files mbsync ignores and
  must always be safe to lose or find stale.

Never break this compatibility; mbsync is the fallback until hml earns trust.
Anything hml cannot reconcile (e.g. UIDVALIDITY mismatch) must stop with an
error, never guess.

## Status

Milestone 3 (done): one binary, subcommands. `hml` = read-only status,
`hml recv` = sync (`-n` dry run), `hml send` = SMTP submission
(sendmail-compatible: `-t`, `-f`, `-a`; replaces msmtp — MUAs swap one
path), `hml search` = reserved, not yet implemented. `-d` distrusts caches
and re-verifies with a full listing. Exit code: 0 in sync, 1 differences,
2 error.

The sync engine (`sync.c`, milestone 2) drives status/recv. Every path is
live-tested against the real store with mbsync cross-checks (mbsync
reports zero corrections on hml-written state): pull, push
(APPEND/APPENDUID), flag merge both ways, gone-propagation, `UID EXPUNGE`,
and the no-Expunge trash semantics. `hml send` is live-tested too: SMTP
AUTH PLAIN over implicit TLS (port 465), dot-stuffing, Bcc stripping under
`-t`; Gmail auto-files the sent copy into `[Gmail]/Sent Mail`, where the
next `recv` picks it up — no APPEND needed.

How the speed works (steady state ~4s for 12 folders, vs minutes for
mbsync): `.hmlstate` per folder caches `UIDVALIDITY`/`UIDNEXT`/
`HIGHESTMODSEQ`/`EXISTS`, the ghost count, and cur//new/ mtimes. If all
match after SELECT, the folder is done in one round-trip ("in sync
(fast)"). Otherwise flag deltas come from `CHANGEDSINCE` and new mail from
`UID FETCH maxpulled+1:*`; the full listing happens only with no baseline.
The cache is written only when disk provably matches the selected server
view, and never right after a mutating sync (counters moved; the next run
re-verifies cheaply, then caches). Losing/staling `.hmlstate` is always
safe.

Sync-correctness rules built in: near uids are reserved in `.uidvalidity`
*before* use (crash cannot reuse one); state is rewritten atomically
(tmp+fsync+rename) every 32 pulls/pushes and at the end; `\Deleted`/T is
excluded from flag merge (mbsync treats it as deletion state, not a flag —
observably so in no-Expunge channels) and handled only by the
gone/expunge machinery; a present `.mbsyncstate.journal` (mbsync crash)
makes `-s` refuse until mbsync recovers it.

Milestone 4 (in progress): the search index, a notmuch replacement.
`hml new` indexes the maildirs into `<mailroot>/.hml.db` (SQLite +
FTS5); `hml search` / `hml count` / `hml tags` query it with notmuch's
query syntax and output shapes (`--output=summary|threads|messages|
files|tags`, `--format=json`, `--limit`, `--sort`), so the existing
notmuch skills/MUAs port by swapping the command. Verified against the
notmuch index on the same store: counts agree on every query class
(180501 vs 180500 messages, `to:`/`from:`/`date:`/`attachment:` all
match within notmuch's sticky-tag noise). Full index of 188k files:
~37s, 525 MB (notmuch: 3.1 GB). Unchanged store: 8 ms.

How the index works (index.c, mime.c, query.c):

- `msg` is one row per Message-ID (a synthesized `hml.<fnv64>` when
  absent); `file` is one row per maildir file keyed `(box, base)` where
  base is the filename before `:2,`, so flag changes and new/->cur/
  moves are renames, not re-parses. Boxes are named `<account>/<near>`
  (`cc/Sent`), which is what `path:cc/**` matches on.
- Change detection is notmuch's: per-directory mtimes in `dir`; a box
  is diffed only when `cur/` or `new/` mtime moved (a dir touched in the
  current second is left unrecorded so it is looked at again). Parsing
  runs on all cores, one writer thread does the SQLite inserts.
- The FTS5 table is contentless (`contentless_delete=1`) with columns
  subject/sender/rcpt/attach/body; `from:x` compiles to
  `sender : "x"`, bare words match every column, trailing `*` is a
  prefix match. Query terms become SQL predicates over `msg` joined by
  AND/OR/NOT, FTS terms as `id IN (SELECT rowid FROM fts WHERE fts
  MATCH ?)` subqueries.
- Threads are JWZ connectivity kept incrementally: a new message joins
  the thread of anything it references, anything referencing it, or
  anything sharing a reference (`ref` table); several become one via
  `UPDATE msg SET thread`. Thread ids are the root's row id, printed as
  16 hex digits like notmuch.
- Tags are *derived*, recomputed per message whenever one of its files
  changes: `unread`/`flagged`/`replied`/`draft`/`passed` from the union
  of its files' maildir flags, folder tags from `foldertags` in
  config.h (Sent→sent, Drafts→draft, Trash→deleted), `inbox` when no
  file is in a listed folder. User tags (`hml tag +x -y`, tag rules in
  config.h, an append-only tag log the index is rebuilt from) are the
  next step; `hml show` too.
- The index is an hml-only cache under the interop contract: delete
  `.hml.db*` and `hml new` rebuilds it. `postrecv` still runs
  `notmuch new`; switch it to `hml new` (or run both) to go live.

Next: `hml tag` + tag log + config.h tag rules, `hml show`, then
per-folder connection fan-out, COMPRESS=DEFLATE, IDLE daemon mode.

## Gmail quirks (learned the hard way, keep in mind)

- Expunging from `[Gmail]/All Mail` doesn't delete: the message lingers
  server-side flagged `\Deleted`, absent from mbsync's state but counted in
  `EXISTS` ("ghosts"; the user's cc account has 3226 accumulated since 2011).
  hml reconciles counts via `UID SEARCH DELETED` and reports them
  informationally, never as new mail.
- `STATUS` `MESSAGES` and `EXAMINE` `EXISTS` can disagree on the same folder.
  Trust `EXISTS` of the mailbox you actually examined.
- `UID SEARCH UID n:*` returns the highest-uid message even when `n` exceeds
  every uid (RFC `*` semantics) — always filter results against the range.
- Capabilities include `CONDSTORE`, `ESEARCH`, `LIST-STATUS`,
  `COMPRESS=DEFLATE`, `UIDPLUS`, `X-GM-EXT-1`; no `QRESYNC`.
- Drafts also appear in All Mail: a message appended to `[Gmail]/Drafts`
  shows up as new mail in `All`, and expunging the draft vanishes it from
  `All` too. Tested live; hml handles both directions.
- Unlike All Mail, expunging from Drafts really deletes (no ghost). hml
  assumes ghost-on-expunge and self-corrects on the next run.
- The state flag letters include `P` = maildir Passed = IMAP `$Forwarded`
  (mbsync tracks it); dropping it would corrupt flags on ~300 of the user's
  messages.
- Frequent LOGINs get tarpitted: after many sessions in a day (e.g. the
  5-minute mail-sync service x 3 accounts), Gmail delays the LOGIN reply
  ~30s while TLS/greeting stay instant. Not an hml bug; the real fix is
  the planned IDLE daemon (one long-lived connection instead of hundreds
  of logins). Gmail also drops long-lived connections mid-command now and
  then; accountmain reconnects and retries the folder once.

## Build

- `make` — must stay warning-free under `-std=c11 -pedantic -Wall -Wextra`;
  the compiler flags are the linter. `make install` symlinks into
  `~/.local/bin`. No test suite; verify by running `./hml` (read-only, safe).
- Dependencies: OpenSSL (`-lssl -lcrypto`), SQLite with FTS5
  (`-lsqlite3`, 3.43+ for `contentless_delete`), iconv (glibc),
  pthreads, vendored `vendor/stb_ds.h` (needs `-Dtypeof=__typeof__`
  under `-std=c11`).
- Index changes are verified by comparing `hml count`/`hml search`
  against `notmuch count`/`notmuch search` on the same queries, and by
  mutating a scratch copy of an account (`cp -r ~/.mail/chgm`, build
  with a config.h pointing `mailroot`/maildirs at it) and re-running
  `hml new`.
- Accounts/channels are compiled into `config.h` (suckless-style, no runtime
  config); passwords come from `PassCmd`-style shell commands (gpg + pass
  store), never live in the source or binary.

## Layout & style

- `hml.c` — main, subcommand dispatch, per-account threads.
- `sync.c` — the engine: three-way diff, merge, execution, `.hmlstate`.
- `send.c` — `hml send`: sendmail-style argv/stdin handling, header/address
  parsing, SMTP dialogue (reuses the TLS transport and line reader).
- `mime.c` — `mailparse`: the searchable text of one message (RFC 2047
  headers, charset conversion via iconv, MIME walk, base64/QP, HTML
  stripped to text, attachment names, References). No dependencies
  beyond libc; threads call it concurrently.
- `index.c` — `hml new` and the schema: maildir diff, parallel parse,
  threading, derived tags. `dbopen` is shared with query.c.
- `query.c` — `hml search`/`count`/`tags`: the query parser (recursive
  descent, notmuch precedence: not > and > or, implicit and) compiled
  to SQL with bound parameters, and the notmuch-shaped text/JSON output.
- `imap.c` — TCP+TLS transport (`tlsconnect` is protocol-neutral; SMTP uses
  it too), logical line reader (streams literals to a file sink with CRLF
  conversion), tagged commands, FETCH body, APPEND.
- `state.c` / `maildir.c` — mbsync state + `.uidvalidity` read/write /
  maildir scanning and file operations (place, rename, delete).
- `config.h` — the account table, included by `hml.c` (other files see it
  through the externs in `hml.h`). `hml.h` — all shared types.
- Style: clang-format via the repo's `.clang-format` (shared across the
  siblings: 4-space indent, attached braces, 80 columns) — run
  `clang-format -i` on files you touch. Blocking I/O with one thread per
  account (no event loop, no callbacks) is a deliberate design choice: the
  IMAP conversation must read linearly.
- Passwords: fetch under the shared mutex (one pinentry at a time), zero the
  buffer after login, never log command strings containing them.

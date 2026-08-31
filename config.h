/* hml configuration: the account table. Edit, then rebuild with `make`.
 * Included by hml.c only; other files see accounts/naccounts through the
 * extern declarations in hml.h. */

#define LEN(a) ((int)(sizeof(a) / sizeof *(a)))

/* every account is Gmail with the same folder layout; the trash channels
 * carry no Expunge in the mbsync config, so hml must not expunge either */
static const Channel gmail[] = {
    {"[Gmail]/All Mail", "All", 1},
    {"[Gmail]/Drafts", "Drafts", 1},
    {"[Gmail]/Sent Mail", "Sent", 1},
    {"[Gmail]/Trash", "Trash", 0},
};

const Account accounts[] = {
    {"cc", "imap.gmail.com", 993, "smtp.gmail.com", 465, "costa@codechem.com",
     "gpg -q --for-your-eyes-only --no-tty -d "
     "~/.password-store/costa@halicea.com.gpg",
     "~/.mail/cc", gmail, LEN(gmail)},
    {"km", "imap.gmail.com", 993, "smtp.gmail.com", 465,
     "kosta.mihajlov@gmail.com",
     "gpg -q --for-your-eyes-only --no-tty -d "
     "~/.password-store/kosta.mihajlov@gmail.com.gpg",
     "~/.mail/km", gmail, LEN(gmail)},
    {"chgm", "imap.gmail.com", 993, "smtp.gmail.com", 465,
     "costa.halicea@gmail.com",
     "gpg -q --for-your-eyes-only --no-tty -d "
     "~/.password-store/costa.halicea@gmail.com.gpg",
     "~/.mail/chgm", gmail, LEN(gmail)},
};
const int naccounts = LEN(accounts);

/* shell commands run once after `hml recv` (all accounts done; not on
 * status or -n dry runs) and after a successful `hml send`; "" = none */
const char *postrecv = "notmuch new";
const char *postsend = "";

/* search index: <mailroot>/.hml.db, an hml-only cache that `hml new`
 * rebuilds from scratch if deleted. Queries name boxes <account>/<near>,
 * e.g. path:cc/Sent */
const char *mailroot = "~/.mail";

/* tags derived from folders: a message with a file in a listed folder
 * carries that tag; a message in none of them is tag:inbox. Flag tags
 * (unread, flagged, replied, draft, passed) come from the maildir flags. */
const FolderTag foldertags[] = {
    {"Sent", "sent"},
    {"Drafts", "draft"},
    {"Trash", "deleted"},
};
const int nfoldertags = LEN(foldertags);

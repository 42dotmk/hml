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
const char *postrecv = "hml new";
const char *postsend = "";

/* local delivery: `hml send` puts mail for anyone @localdomain (or
 * @localhost) into the maildir <localbox>/<localpart>/ instead of
 * handing it to SMTP; no account is needed. `hml new` indexes those
 * boxes as <localdomain>/<localpart> (path:hal/main). hal's sessions
 * and the user's own hal inbox live here. */
const char *localdomain = "hal";
const char *localbox = "~/.mail/hal";

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

/* tag rules: `hml new` applies each to the messages it just indexed
 * that match the query. Manual `hml tag` edits (kept in
 * <mailroot>/.htags, replayed on rebuild) always win over rules. */
const TagRule tagrules[] = {
    /* hal's message bus: what hal is told stays out of my inbox view,
     * what it leaves for me (to:user@hal) stays in */
    {"+hal -inbox", "to:main@hal"},

    /* codechem forms */
    {"+job-application -inbox",
     "from:mailer@codechem.com subject:\"New Contact Application\""},
    {"+job-application -inbox",
     "from:mailer@halicea.com subject:\"New Job Application\""},
    {"+contact -inbox", "to:contact@codechem.com"},

    /* notifications / newsletters */
    {"+linkedin -inbox", "from:linkedin.com"},
    {"+linkedin +job-alert -inbox", "from:jobalerts-noreply@linkedin.com"},
    {"+github -inbox", "from:github.com"},
    {"+slack -inbox", "from:slack.com"},
    {"+bills +hetzner -inbox", "from:hetzner.com"},
    {"+quora -inbox", "from:quora.com"},

    /* banking — never inbox, as the notmuch rules had it */
    {"+bank -inbox", "from:unibank.com.mk"},
    {"+bank -inbox", "from:nlb.mk or from:24x7.com.mk"},
    {"+bank -inbox", "from:revolut.com"},
    {"+bank -inbox", "from:paypal.com"},
    {"+bank -inbox", "from:halkbank.mk"},
    {"+bank -inbox",
     "from:deutsche-bank.de or (from:db.com and not from:chess-db.com)"},
    {"+bank -inbox", "from:stb.com.mk"},
    {"+bank -inbox", "from:kb.com.mk"},
};
const int ntagrules = LEN(tagrules);

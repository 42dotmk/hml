#include "hml.h"

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

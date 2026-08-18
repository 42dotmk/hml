/* hml - keep maildirs and IMAP mailboxes in step, sharing mbsync's own
 * on-disk sync state so the two tools stay interchangeable */
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#include "hml.h"

typedef struct {
  const Account *a;
  int mode, force;
  int rc; /* 0 in sync, 1 differences, 2 error */
} Job;

static pthread_mutex_t outmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pwmtx =  PTHREAD_MUTEX_INITIALIZER; /* one pinentry at a time */

void report(const char *label, const char *fmt, ...) {
  va_list ap;

  pthread_mutex_lock(&outmtx);
  printf("%-12s ", label);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
  pthread_mutex_unlock(&outmtx);
}

char *runpasscmd(const char *cmd, char *err, size_t errlen) {
  char buf[256];
  FILE *p;
  size_t n;

  if (!(p = popen(cmd, "r"))) {
    snprintf(err, errlen, "cannot run passcmd");
    return NULL;
  }
  n = fread(buf, 1, sizeof buf - 1, p);
  buf[n] = '\0';
  if (pclose(p) != 0 || !buf[0]) {
    snprintf(err, errlen, "passcmd failed");
    return NULL;
  }
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = '\0';
  return strdup(buf);
}

static Imap *acctconnect(const Account *a, const char *pass, char *err,
                         size_t errlen) {
  Imap *im = imapconnect(a->host, a->port, err, errlen);

  if (!im)
    return NULL;
  if (imaplogin(im, a->user, pass, err, errlen) < 0) {
    imapclose(im);
    return NULL;
  }
  return im;
}

static void *accountmain(void *arg) {
  Job *j = arg;
  const Account *a = j->a;
  char err[256];
  char *pass;
  Imap *im;
  int i, rc;

  pthread_mutex_lock(&pwmtx);
  pass = runpasscmd(a->passcmd, err, sizeof err);
  pthread_mutex_unlock(&pwmtx);
  if (!pass) {
    report(a->name, "error: %s", err);
    j->rc = 2;
    return NULL;
  }
  im = acctconnect(a, pass, err, sizeof err);
  if (!im) {
    report(a->name, "error: %s", err);
    j->rc = 2;
    goto out;
  }
  for (i = 0; i < a->nchannels; i++) {
    rc = syncbox(im, a, &a->channels[i], j->mode, j->force);
    if (rc == 2) {
      /* Gmail drops long-lived connections mid-listing now and then;
       * reconnect and give the folder one more try */
      imapclose(im);
      if (!(im = acctconnect(a, pass, err, sizeof err))) {
        report(a->name, "error: reconnect: %s", err);
        j->rc = 2;
        goto out;
      }
      rc = syncbox(im, a, &a->channels[i], j->mode, j->force);
    }
    if (rc > j->rc)
      j->rc = rc;
  }
  imapclose(im);
out:
  memset(pass, 0, strlen(pass));
  free(pass);
  return NULL;
}

static int usage(int rc) {
  fputs("usage: hml [command] [-n] [-d] [account ...]\n"
        "  (none)   read-only status report\n"
        "  recv     sync: pull/push mail, flags and deletions\n"
        "  recv -n  dry run: list what recv would do\n"
        "  send     SMTP submission: hml send [-t] [-f from] [-a account]\n"
        "           [rcpt ...] < message\n"
        "  search   not implemented yet\n"
        "  -d       distrust caches, verify with a full listing\n",
        stderr);
  return rc;
}

int main(int argc, char *argv[]) {
  Job jobs[16];
  pthread_t tid[16];
  struct timespec t0, t1;
  int i = 1, k, njobs = 0, mode = MStatus, force = 0, rc = 0;

  if (argc > 1 && argv[1][0] != '-') {
    if (!strcmp(argv[1], "recv")) {
      mode = MSync;
      i = 2;
    } else if (!strcmp(argv[1], "status")) {
      i = 2;
    } else if (!strcmp(argv[1], "send")) {
      return sendmain(argc - 2, argv + 2);
    } else if (!strcmp(argv[1], "search")) {
      fputs("hml: search is not implemented yet\n", stderr);
      return 2;
    } else {
      /* not a command: an account name filters the status report */
      for (k = 0; k < naccounts; k++)
        if (!strcmp(argv[1], accounts[k].name))
          break;
      if (k == naccounts)
        return usage(2);
    }
  }
  for (; i < argc && argv[i][0] == '-'; i++) {
    const char *p;
    for (p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'n': mode = MDry; break;
      case 'd': force = 1; break;
      default:
        return usage(*p == 'h' ? 0 : 2);
      }
    }
  }
  for (k = 0; k < naccounts && njobs < 16; k++) {
    int want = i >= argc;
    int j;
    for (j = i; j < argc; j++)
      if (!strcmp(argv[j], accounts[k].name))
        want = 1;
    if (!want)
      continue;
    jobs[njobs].a = &accounts[k];
    jobs[njobs].mode = mode;
    jobs[njobs].force = force;
    jobs[njobs].rc = 0;
    njobs++;
  }
  if (!njobs) {
    fputs("hml: no matching account\n", stderr);
    return 2;
  }

  signal(SIGPIPE, SIG_IGN);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (i = 0; i < njobs; i++)
    pthread_create(&tid[i], NULL, accountmain, &jobs[i]);
  for (i = 0; i < njobs; i++) {
    pthread_join(tid[i], NULL);
    if (jobs[i].rc > rc)
      rc = jobs[i].rc;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("%.2fs\n", (double)(t1.tv_sec - t0.tv_sec) +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
  return rc;
}

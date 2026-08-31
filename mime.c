/* mime.c - the searchable text of a message: decoded headers, the text of
 * every text part (HTML stripped), attachment names. Only what the index
 * needs is produced; nothing is kept from parts that cannot be decoded. */
#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <stb_ds.h>

#include "hml.h"

enum { BodyMax = 512 * 1024, DepthMax = 16 };

typedef struct {
    const char *name;
    size_t nlen;
    const char *val; /* raw: still folded, leading space included */
    size_t vlen;
} Hdr;

typedef struct {
    char *body, *attach; /* stb_ds char arrays */
    int depth;
} Ctx;

static void putn(char **b, const char *s, size_t n) {
    if (n)
        memcpy(arraddnptr(*b, n), s, n);
}

static void putstr(char **b, const char *s) { putn(b, s, strlen(s)); }

static void putcp(char **b, unsigned long cp) {
    char u[4];
    int n;

    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | cp >> 6);
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12);
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else if (cp < 0x110000) {
        u[0] = (char)(0xF0 | cp >> 18);
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    } else
        return;
    putn(b, u, (size_t)n);
}

/* stb array -> malloc'd string; the array is freed */
static char *fin(char **b) {
    char *s;

    arrput(*b, '\0');
    s = strdup(*b);
    arrfree(*b);
    *b = NULL;
    return s;
}

/* replace NULs and malformed UTF-8 in place so SQLite sees clean text */
static void utf8fix(char *s) {
    size_t i = 0, k, n = strlen(s), j;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            k = 1;
        else if (c >= 0xE0 && c <= 0xEF)
            k = 2;
        else if (c >= 0xF0 && c <= 0xF4)
            k = 3;
        else {
            s[i++] = '?';
            continue;
        }
        for (j = 1; j <= k; j++)
            if (i + j >= n || ((unsigned char)s[i + j] & 0xC0) != 0x80)
                break;
        if (j <= k)
            s[i++] = '?';
        else
            i += k + 1;
    }
}

static const char *findci(const char *s, size_t n, const char *needle) {
    size_t k = strlen(needle), i;

    for (i = 0; i + k <= n; i++)
        if (!strncasecmp(s + i, needle, k))
            return s + i;
    return NULL;
}

/* --- header block ------------------------------------------------------ */

/* collect the headers; returns the offset where the body starts */
static size_t headers(const char *s, size_t n, Hdr **out) {
    size_t i = 0;

    while (i < n) {
        size_t ls = i, ce;
        const char *nl = memchr(s + i, '\n', n - i), *colon;
        ce = nl ? (size_t)(nl - s) : n;
        i = nl ? ce + 1 : n;
        if (ce > ls && s[ce - 1] == '\r')
            ce--;
        if (ce == ls) /* blank line: body follows */
            return i;
        if ((s[ls] == ' ' || s[ls] == '\t') && arrlen(*out)) {
            Hdr *h = &arrlast(*out);
            h->vlen = (size_t)(s + ce - h->val); /* folded continuation */
            continue;
        }
        if (!(colon = memchr(s + ls, ':', ce - ls)))
            continue; /* not a header line (mbox From_ and such) */
        Hdr h = {s + ls, (size_t)(colon - (s + ls)), colon + 1,
                 (size_t)(s + ce - (colon + 1))};
        arrput(*out, h);
    }
    return n;
}

/* first header of that name, unfolded and trimmed; NULL if absent */
static char *hget(Hdr *h, const char *name) {
    size_t k = strlen(name), i, j, n;
    char *v;

    for (i = 0; i < arrlenu(h); i++) {
        if (h[i].nlen != k || strncasecmp(h[i].name, name, k))
            continue;
        if (!(v = malloc(h[i].vlen + 1)))
            return NULL;
        for (j = n = 0; j < h[i].vlen; j++)
            if (h[i].val[j] != '\r' && h[i].val[j] != '\n')
                v[n++] = h[i].val[j];
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
            n--;
        v[n] = '\0';
        for (j = 0; v[j] == ' ' || v[j] == '\t'; j++)
            ;
        memmove(v, v + j, n - j + 1);
        return v;
    }
    return NULL;
}

/* --- decoders ---------------------------------------------------------- */

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

static void b64dec(char **out, const char *s, size_t n) {
    unsigned acc = 0;
    int bits = 0, v;
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '=')
            break;
        if ((v = b64val((unsigned char)s[i])) < 0)
            continue;
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            arrput(*out, (char)(acc >> bits & 0xFF));
        }
    }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* quoted-printable; in header (RFC 2047 "Q") mode '_' is a space */
static void qpdec(char **out, const char *s, size_t n, int hdr) {
    size_t i;
    int a, b;

    for (i = 0; i < n; i++) {
        if (s[i] == '=') {
            if (i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {
                i += s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n' ? 2 : 1;
                continue; /* soft line break */
            }
            if (i + 2 < n && (a = hexval(s[i + 1])) >= 0 &&
                (b = hexval(s[i + 2])) >= 0) {
                arrput(*out, (char)(a << 4 | b));
                i += 2;
                continue;
            }
        }
        arrput(*out, hdr && s[i] == '_' ? ' ' : s[i]);
    }
}

/* append in as UTF-8; unknown charsets and bad input degrade to '?' */
static void toutf8(char **out, const char *cs, const char *in, size_t n) {
    char buf[4096], *ip = (char *)in, *op;
    size_t il = n, ol, r;
    iconv_t cd;

    if (!cs || !*cs || !strcasecmp(cs, "utf-8") || !strcasecmp(cs, "utf8") ||
        !strcasecmp(cs, "us-ascii") || !strcasecmp(cs, "ascii") ||
        (cd = iconv_open("UTF-8", cs)) == (iconv_t)-1) {
        putn(out, in, n);
        return;
    }
    while (il) {
        op = buf;
        ol = sizeof buf;
        r = iconv(cd, &ip, &il, &op, &ol);
        putn(out, buf, sizeof buf - ol);
        if (r != (size_t)-1)
            break;
        if (errno == EILSEQ) {
            ip++;
            il--;
            arrput(*out, '?');
        } else if (errno != E2BIG)
            break; /* EINVAL: truncated multibyte sequence at the end */
    }
    iconv_close(cd);
}

/* RFC 2047: "=?charset?B|Q?text?=" words, whitespace between two adjacent
 * words dropped; the rest of the value is passed through */
static char *hdrdecode(const char *v) {
    char *out = NULL, *tmp, cs[64];
    const char *p = v, *q, *c1, *c2, *end, *lastend = NULL;
    size_t k;

    while (*p) {
        if (!(q = strstr(p, "=?"))) {
            putstr(&out, p);
            break;
        }
        c1 = strchr(q + 2, '?');
        c2 = c1 ? strchr(c1 + 1, '?') : NULL;
        end = c2 ? strstr(c2 + 1, "?=") : NULL;
        if (!end || c2 - c1 != 2 || (size_t)(c1 - q - 2) >= sizeof cs) {
            putn(&out, p, (size_t)(q + 2 - p));
            p = q + 2;
            lastend = NULL;
            continue;
        }
        if (!(lastend == p && (size_t)(q - p) == strspn(p, " \t")))
            putn(&out, p, (size_t)(q - p));
        k = (size_t)(c1 - q - 2);
        memcpy(cs, q + 2, k);
        cs[k] = '\0';
        if (strchr(cs, '*')) /* RFC 2231 language tag */
            *strchr(cs, '*') = '\0';
        tmp = NULL;
        if (c1[1] == 'B' || c1[1] == 'b')
            b64dec(&tmp, c2 + 1, (size_t)(end - c2 - 1));
        else
            qpdec(&tmp, c2 + 1, (size_t)(end - c2 - 1), 1);
        toutf8(&out, cs, tmp, arrlenu(tmp));
        arrfree(tmp);
        p = lastend = end + 2;
    }
    return fin(&out);
}

/* --- Content-* header parameters --------------------------------------- */

/* "type/subtype" lowercased, from the start of a Content-Type value */
static void mediatype(const char *v, char *out, size_t cap) {
    size_t i;

    for (i = 0;
         i + 1 < cap && v[i] && v[i] != ';' && v[i] != ' ' && v[i] != '\t'; i++)
        out[i] = (char)tolower((unsigned char)v[i]);
    out[i] = '\0';
}

/* value of a ;name= parameter, unquoted, RFC 2231 name*= decoded to
 * UTF-8, malloc'd; NULL if absent */
static char *param(const char *v, const char *name) {
    size_t k = strlen(name);
    const char *p = v, *e;
    char *out = NULL, *raw = NULL;
    int ext;

    while ((p = strchr(p, ';'))) {
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncasecmp(p, name, k))
            continue;
        p += k;
        ext = 0;
        if (*p == '*') { /* name*= or name*0= (first continuation only) */
            p++;
            if (*p == '0')
                p++;
            if (*p == '*') {
                p++;
                ext = 1;
            }
        }
        if (*p != '=')
            continue;
        p++;
        if (*p == '"') {
            e = strchr(++p, '"');
            if (!e)
                e = p + strlen(p);
        } else {
            e = p;
            while (*e && *e != ';' && *e != ' ' && *e != '\t')
                e++;
        }
        putn(&raw, p, (size_t)(e - p));
        arrput(raw, '\0');
        if (ext) { /* charset'lang'percent-encoded */
            char *cs = raw, *q = strchr(raw, '\''), *r, *t = NULL;
            if (q && (r = strchr(q + 1, '\''))) {
                *q = '\0';
                for (r++; *r; r++) {
                    int a, b;
                    if (*r == '%' && (a = hexval(r[1])) >= 0 &&
                        (b = hexval(r[2])) >= 0) {
                        arrput(t, (char)(a << 4 | b));
                        r += 2;
                    } else
                        arrput(t, *r);
                }
                toutf8(&out, cs, t, arrlenu(t));
                arrfree(t);
            } else
                putstr(&out, raw);
        } else
            putstr(&out, raw);
        arrfree(raw);
        return fin(&out);
    }
    return NULL;
}

/* --- dates ------------------------------------------------------------- */

static long long civil(long y, int m, int d) { /* days since 1970-01-01 */
    long long era, doe, yoe, doy;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* RFC 5322 date, tolerant of the usual deviations; 0 if hopeless */
static long parsedate(const char *s) {
    static const char *mon = "janfebmaraprmayjunjulaugsepoctnovdec";
    static const struct {
        const char *name;
        int off;
    } zones[] = {{"UT", 0},   {"GMT", 0},  {"EST", -5}, {"EDT", -4},
                 {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6},
                 {"PST", -8}, {"PDT", -7}, {"CET", 1},  {"CEST", 2}};
    const char *p = s, *m;
    long day, year, hh = 0, mm = 0, ss = 0, off = 0;
    int month;
    size_t i;

    while (*p == ' ' || *p == '\t')
        p++;
    if (isalpha((unsigned char)*p)) { /* weekday */
        while (isalpha((unsigned char)*p))
            p++;
        while (*p == ',' || *p == ' ')
            p++;
    }
    day = strtol(p, (char **)&p, 10);
    while (*p == ' ' || *p == '-')
        p++;
    if (!isalpha((unsigned char)p[0]) || !isalpha((unsigned char)p[1]) ||
        !isalpha((unsigned char)p[2]))
        return 0;
    for (month = 0; month < 12; month++) {
        m = mon + month * 3;
        if (tolower((unsigned char)p[0]) == m[0] &&
            tolower((unsigned char)p[1]) == m[1] &&
            tolower((unsigned char)p[2]) == m[2])
            break;
    }
    if (month == 12)
        return 0;
    while (isalpha((unsigned char)*p))
        p++;
    while (*p == ' ' || *p == '-')
        p++;
    year = strtol(p, (char **)&p, 10);
    if (year < 100)
        year += year < 70 ? 2000 : 1900;
    if (day < 1 || day > 31 || year < 1970)
        return 0;
    while (*p == ' ')
        p++;
    if (isdigit((unsigned char)*p)) {
        hh = strtol(p, (char **)&p, 10);
        if (*p == ':')
            mm = strtol(p + 1, (char **)&p, 10);
        if (*p == ':')
            ss = strtol(p + 1, (char **)&p, 10);
    }
    while (*p == ' ')
        p++;
    if (*p == '+' || *p == '-') {
        long z = strtol(p + 1, NULL, 10);
        off = (z / 100 * 60 + z % 100) * 60 * (*p == '-' ? -1 : 1);
    } else
        for (i = 0; i < sizeof zones / sizeof *zones; i++)
            if (!strncasecmp(p, zones[i].name, strlen(zones[i].name)))
                off = zones[i].off * 3600;
    return (long)(civil(year, month + 1, (int)day) * 86400 + hh * 3600 +
                  mm * 60 + ss - off);
}

/* --- body text --------------------------------------------------------- */

static void entity(char **out, const char *s, size_t n, size_t *adv) {
    static const struct {
        const char *name;
        const char *rep;
    } ents[] = {{"amp", "&"},  {"lt", "<"},   {"gt", ">"},    {"quot", "\""},
                {"apos", "'"}, {"nbsp", " "}, {"ndash", "-"}, {"mdash", "-"}};
    size_t k, i;

    for (k = 1; k < n && k < 12 && s[k] != ';'; k++)
        ;
    if (k >= n || s[k] != ';') {
        arrput(*out, '&');
        *adv = 1;
        return;
    }
    *adv = k + 1;
    if (s[1] == '#') {
        unsigned long cp = s[2] == 'x' || s[2] == 'X'
                               ? strtoul(s + 3, NULL, 16)
                               : strtoul(s + 2, NULL, 10);
        putcp(out, cp ? cp : '?');
        return;
    }
    for (i = 0; i < sizeof ents / sizeof *ents; i++)
        if (strlen(ents[i].name) == k - 1 &&
            !strncmp(s + 1, ents[i].name, k - 1)) {
            putstr(out, ents[i].rep);
            return;
        }
    putn(out, s, k + 1); /* unknown: keep as written */
}

static int blocktag(const char *t, size_t n) {
    static const char *blocks[] = {
        "br", "p",     "div", "tr", "li", "h1", "h2", "h3",  "h4",        "h5",
        "h6", "table", "hr",  "td", "th", "ul", "ol", "pre", "blockquote"};
    size_t i;

    for (i = 0; i < sizeof blocks / sizeof *blocks; i++)
        if (strlen(blocks[i]) == n && !strncasecmp(t, blocks[i], n))
            return 1;
    return 0;
}

/* HTML to text: tags dropped, style/script bodies skipped, entities
 * decoded, whitespace collapsed */
static void htmltext(char **out, const char *s, size_t n) {
    size_t i = 0, j, ns, adv;
    int sp = 1, close;
    const char *e;

    while (i < n) {
        if (s[i] == '<') {
            if (i + 4 <= n && !memcmp(s + i, "<!--", 4)) {
                e = findci(s + i + 4, n - i - 4, "-->");
                i = e ? (size_t)(e - s) + 3 : n;
                continue;
            }
            j = i + 1;
            close = j < n && s[j] == '/';
            if (close)
                j++;
            ns = j;
            while (j < n && isalnum((unsigned char)s[j]))
                j++;
            if (blocktag(s + ns, j - ns)) {
                arrput(*out, '\n');
                sp = 1;
            }
            if (!close && (j - ns == 5 && !strncasecmp(s + ns, "style", 5))) {
                e = findci(s + j, n - j, "</style");
                i = e ? (size_t)(e - s) : n;
                continue;
            }
            if (!close && (j - ns == 6 && !strncasecmp(s + ns, "script", 6))) {
                e = findci(s + j, n - j, "</script");
                i = e ? (size_t)(e - s) : n;
                continue;
            }
            while (j < n && s[j] != '>')
                j++;
            i = j < n ? j + 1 : n;
            continue;
        }
        if (s[i] == '&') {
            entity(out, s + i, n - i, &adv);
            i += adv;
            sp = 0;
            continue;
        }
        if (isspace((unsigned char)s[i])) {
            if (!sp)
                arrput(*out, ' ');
            sp = 1;
        } else {
            arrput(*out, s[i]);
            sp = 0;
        }
        i++;
    }
}

/* undo the Content-Transfer-Encoding; stb array */
static char *ctedecode(const char *cte, const char *s, size_t n) {
    char *out = NULL;

    if (cte && !strncasecmp(cte, "base64", 6))
        b64dec(&out, s, n);
    else if (cte && !strncasecmp(cte, "quoted-printable", 16))
        qpdec(&out, s, n, 0);
    else
        putn(&out, s, n);
    return out;
}

static void walk(Ctx *c, const char *s, size_t n);

/* the parts between --boundary lines, each walked as its own entity */
static void multipart(Ctx *c, const char *s, size_t n, const char *b) {
    size_t bl = strlen(b), pos = 0, start = 0, end;
    const char *nl;
    int in = 0;

    while (pos < n) {
        nl = memchr(s + pos, '\n', n - pos);
        end = nl ? (size_t)(nl - s) + 1 : n;
        if (end - pos >= bl + 2 && s[pos] == '-' && s[pos + 1] == '-' &&
            !memcmp(s + pos + 2, b, bl)) {
            if (in)
                walk(c, s + start, pos > start ? pos - start : 0);
            if (end - pos >= bl + 4 && s[pos + bl + 2] == '-' &&
                s[pos + bl + 3] == '-')
                return; /* closing delimiter */
            in = 1;
            start = end;
        }
        pos = end;
    }
    if (in)
        walk(c, s + start, n - start);
}

/* one entity: its own header block, then whatever the type says */
static void walk(Ctx *c, const char *s, size_t n) {
    Hdr *h = NULL;
    size_t bo = headers(s, n, &h);
    char *ct = hget(h, "Content-Type"),
         *cte = hget(h, "Content-Transfer-Encoding"),
         *cd = hget(h, "Content-Disposition"), *fn = NULL, *b, *dec, *u, *cs,
         type[128] = "text/plain";

    if (ct)
        mediatype(ct, type, sizeof type);
    if (cd)
        fn = param(cd, "filename");
    if (!fn && ct)
        fn = param(ct, "name");
    if (fn) {
        char *d = hdrdecode(fn);
        putstr(&c->attach, d);
        arrput(c->attach, ' ');
        free(d);
        free(fn);
    }
    if (!strncmp(type, "multipart/", 10) && ct && c->depth < DepthMax) {
        if ((b = param(ct, "boundary"))) {
            c->depth++;
            multipart(c, s + bo, n - bo, b);
            c->depth--;
            free(b);
        }
    } else if (!strcmp(type, "message/rfc822") && c->depth < DepthMax) {
        dec = ctedecode(cte, s + bo, n - bo);
        c->depth++;
        walk(c, dec, arrlenu(dec));
        c->depth--;
        arrfree(dec);
    } else if (!strncmp(type, "text/", 5) && arrlen(c->body) < BodyMax) {
        dec = ctedecode(cte, s + bo, n - bo);
        cs = ct ? param(ct, "charset") : NULL;
        u = NULL;
        toutf8(&u, cs, dec, arrlenu(dec));
        if (!strcmp(type, "text/html"))
            htmltext(&c->body, u, arrlenu(u));
        else
            putn(&c->body, u, arrlenu(u));
        arrput(c->body, '\n');
        arrfree(u);
        arrfree(dec);
        free(cs);
    }
    free(ct);
    free(cte);
    free(cd);
    arrfree(h);
}

/* --- message ----------------------------------------------------------- */

/* next <...> token in v; NULL when none remain */
static char *angle(const char **v) {
    const char *lt = strchr(*v, '<'), *gt;
    char *r;

    if (!lt || !(gt = strchr(lt, '>')))
        return NULL;
    *v = gt + 1;
    r = malloc((size_t)(gt - lt));
    memcpy(r, lt + 1, (size_t)(gt - lt - 1));
    r[gt - lt - 1] = '\0';
    return r;
}

static void addref(Mail *m, char *id) {
    ptrdiff_t i;

    if (!strcmp(id, m->mid)) {
        free(id);
        return;
    }
    for (i = 0; i < arrlen(m->refs); i++)
        if (!strcmp(m->refs[i], id)) {
            free(id);
            return;
        }
    arrput(m->refs, id);
}

static char *hdrtext(Hdr *h, const char *name) {
    char *raw = hget(h, name), *d;

    if (!raw)
        return strdup("");
    d = hdrdecode(raw);
    free(raw);
    utf8fix(d);
    return d;
}

int mailparse(const char *buf, size_t n, Mail *m) {
    Hdr *h = NULL;
    Ctx c = {NULL, NULL, 0};
    char *v, *cc, *id;
    const char *p;
    size_t i;

    memset(m, 0, sizeof *m);
    headers(buf, n, &h);
    if ((v = hget(h, "Message-ID"))) {
        p = v;
        if ((id = angle(&p)))
            m->mid = id;
        else if (*v)
            m->mid = strdup(v);
        free(v);
    }
    if (!m->mid) { /* no usable id: derive one from the content */
        uint64_t hash = 14695981039346656037ULL;
        char tmp[32];
        for (i = 0; i < n; i++)
            hash = (hash ^ (unsigned char)buf[i]) * 1099511628211ULL;
        snprintf(tmp, sizeof tmp, "hml.%016llx", (unsigned long long)hash);
        m->mid = strdup(tmp);
    }
    utf8fix(m->mid);
    m->subject = hdrtext(h, "Subject");
    m->from = hdrtext(h, "From");
    m->to = hdrtext(h, "To");
    for (i = 0; i < 2; i++) { /* To, Cc and Bcc all count as recipients */
        cc = hdrtext(h, i ? "Bcc" : "Cc");
        if (*cc) {
            size_t a = strlen(m->to), b = strlen(cc);
            m->to = realloc(m->to, a + b + 3);
            snprintf(m->to + a, b + 3, "%s%s", a ? ", " : "", cc);
        }
        free(cc);
    }
    if ((v = hget(h, "Date"))) {
        m->date = parsedate(v);
        free(v);
    }
    if ((v = hget(h, "References"))) {
        for (p = v; (id = angle(&p));)
            addref(m, id);
        free(v);
    }
    if ((v = hget(h, "In-Reply-To"))) {
        for (p = v; (id = angle(&p));)
            addref(m, id);
        free(v);
    }
    arrfree(h);
    walk(&c, buf, n);
    m->attach = fin(&c.attach);
    m->body = fin(&c.body);
    utf8fix(m->attach);
    utf8fix(m->body);
    return 0;
}

void mailfree(Mail *m) {
    ptrdiff_t i;

    free(m->mid);
    free(m->subject);
    free(m->from);
    free(m->to);
    free(m->attach);
    free(m->body);
    for (i = 0; i < arrlen(m->refs); i++)
        free(m->refs[i]);
    arrfree(m->refs);
    memset(m, 0, sizeof *m);
}

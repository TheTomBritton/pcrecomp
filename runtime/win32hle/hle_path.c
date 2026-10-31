/*
 * hle_path.c - Windows paths on a POSIX file system.
 *
 * A guest names files the Windows way: `C:\Westwood\Sun\SCORES.MIX`,
 * `..\Movies`, `data\rules.ini`, in any case. The host's files are on a
 * case-sensitive file system in whatever case they were installed in. So:
 *
 *   - a drive letter maps to a host directory (hle_set_drive; Z: is "/", as
 *     under Wine), and a path without one is relative to the host's cwd;
 *   - '\\' and '/' both separate;
 *   - each component that does not exist as spelled is looked up in its
 *     directory without regard to case.
 *
 * Lexical '..' traversal in a mapped-drive path stops at that drive's mapped
 * root. This is not a symlink sandbox: a filesystem symlink below the root
 * may still resolve outside it. Drive-relative forms such as C:folder are
 * currently treated as rooted at the mapped drive, not relative to a
 * per-drive current directory as on Win32.
 *
 * The reverse (a host directory as the guest should see it) is
 * hle_guest_path, for GetCurrentDirectoryA and GetModuleFileNameA.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include "win32hle.h"

static char *g_drive[26];

void hle_set_drive(char letter, const char *host_root) {
    int i = (letter | 0x20) - 'a';
    if (i < 0 || i >= 26) return;
    free(g_drive[i]);
    g_drive[i] = host_root ? realpath(host_root, NULL) : NULL;
}

static const char *drive_root(int i) {
    if (g_drive[i]) return g_drive[i];
    return i == 'z' - 'a' ? "" : NULL;          /* Z: is the host's root */
}

static int at_or_under(const char *path, const char *root) {
    size_t len = strlen(root);
    if (strncmp(path, root, len)) return 0;
    if (len == 1 && root[0] == '/') return path[0] == '/';
    return path[len] == '/' || path[len] == 0;
}

/* ---- directory listings, kept ----
 * A guest path is matched component by component without case, which
 * lists the directory on every miss, and a game probes for files it does
 * not have thousands of times as it loads. On a storage whose every call
 * is a message to another thread (the browser's) that took minutes, so a
 * directory's listing is kept until something is written anywhere
 * (hle_path_changed: the writers call it). */
#define DIR_CACHE 16
typedef struct { char *name; int is_dir; } dent_t;
static struct { char *dir; dent_t *ents; int n; unsigned gen; } g_dc[DIR_CACHE];
static int g_dc_next;
static unsigned g_path_gen = 1;
static pthread_mutex_t g_dc_lock = PTHREAD_MUTEX_INITIALIZER;
void hle_path_changed(void) { __sync_fetch_and_add(&g_path_gen, 1); }
static void dc_drop(int i) {
    for (int k = 0; k < g_dc[i].n; k++) free(g_dc[i].ents[k].name);
    free(g_dc[i].ents), free(g_dc[i].dir);
    g_dc[i].ents = NULL, g_dc[i].dir = NULL, g_dc[i].n = 0;
}
/* The entry of dir named want (case aside): its name as the directory has
 * it, and whether it is a directory; 0 when there is none. */
static int dir_find(const char *dir, const char *want, char *name, size_t n, int *is_dir) {
    unsigned gen = g_path_gen;
    pthread_mutex_lock(&g_dc_lock);
    int slot = -1;
    for (int i = 0; i < DIR_CACHE; i++)
        if (g_dc[i].dir && !strcmp(g_dc[i].dir, dir)) { slot = i; break; }
    if (slot >= 0 && g_dc[slot].gen != gen) { dc_drop(slot); slot = -1; }
    if (slot < 0) {
        slot = g_dc_next, g_dc_next = (g_dc_next + 1) % DIR_CACHE;
        dc_drop(slot);
        int cap = 64, cnt = 0;
        dent_t *ents = (dent_t *)malloc(sizeof *ents * (size_t)cap);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (cnt == cap) ents = (dent_t *)realloc(ents, sizeof *ents * (size_t)(cap *= 2));
            ents[cnt].name = strdup(e->d_name);
            int t = e->d_type == DT_DIR ? 1 : e->d_type == DT_UNKNOWN ? -1 : 0;
            if (t < 0) {
                char full[1024];
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
                t = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            }
            ents[cnt++].is_dir = t;
        }
        if (d) closedir(d);
        g_dc[slot].dir = strdup(dir), g_dc[slot].ents = ents, g_dc[slot].n = cnt, g_dc[slot].gen = gen;
    }
    int found = 0;
    for (int k = 0; k < g_dc[slot].n && !found; k++)
        if (!strcasecmp(g_dc[slot].ents[k].name, want)) {
            snprintf(name, n, "%s", g_dc[slot].ents[k].name);
            if (is_dir) *is_dir = g_dc[slot].ents[k].is_dir;
            found = 1;
        }
    pthread_mutex_unlock(&g_dc_lock);
    return found;
}
/* stat, through the listings: 1 if there, with whether it is a directory. */
static int cached_stat(const char *path, int *is_dir) {
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1]) { struct stat st; int r = stat(path, &st) == 0; if (r && is_dir) *is_dir = S_ISDIR(st.st_mode); return r; }
    char dir[1024], name[512];
    size_t dl = (size_t)(slash - path);
    if (dl >= sizeof dir) return 0;
    memcpy(dir, path, dl), dir[dl] = 0;
    return dir_find(dl ? dir : "/", slash + 1, name, sizeof name, is_dir) && !strcmp(name, slash + 1);
}

/* Append one component to out (which holds an existing directory, or ""
 * for the cwd), matching it without case if it is not there as spelled.
 * Returns 1 if the result exists. An optional anchor clamps lexical parent
 * traversal at a mapped root. This does not sandbox symlink targets. */
static int join_component(char *out, size_t n, const char *anchor, const char *comp, size_t clen) {
    size_t len = strlen(out);
    if (clen == 1 && comp[0] == '.') return 1;
    if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
        if (anchor) {
            size_t root_len = strlen(anchor);
            if (!at_or_under(out, anchor) || len <= root_len) {
                snprintf(out, n, "%s", anchor);
                return 1;
            }
            char *slash = strrchr(out, '/');
            if (!slash || (size_t)(slash - out) < root_len) snprintf(out, n, "%s", anchor);
            else if (slash == out) slash[1] = 0;
            else *slash = 0;
            if (!at_or_under(out, anchor)) snprintf(out, n, "%s", anchor);
            return 1;
        }
        if (!len || !strcmp(out, ".") ) { snprintf(out, n, ".."); return 1; }
        char *slash = strrchr(out, '/');
        if (slash && strcmp(slash + 1, "..")) { if (slash == out) slash[1] = 0; else *slash = 0; return 1; }
        snprintf(out + len, n - len, "%s..", len && out[len - 1] != '/' ? "/" : "");
        return 1;
    }
    char want[512];
    if (clen >= sizeof want) clen = sizeof want - 1;
    memcpy(want, comp, clen), want[clen] = 0;
    const char *sep = len && out[len - 1] != '/' ? "/" : "";
    char name[512];
    int found = dir_find(len ? out : ".", want, name, sizeof name, NULL);
    snprintf(out + len, n - len, "%s%s", sep, found ? name : want);
    return found;
}

/* ---- an overlay: a mod's folder laid over the game's ----
 *
 * With hle_set_overlay(base, overlay), a file under base is looked for in
 * overlay first (at the same relative path, without case), so a mod's
 * rules.ini or map is read instead of the game's. Writes to files under base
 * go to the overlay (hle_host_path_for_write), so playing a mod never changes
 * the game's own files; FindFirstFileA lists both, the overlay's first. */
static char *g_overlay_base, *g_overlay;

void hle_set_overlay(const char *base, const char *overlay) {
    free(g_overlay_base), free(g_overlay);
    g_overlay_base = base && overlay ? realpath(base, NULL) : NULL;
    g_overlay = base && overlay ? realpath(overlay, NULL) : NULL;
    if (!g_overlay_base || !g_overlay) free(g_overlay_base), free(g_overlay), g_overlay_base = g_overlay = NULL;
}

/* host as an absolute path, if it is relative (to the cwd). */
static void absolute(const char *host, char *out, size_t n) {
    char cwd[768];
    if (host[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(out, n, "%s", host);
    else snprintf(out, n, "%s/%s", cwd, strcmp(host, ".") ? host : "");
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

static int under(const char *path, const char *dir) {
    size_t l = strlen(dir);
    if (l == 1 && dir[0] == '/') return path[0] == '/';
    return !strncmp(path, dir, l) && (path[l] == '/' || !path[l]);
}

/* Lexically canonicalize an absolute POSIX path without resolving symlinks.
 * This is used for overlay prefix classification, not filesystem isolation. */
static int normalize_absolute(const char *path, char *out, size_t n) {
    if (!path || path[0] != '/' || !n) return 0;
    if (snprintf(out, n, "/") >= (int)n) return 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {
            /* skip */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (strcmp(out, "/")) {
                char *slash = strrchr(out, '/');
                if (!slash || slash == out) snprintf(out, n, "/");
                else *slash = 0;
            }
        } else if (len) {
            size_t used = strlen(out);
            int wrote = snprintf(out + used, n - used, "%s%.*s", used > 1 ? "/" : "", (int)len, p);
            if (wrote < 0 || (size_t)wrote >= n - used) return 0;
        }
        p = e;
        while (*p == '/') p++;
    }
    return 1;
}

int hle_overlay_of(const char *host, char *out, size_t n) {
    char abs[1024], normalized[1024];
    out[0] = 0;
    if (!g_overlay) return -1;
    absolute(host, abs, sizeof abs);
    if (!normalize_absolute(abs, normalized, sizeof normalized) ||
        under(normalized, g_overlay) || !under(normalized, g_overlay_base)) return -1;
    snprintf(out, n, "%s", g_overlay);
    const char *p = normalized + strlen(g_overlay_base);
    int exists = 1;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        if (e > p) exists = join_component(out, n, g_overlay, p, (size_t)(e - p)) && exists;
        p = e;
    }
    return exists;
}

/* The path a write to guest should go to: the overlay's copy when the file is
 * the game's (its directories made; with keep, the game's file copied there
 * first, so a write into the middle of it keeps the rest). */
int hle_host_path_for_write(const char *guest, char *out, size_t n, int keep) {
    int exists = hle_host_path(guest, out, n);
    char ov[1024];
    if (hle_overlay_of(out, ov, sizeof ov) < 0) return exists;
    for (char *s = ov + strlen(g_overlay); *s && (s = strchr(s + 1, '/')); ) {   /* mkdir -p its directory */
        *s = 0;
        mkdir(ov, 0755);
        *s = '/';
    }
    if (keep && exists) {
        FILE *in = fopen(out, "rb"), *to = in ? fopen(ov, "wb") : NULL;
        char buf[65536];
        size_t r;
        while (to && (r = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, r, to);
        if (in) fclose(in);
        if (to) fclose(to);
    }
    hle_path_changed();                                    /* what the caller writes next is new */
    snprintf(out, n, "%s", ov);
    return keep && exists;
}

static int host_path_plain(const char *guest, char *out, size_t n);

/* Guest paths a host hides: hle_host_path answers "not there" for them and
 * hands back an empty host path, which no open can use. The House of the
 * Dead takes the first CD-ROM drive whose root has no thotd.exe for its
 * disc; the virtual disc is the game folder, which has one. */
#define MAX_HIDDEN 8
static char g_hidden[MAX_HIDDEN][260];
static int g_nhidden;
void hle_path_hide(const char *guest) {
    if (g_nhidden < MAX_HIDDEN && guest && strlen(guest) < 260) snprintf(g_hidden[g_nhidden++], 260, "%s", guest);
}
static int hidden(const char *guest) {
    for (int i = 0; i < g_nhidden; i++) {
        const char *a = g_hidden[i], *b = guest;
        while (*a && *b) {
            char ca = *a == '/' ? '\\' : (char)((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a);
            char cb = *b == '/' ? '\\' : (char)((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b);
            if (ca != cb) break;
            a++, b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

int hle_host_path(const char *guest, char *out, size_t n) {
    if (guest && g_nhidden && hidden(guest)) { out[0] = 0; return 0; }
    int exists = host_path_plain(guest, out, n);
    char ov[1024];
    struct stat st;
    /* A file only: a directory stays the game's (the cwd is set through here,
     * and FindFirstFileA lists both) */
    int is_dir = 0;
    (void)st;
    if (g_overlay && hle_overlay_of(out, ov, sizeof ov) == 1 && cached_stat(ov, &is_dir) && !is_dir) {
        snprintf(out, n, "%s", ov);
        return 1;
    }
    return exists;
}

/* The game's own path for guest, ignoring any overlay. */
int hle_host_path_base(const char *guest, char *out, size_t n) { return host_path_plain(guest, out, n); }

static int host_path_plain(const char *guest, char *out, size_t n) {
    out[0] = 0;
    if (!guest) return 0;
    const char *p = guest;
    const char *anchor = NULL;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        const char *root = drive_root((p[0] | 0x20) - 'a');
        if (!root) return 0;
        anchor = *root ? root : "/";
        snprintf(out, n, "%s", anchor);
        p += 2;
        while (*p == '\\' || *p == '/') p++;
    } else if (*p == '\\' || *p == '/') {
        /* rooted without a drive: the C: drive's root, if there is one */
        const char *root = drive_root(2);
        anchor = root && *root ? root : "/";
        snprintf(out, n, "%s", anchor);
        while (*p == '\\' || *p == '/') p++;
    }
    while (*p) {
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        if (e > p) join_component(out, n, anchor, p, (size_t)(e - p));
        p = e;
        while (*p == '\\' || *p == '/') p++;
    }
    if (!out[0]) snprintf(out, n, ".");
    /* Through the kept listings (#148), not a stat per probe; a listing has
     * broken symlinks too, as lstat would (#146). */
    return cached_stat(out, NULL);
}

/* A host path (absolute) as the guest sees it: under a mapped drive, that
 * drive; otherwise Z: and the whole path. Backslashes. */
void hle_guest_path(const char *host, char *out, size_t n) {
    char abs[1024];
    if (host[0] != '/') {
        char cwd[768];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, host);
    } else {
        snprintf(abs, sizeof abs, "%s", host);
    }
    char *real = realpath(abs, NULL);
    const char *h = real ? real : abs;
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < 26; i++) {
        const char *r = g_drive[i];
        size_t l = r ? strlen(r) : 0;
        if (r && !strncmp(h, r, l) && (h[l] == '/' || !h[l]) && l >= best_len) best = i, best_len = l;
    }
    const char *rest = best >= 0 ? h + best_len : h;
    snprintf(out, n, "%c:%s", best >= 0 ? 'A' + best : 'Z', *rest ? "" : "\\");
    size_t k = strlen(out);
    for (; *rest && k + 1 < n; rest++) out[k++] = *rest == '/' ? '\\' : *rest;
    out[k] = 0;
    free(real);
}

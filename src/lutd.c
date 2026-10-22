/*
 * lutd - apply a 3D LUT (.cube) to JPEGs written by a Samsung NX300
 *
 * Run on the camera (armel, root, started from /mnt/mmc/autoexec.sh).
 * Watches the SD card, and whenever the camera closes a new *.JPG,
 * re-encodes it through the LUT (in place, or as a _LUT copy).
 *
 * Modes:
 *   lutd --once IN.JPG OUT.JPG LOOK.cube [quality]   (desktop test)
 *   lutd [CONF]                                       (daemon)
 *
 * Licence: public domain / CC0. Use at your own risk: it rewrites the
 * JPEGs on your card, keep KEEP_ORIG=1 until you trust it.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <jpeglib.h>
#include <linux/input.h>

#define LUT_MAX   65
#define PATH_MAX2 1024
#define PATH_BIG  (PATH_MAX2 + 320)   /* config path + "/" + filename */

/* ------------------------------------------------------------------ */
/* logging                                                             */

static FILE *g_log;

static void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    va_list ap;
    FILE *f = g_log ? g_log : stderr;

    localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(f, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

/* ------------------------------------------------------------------ */
/* libjpeg error handling: never let a broken file kill the daemon     */

static jmp_buf g_jb;

static void jpeg_error_exit(j_common_ptr ci)
{
    char buf[JMSG_LENGTH_MAX];
    (*ci->err->format_message)(ci, buf);
    logmsg("jpeg: %s", buf);
    longjmp(g_jb, 1);
}

static void jpeg_emit_message(j_common_ptr ci, int level)
{
    (void)ci;
    (void)level;
}

/* ------------------------------------------------------------------ */
/* LUT                                                                 */

typedef struct {
    int n3, n1;
    unsigned short *t3;          /* n3^3*3, index (((b*n3+g)*n3+r)*3+c), 0..65535 */
    unsigned short *t1;          /* n1*3 shaper, 0..65535 */
    double dmin[3], dmax[3];
    unsigned char idx[3][256];   /* per input byte: 3D lattice index */
    unsigned char fr[3][256];    /* per input byte: lattice fraction, 0..255 */
} Lut;

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

static int lut_load(const char *path, Lut *L)
{
    FILE *f;
    char line[1024];
    long i1 = 0, i3 = 0;
    int c;

    memset(L, 0, sizeof *L);
    for (c = 0; c < 3; c++) { L->dmin[c] = 0.0; L->dmax[c] = 1.0; }

    if (!(f = fopen(path, "r"))) {
        logmsg("lut: cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        int n;

        if (!*p || *p == '#') continue;

        if (!strncasecmp(p, "TITLE", 5)) continue;

        if (sscanf(p, "LUT_1D_SIZE %d", &n) == 1) {
            if (n < 2 || n > 4096) { logmsg("lut: bad 1D size %d", n); goto bad; }
            L->n1 = n;
            L->t1 = calloc((size_t)n * 3, sizeof(unsigned short));
            if (!L->t1) goto nomem;
            continue;
        }
        if (sscanf(p, "LUT_3D_SIZE %d", &n) == 1) {
            if (n < 2 || n > LUT_MAX) { logmsg("lut: bad 3D size %d (max %d)", n, LUT_MAX); goto bad; }
            L->n3 = n;
            L->t3 = calloc((size_t)n * n * n * 3, sizeof(unsigned short));
            if (!L->t3) goto nomem;
            continue;
        }
        if (sscanf(p, "DOMAIN_MIN %lf %lf %lf", &L->dmin[0], &L->dmin[1], &L->dmin[2]) == 3)
            continue;
        if (sscanf(p, "DOMAIN_MAX %lf %lf %lf", &L->dmax[0], &L->dmax[1], &L->dmax[2]) == 3)
            continue;
        if (!strncasecmp(p, "LUT_1D_INPUT_RANGE", 18) ||
            !strncasecmp(p, "LUT_3D_INPUT_RANGE", 18)) {
            logmsg("lut: %s is not supported, assuming 0..1 domain", p);
            continue;
        }

        {
            double r, g, b;
            if (sscanf(p, "%lf %lf %lf", &r, &g, &b) != 3) {
                logmsg("lut: skip unreadable line (file may be truncated): %s", p);
                continue;
            }
            if (L->n1 && i1 < (long)L->n1 * 3) {
                L->t1[i1++] = (unsigned short)((r < 0 ? 0 : r > 1 ? 1 : r) * 65535.0 + 0.5);
                L->t1[i1++] = (unsigned short)((g < 0 ? 0 : g > 1 ? 1 : g) * 65535.0 + 0.5);
                L->t1[i1++] = (unsigned short)((b < 0 ? 0 : b > 1 ? 1 : b) * 65535.0 + 0.5);
            } else if (L->n3 && i3 < (long)L->n3 * L->n3 * L->n3 * 3) {
                L->t3[i3++] = (unsigned short)((r < 0 ? 0 : r > 1 ? 1 : r) * 65535.0 + 0.5);
                L->t3[i3++] = (unsigned short)((g < 0 ? 0 : g > 1 ? 1 : g) * 65535.0 + 0.5);
                L->t3[i3++] = (unsigned short)((b < 0 ? 0 : b > 1 ? 1 : b) * 65535.0 + 0.5);
            } else {
                logmsg("lut: too many entries, ignoring rest");
                break;
            }
        }
    }
    fclose(f);

    if (!L->n3) { logmsg("lut: no LUT_3D_SIZE in %s", path); return -1; }
    if (i3 != (long)L->n3 * L->n3 * L->n3 * 3) {
        logmsg("lut: %s has %ld of %ld 3D values", path, i3,
               (long)L->n3 * L->n3 * L->n3 * 3);
        return -1;
    }
    if (L->n1 && i1 != (long)L->n1 * 3) { logmsg("lut: incomplete 1D table"); return -1; }

    for (c = 0; c < 3; c++) {
        if (L->dmax[c] <= L->dmin[c]) { logmsg("lut: bad domain"); return -1; }
    }
    return 0;

nomem:
    logmsg("lut: out of memory");
bad:
    fclose(f);
    return -1;
}

/* 1D shaper at normalised input x, returns 0..65535 */
static unsigned short shaper(const Lut *L, int ch, double x)
{
    double pos, a, b;
    int i;

    if (L->n1 < 2) return (unsigned short)(x * 65535.0 + 0.5);

    pos = x * (L->n1 - 1);
    i = (int)pos;
    if (i >= L->n1 - 1) { i = L->n1 - 2; pos = (double)i; }
    a = L->t1[(size_t)i * 3 + ch];
    b = L->t1[(size_t)(i + 1) * 3 + ch];
    return (unsigned short)(a + (b - a) * (pos - i) + 0.5);
}

static int lut_prepare(Lut *L)
{
    int c, v;

    for (c = 0; c < 3; c++) {
        for (v = 0; v < 256; v++) {
            double y = shaper(L, c, v / 255.0) / 65535.0;
            double z = (y - L->dmin[c]) / (L->dmax[c] - L->dmin[c]);
            double pos, frac;
            int i;

            if (z < 0.0) z = 0.0;
            if (z > 1.0) z = 1.0;
            pos = z * (L->n3 - 1);
            i = (int)pos;
            frac = pos - i;
            if (i >= L->n3 - 1) { i = L->n3 - 2; frac = 1.0; }
            L->idx[c][v] = (unsigned char)i;
            L->fr[c][v] = (unsigned char)(frac * 255.0 + 0.5);
        }
    }
    return 0;
}

static inline int lerp8(int a, int b, int f)
{
    return a + (((b - a) * f) >> 8);
}

/* ------------------------------------------------------------------ */
/* active LUT switching                                                */
/*                                                                     */
/* Put a file named "active" next to the LUTs, containing one file     */
/* name (or an absolute path).  It is re-checked before every photo,   */
/* so switching does not need a rebuild - only a fresh boot.           */
/* ------------------------------------------------------------------ */

static Lut g_lut;
static int g_force_default = 0;
static char g_lut_default[PATH_BIG];   /* cfg->lut                    */
static char g_lut_dir[PATH_BIG];       /* dirname(cfg->lut)           */
static char g_lut_cur[PATH_BIG];       /* path currently loaded       */
static time_t g_lut_mtime;
static ino_t g_lut_ino;

static void lut_choose_path(char *out, size_t n)
{
    char active[PATH_BIG];
    FILE *f;

    if (g_force_default) {
        snprintf(out, n, "%s", g_lut_default);
        return;
    }
    snprintf(active, sizeof active, "%s/active", g_lut_dir);
    if ((f = fopen(active, "r"))) {
        char line[512];
        if (fgets(line, sizeof line, f)) {
            char *p = trim(line);
            size_t l = strlen(p);
            while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' '))
                p[--l] = 0;
            if (*p) {
                if (*p == '/') snprintf(out, n, "%s", p);
                else           snprintf(out, n, "%s/%s", g_lut_dir, p);
                fclose(f);
                return;
            }
        }
        fclose(f);
    }
    snprintf(out, n, "%s", g_lut_default);
}

/* set default path + the directory where "active" lives */
static void lut_set_paths(const char *lutpath)
{
    char *slash;

    snprintf(g_lut_default, sizeof g_lut_default, "%s", lutpath);
    snprintf(g_lut_dir, sizeof g_lut_dir, "%s", lutpath);
    slash = strrchr(g_lut_dir, '/');
    if (slash) *slash = 0;
    else       snprintf(g_lut_dir, sizeof g_lut_dir, ".");
}

/* 在屏幕上弹一条状态条（异步，失败就算了） */
static int g_overlay_on = 0;
static int g_strength = 100;   /* LUT 混合强度，100 = 完全套用 */

static void overlay(const char *text, int secs, int r, int g, int b)
{
    char bin[PATH_BIG], col[32], sec[8];
    char *argv[6];
    pid_t pid;

    if (!g_overlay_on) return;
    snprintf(bin, sizeof bin, "%s/lutshow-hf.arm", g_lut_dir);
    if (access(bin, X_OK) != 0) {
        snprintf(bin, sizeof bin, "%s/lutshow.arm", g_lut_dir);
        if (access(bin, X_OK) != 0) return;
    }
    snprintf(col, sizeof col, "%d,%d,%d", r, g, b);
    snprintf(sec, sizeof sec, "%d", secs);
    argv[0] = bin;
    argv[1] = (char *)text;
    argv[2] = sec;
    argv[3] = (char *)"--color";
    argv[4] = col;
    argv[5] = NULL;

    pid = fork();
    if (pid == 0) {
        execv(bin, argv);
        _exit(127);
    }
}

/* LED 状态：处理中常亮，完成熄灭，失败闪三下 */
static char g_led[192] = "";

static void led_set(int on)
{
    FILE *f;

    if (!g_led[0]) return;
    if (!(f = fopen(g_led, "w"))) return;
    fputc(on ? '1' : '0', f);
    fclose(f);
}

static void led_blink(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        led_set(1);
        usleep(150000);
        led_set(0);
        usleep(150000);
    }
}

/* reload only when the chosen file changed; cheap (one stat per photo) */
static int lut_maybe_reload(void)
{
    char path[PATH_BIG];
    struct stat st;
    Lut tmp;

    lut_choose_path(path, sizeof path);
    if (stat(path, &st) != 0) return -1;
    if (g_lut.n3 && st.st_mtime == g_lut_mtime && st.st_ino == g_lut_ino) return 0;

    memset(&tmp, 0, sizeof tmp);
    if (lut_load(path, &tmp) != 0) return -1;
    lut_prepare(&tmp);

    free(g_lut.t1);
    free(g_lut.t3);
    g_lut = tmp;
    g_lut_mtime = st.st_mtime;
    g_lut_ino = st.st_ino;
    snprintf(g_lut_cur, sizeof g_lut_cur, "%s", path);
    logmsg("lut: using %s (3D %d^3, 1D %d)", path, g_lut.n3, g_lut.n1);
    return 0;
}

/* 先试 active 指定的 LUT；不行就退回 conf 里的默认 LUT；都不行才失败 */
static int lut_init(void)
{
    g_force_default = 0;
    g_lut_mtime = 0;
    g_lut_ino = 0;
    if (lut_maybe_reload() == 0) return 0;

    g_force_default = 1;
    g_lut_mtime = 0;
    g_lut_ino = 0;
    logmsg("lut: active LUT unusable, falling back to %s", g_lut_default);
    if (lut_maybe_reload() == 0) return 0;

    logmsg("lut: no usable LUT at all");
    return -1;
}

/* ------------------------------------------------------------------ */
/* 相机按键：热切换 LUT                                                */
/* 只读 /dev/input/eventN，不 grab、不吞事件，相机自己照常收到按键。    */
/* ------------------------------------------------------------------ */

static int g_in_fd[8];
static int g_in_n = 0;
static int g_hotkey = 0;
static int g_probe_keys = 0;

static void input_open(void)
{
    int i;

    for (i = 0; i < 8 && g_in_n < 8; i++) {
        char p[64];
        int fd;
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) g_in_fd[g_in_n++] = fd;
    }
    logmsg("input: opened %d event device(s), hotkey=%d, probe=%d",
           g_in_n, g_hotkey, g_probe_keys);
}

/* 切到目录里下一个 .cube，并写进 active 保存选择 */
static void hotkey_next(void)
{
    DIR *d;
    struct dirent *e;
    char names[32][256];
    int n = 0, i, j, cur = -1, next;
    const char *base;

    if (!(d = opendir(g_lut_dir))) return;
    while ((e = readdir(d)) && n < 32) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l > 200) continue;
        if (strcasecmp(e->d_name + l - 5, ".cube")) continue;
        snprintf(names[n], sizeof names[n], "%s", e->d_name);
        n++;
    }
    closedir(d);
    if (n < 2) {
        logmsg("hotkey: need at least 2 .cube files in %s", g_lut_dir);
        led_blink(1);
        return;
    }

    for (i = 1; i < n; i++) {                 /* 简单插入排序 */
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", names[i]);
        for (j = i - 1; j >= 0 && strcasecmp(names[j], tmp) > 0; j--)
            snprintf(names[j + 1], sizeof names[j + 1], "%s", names[j]);
        snprintf(names[j + 1], sizeof names[j + 1], "%s", tmp);
    }

    base = strrchr(g_lut_cur, '/');
    base = base ? base + 1 : g_lut_cur;
    for (i = 0; i < n; i++)
        if (!strcasecmp(names[i], base)) { cur = i; break; }

    next = (cur + 1) % n;

    {
        char ap[PATH_BIG];
        FILE *f;
        snprintf(ap, sizeof ap, "%s/active", g_lut_dir);
        if ((f = fopen(ap, "w"))) {
            fprintf(f, "%s\n", names[next]);
            fclose(f);
        }
    }
    lut_maybe_reload();
    logmsg("hotkey: %s (%d/%d)", names[next], next + 1, n);
    led_blink(next + 1);                      /* 闪 N 下 = 第 N 个 */
}

static void input_handle(void)
{
    int i;

    for (i = 0; i < g_in_n; i++) {
        uint8_t buf[512];
        ssize_t len;

        while ((len = read(g_in_fd[i], buf, sizeof buf)) > 0) {
            size_t off = 0;
            while (off + sizeof(struct input_event) <= (size_t)len) {
                struct input_event ev;
                memcpy(&ev, buf + off, sizeof ev);
                off += sizeof ev;
                if (ev.type != EV_KEY || ev.value != 1) continue;
                if (g_probe_keys) logmsg("key: code=%d", ev.code);
                if (g_hotkey && ev.code == g_hotkey) hotkey_next();
            }
        }
    }
}

static void apply_row(const Lut *L, unsigned char *row, int w)
{
    const int n = L->n3;
    const unsigned short *t = L->t3;
    int x;

    for (x = 0; x < w; x++) {
        unsigned char *p = row + (size_t)x * 3;
        int r = p[0], g = p[1], b = p[2];
        int ri = L->idx[0][r], rf = L->fr[0][r];
        int gi = L->idx[1][g], gf = L->fr[1][g];
        int bi = L->idx[2][b], bf = L->fr[2][b];
        int o000 = ((bi * n + gi) * n + ri) * 3;
        int o100 = o000 + 3;
        int o010 = ((bi * n + gi + 1) * n + ri) * 3;
        int o110 = o010 + 3;
        int o001 = (((bi + 1) * n + gi) * n + ri) * 3;
        int o101 = o001 + 3;
        int o011 = (((bi + 1) * n + gi + 1) * n + ri) * 3;
        int o111 = o011 + 3;
        int c;

        for (c = 0; c < 3; c++) {
            int v0 = lerp8(t[o000 + c], t[o100 + c], rf);
            int v1 = lerp8(t[o010 + c], t[o110 + c], rf);
            int v2 = lerp8(t[o001 + c], t[o101 + c], rf);
            int v3 = lerp8(t[o011 + c], t[o111 + c], rf);
            int v4 = lerp8(v0, v1, gf);
            int v5 = lerp8(v2, v3, gf);
            int v = lerp8(v4, v5, bf);
            int o = (v * 255 + 32768) >> 16;
            if (o < 0) o = 0;
            if (o > 255) o = 255;
            if (g_strength < 100) {          /* 和原色按比例混合 */
                int orig = p[c];
                o = orig + ((o - orig) * g_strength + 50) / 100;
                if (o < 0) o = 0;
                if (o > 255) o = 255;
            }
            p[c] = (unsigned char)o;
        }
    }
}

/* ------------------------------------------------------------------ */
/* minimal EXIF we can synthesize for previews that ship without one   */

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void put_entry(unsigned char *p, unsigned tag, unsigned type,
                      unsigned long count, unsigned long val)
{
    put16(p, tag);
    put16(p + 2, type);
    put32(p + 4, count);
    put32(p + 8, val);
}

/*
 * Fixed layout, little endian, offsets are relative to the TIFF header
 * (which starts 6 bytes into the payload, after "Exif\0\0"):
 *
 *   0  header              8
 *   8  IFD0 (5 entries)   66   -> data at 74
 *  74  Make                8
 *  82  Model               6
 *  88  DateTime           20
 * 108  ExifIFD (1 entry)  18   -> data at 126
 * 126  DateTimeOriginal   20
 *                         ---
 *                         146 bytes, payload 152, APP1 length 154
 */
#define APP1_PAYLOAD 152

static int build_app1(unsigned char *p, int orientation, const char *dt)
{
    static const char make[]  = "SAMSUNG";
    static const char model[] = "NX300";
    char stamp[20];
    int n;

    memcpy(stamp, dt, 19);
    stamp[19] = 0;

    memcpy(p, "Exif\0\0", 6);
    p[6] = 'I'; p[7] = 'I';
    put16(p + 8, 42);
    put32(p + 10, 8);
    n = 14;

    put16(p + n, 5); n += 2;
    put_entry(p + n, 0x010F, 2, sizeof make,  74); n += 12;
    put_entry(p + n, 0x0110, 2, sizeof model, 82); n += 12;
    put_entry(p + n, 0x0112, 3, 1, (unsigned long)orientation); n += 12;
    put_entry(p + n, 0x0132, 2, 20, 88);  n += 12;
    put_entry(p + n, 0x8769, 4, 1, 108);  n += 12;
    put32(p + n, 0); n += 4;

    memcpy(p + n, make, sizeof make);   n += (int)sizeof make;
    memcpy(p + n, model, sizeof model); n += (int)sizeof model;
    memcpy(p + n, stamp, 20);           n += 20;

    put16(p + n, 1); n += 2;
    put_entry(p + n, 0x9003, 2, 20, 126); n += 12;
    put32(p + n, 0); n += 4;

    memcpy(p + n, stamp, 20); n += 20;
    return n;
}

/* ------------------------------------------------------------------ */
/* JPEG                                                                */

/* how closely to mimic the camera's own JPEG container */
typedef struct {
    int quality;
    int copy_app1;    /* keep EXIF                                       */
    int copy_app2;    /* keep APP2 (Samsung/MPF/ICC)                     */
    int no_jfif;      /* do not emit the JFIF APP0 header                */
    int copy_dri;     /* reuse the source's restart interval             */
    int regrade_thumb;/* re-LUT the EXIF thumbnail in place              */
} Enc;

/* ------------------------------------------------------------------ */
/* EXIF thumbnail: regrade the tiny embedded JPEG in place, so Windows  */
/* Explorer and the camera index show the graded colours too.           */
/* We never move anything: the new thumbnail must fit into the old      */
/* slot, so all EXIF offsets stay valid.                                */
/* ------------------------------------------------------------------ */

static jmp_buf g_tjb;

static void thumb_error_exit(j_common_ptr ci) { (void)ci; longjmp(g_tjb, 1); }
static void thumb_silent(j_common_ptr ci, int level) { (void)ci; (void)level; }

static unsigned t16(const unsigned char *p, int le)
{
    return le ? (unsigned)(p[0] | (p[1] << 8)) : (unsigned)((p[0] << 8) | p[1]);
}

static unsigned long t32(const unsigned char *p, int le)
{
    if (le)
        return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
               ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void tw16(unsigned char *p, unsigned v, int le)
{
    if (le) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
    else    { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
}

static void tw32(unsigned char *p, unsigned long v, int le)
{
    if (le) {
        p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
        p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
    } else {
        p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
        p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
    }
}

static int thumb_regrade(unsigned char *app1, int app1_len, const Lut *L)
{
    struct jpeg_decompress_struct d;
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr jd, jc;
    unsigned char *rgb = NULL;
    unsigned char *jpg = NULL;
    unsigned long jlen = 0;
    volatile int dok = 0, cok = 0, rc = -1;
    unsigned char *tiff, *thumb;
    unsigned long ifd0, n0, ifd1, n1, toff = 0, tlen = 0, i;
    int le, w, h, y;

    if (app1_len < 24 || memcmp(app1, "Exif\0\0", 6) != 0) return -1;
    tiff = app1 + 6;
    le = (tiff[0] == 'I');

    ifd0 = t32(tiff + 4, le);
    if (ifd0 + 2 > (unsigned long)(app1_len - 6)) return -1;
    n0 = t16(tiff + ifd0, le);
    if (ifd0 + 2 + n0 * 12 + 4 > (unsigned long)(app1_len - 6)) return -1;
    ifd1 = t32(tiff + ifd0 + 2 + n0 * 12, le);
    if (!ifd1 || ifd1 + 2 > (unsigned long)(app1_len - 6)) return -1;
    n1 = t16(tiff + ifd1, le);
    if (ifd1 + 2 + n1 * 12 > (unsigned long)(app1_len - 6)) return -1;

    for (i = 0; i < n1; i++) {
        unsigned long e = ifd1 + 2 + i * 12;
        unsigned tag = t16(tiff + e, le);
        unsigned typ = t16(tiff + e + 2, le);
        if (tag == 0x0201 && (typ == 4 || typ == 3))
            toff = (typ == 3) ? t16(tiff + e + 8, le) : t32(tiff + e + 8, le);
        if (tag == 0x0202 && (typ == 4 || typ == 3))
            tlen = (typ == 3) ? t16(tiff + e + 8, le) : t32(tiff + e + 8, le);
    }
    if (!toff || tlen < 200) return -1;
    if (toff + tlen > (unsigned long)(app1_len - 6)) return -1;
    thumb = tiff + toff;
    if (thumb[0] != 0xFF || thumb[1] != 0xD8) return -1;

    memset(&d, 0, sizeof d);
    memset(&c, 0, sizeof c);
    d.err = jpeg_std_error(&jd);
    jd.error_exit = thumb_error_exit;
    jd.emit_message = thumb_silent;
    c.err = jpeg_std_error(&jc);
    jc.error_exit = thumb_error_exit;
    jc.emit_message = thumb_silent;

    if (setjmp(g_tjb)) { rc = -3; goto done; }

    jpeg_create_decompress(&d);
    dok = 1;
    jpeg_mem_src(&d, thumb, tlen);
    jpeg_read_header(&d, TRUE);
    d.out_color_space = JCS_RGB;
    jpeg_start_decompress(&d);

    w = (int)d.output_width;
    h = (int)d.output_height;
    if (w < 8 || h < 8 || w > 1200 || h > 1200) { rc = -2; goto done; }
    if (!(rgb = malloc((size_t)w * (size_t)h * 3))) { rc = -1; goto done; }

    for (y = 0; y < h; y++) {
        JSAMPROW rp = rgb + (size_t)y * (size_t)w * 3;
        jpeg_read_scanlines(&d, &rp, 1);
        apply_row(L, rp, w);
    }
    jpeg_finish_decompress(&d);

    jpeg_create_compress(&c);
    cok = 1;
    jpeg_mem_dest(&c, &jpg, &jlen);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_colorspace(&c, JCS_YCbCr);
    jpeg_set_quality(&c, 78, TRUE);
    c.write_JFIF_header = FALSE;
    c.write_Adobe_marker = FALSE;
    jpeg_start_compress(&c, TRUE);
    for (y = 0; y < h; y++) {
        JSAMPROW wp = rgb + (size_t)y * (size_t)w * 3;
        jpeg_write_scanlines(&c, &wp, 1);
    }
    jpeg_finish_compress(&c);

    if (!jpg || jlen == 0 || jlen > tlen) { rc = -2; goto done; }

    memcpy(thumb, jpg, jlen);
    memset(thumb + jlen, 0, (size_t)(tlen - jlen));

    for (i = 0; i < n1; i++) {
        unsigned long e = ifd1 + 2 + i * 12;
        unsigned tag = t16(tiff + e, le);
        unsigned typ = t16(tiff + e + 2, le);
        if (tag != 0x0202) continue;
        if (typ == 3) tw16(tiff + e + 8, (unsigned)jlen, le);
        else          tw32(tiff + e + 8, jlen, le);
    }
    rc = 0;

done:
    if (cok) jpeg_destroy_compress(&c);
    if (dok) jpeg_destroy_decompress(&d);
    free(rgb);
    free(jpg);
    return rc;
}

static int apply_jpeg(const char *in, const char *out, const Lut *L,
                      const Enc *e, int orientation, const char *dt)
{
    struct jpeg_decompress_struct d;
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr jd, jc;
    FILE *volatile fi = NULL, *volatile fo = NULL;
    unsigned char *volatile row = NULL;
    volatile int dok = 0, cok = 0, rc = -1;
    volatile int w = 0;

    memset(&d, 0, sizeof d);
    memset(&c, 0, sizeof c);

    if (!(fi = fopen(in, "rb"))) { logmsg("open %s: %s", in, strerror(errno)); return -1; }

    d.err = jpeg_std_error(&jd);
    jd.error_exit = jpeg_error_exit;
    jd.emit_message = jpeg_emit_message;
    c.err = jpeg_std_error(&jc);
    jc.error_exit = jpeg_error_exit;
    jc.emit_message = jpeg_emit_message;

    if (setjmp(g_jb)) { rc = -3; goto done; }

    jpeg_create_decompress(&d);
    dok = 1;
    jpeg_stdio_src(&d, fi);
    if (e->copy_app1) jpeg_save_markers(&d, JPEG_APP0 + 1, 0xFFFF);   /* EXIF */
    if (e->copy_app2) jpeg_save_markers(&d, JPEG_APP0 + 2, 0xFFFF);   /* APP2 */
    jpeg_read_header(&d, TRUE);

    if (d.progressive_mode) { logmsg("skip progressive jpeg: %s", in); rc = -2; goto done; }
    if (d.image_width < 16 || d.image_height < 16) { rc = -2; goto done; }

    d.out_color_space = JCS_RGB;
    d.dct_method = JDCT_IFAST;      /* speed over the last bit of accuracy */
    jpeg_start_decompress(&d);

    w = (int)d.output_width;

    if (!(fo = fopen(out, "wb"))) { logmsg("create %s: %s", out, strerror(errno)); rc = -1; goto done; }

    jpeg_create_compress(&c);
    cok = 1;
    jpeg_stdio_dest(&c, fo);
    c.image_width = d.output_width;
    c.image_height = d.output_height;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_colorspace(&c, JCS_YCbCr);
    if (d.num_components >= 3) {           /* keep the source's chroma subsampling */
        int i;
        for (i = 0; i < 3; i++) {
            c.comp_info[i].h_samp_factor = d.comp_info[i].h_samp_factor;
            c.comp_info[i].v_samp_factor = d.comp_info[i].v_samp_factor;
        }
    }
    jpeg_set_quality(&c, e->quality, TRUE);
    c.dct_method = JDCT_IFAST;      /* shorter write window = safer on power loss */
    if (e->no_jfif) {               /* camera files start with APP1, no JFIF APP0 */
        c.write_JFIF_header = FALSE;
        c.write_Adobe_marker = FALSE;
    }
    if (e->copy_dri && d.restart_interval)
        c.restart_interval = d.restart_interval;   /* Samsung decoders expect RSTn */
    jpeg_start_compress(&c, TRUE);

    if (e->copy_app1 || e->copy_app2) {
        jpeg_saved_marker_ptr m;
        for (m = d.marker_list; m; m = m->next) {
            if (e->regrade_thumb && m->marker == JPEG_APP0 + 1 && m->data_length > 64) {
                unsigned char *buf = malloc(m->data_length);
                if (buf) {
                    memcpy(buf, m->data, m->data_length);
                    if (thumb_regrade(buf, (int)m->data_length, L) == 0)
                        logmsg("exif thumbnail regraded");
                    jpeg_write_marker(&c, m->marker, buf, m->data_length);
                    free(buf);
                    continue;
                }
            }
            jpeg_write_marker(&c, m->marker, m->data, m->data_length);
        }
    }

    if (orientation >= 1 && orientation <= 8) {
        unsigned char app1[APP1_PAYLOAD];
        int len = build_app1(app1, orientation, dt ? dt : "2000:01:01 00:00:00");
        jpeg_write_marker(&c, JPEG_APP0 + 1, app1, (unsigned int)len);
    }

    if (!(row = malloc((size_t)w * 3))) { logmsg("oom"); rc = -1; goto done; }

    while (d.output_scanline < d.output_height) {
        JSAMPROW rp = row;
        JSAMPROW wp = row;
        jpeg_read_scanlines(&d, &rp, 1);
        apply_row(L, row, w);
        jpeg_write_scanlines(&c, &wp, 1);
    }

    jpeg_finish_compress(&c);
    fflush(fo);
    fsync(fileno(fo));
    jpeg_finish_decompress(&d);
    rc = 0;

done:
    free(row);
    if (cok) jpeg_destroy_compress(&c);
    if (dok) jpeg_destroy_decompress(&d);
    if (fo) fclose(fo);
    if (fi) fclose(fi);
    return rc;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static int is_jpeg_name(const char *n)
{
    size_t l = strlen(n);
    if (l < 5 || n[0] == '.') return 0;
    return !strcasecmp(n + l - 4, ".jpg") || !strcasecmp(n + l - 4, ".jpeg");
}

static int has_eoi(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char t[2];
    int ok = 0;

    if (!f) return 0;
    if (fseek(f, -2, SEEK_END) == 0 && fread(t, 1, 2, f) == 2)
        ok = (t[0] == 0xFF && t[1] == 0xD9);
    fclose(f);
    return ok;
}

static void mkdir_p(const char *path)
{
    char tmp[PATH_MAX2];
    char *p;

    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    }
    mkdir(tmp, 0777);
}

/* remove leftovers of an interrupted run (.lutd-*.tmp / .lutd-*.preview.jpg) */
static void cleanup_stale(const char *dir, int depth)
{
    DIR *d;
    struct dirent *e;
    char sub[PATH_BIG];

    if (depth > 4 || !(d = opendir(dir))) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            if (!strncmp(e->d_name, ".lutd-", 6)) {
                snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
                if (unlink(sub) == 0) logmsg("cleanup stale %s", sub);
            }
            continue;
        }
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        {
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) cleanup_stale(sub, depth + 1);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* config                                                              */

typedef struct {
    char watch[PATH_MAX2];
    char lut[PATH_MAX2];
    char log[PATH_MAX2];
    char mode[16];
    char suffix[32];
    char origdir[PATH_MAX2];
    int keep_orig;
    int copy_app1;
    int copy_app2;
    int no_jfif;
    int copy_dri;
    int thumb;
    int overlay;
    char led[192];
    int hotkey;
    int probe_keys;
    int strength;
    int srw;
    int quality;
    long min_bytes;
} Cfg;

static void enc_from_cfg(const Cfg *cfg, Enc *e)
{
    e->quality   = cfg->quality;
    e->copy_app1 = cfg->copy_app1;
    e->copy_app2 = cfg->copy_app2;
    e->no_jfif   = cfg->no_jfif;
    e->copy_dri  = cfg->copy_dri;
    e->regrade_thumb = cfg->thumb;
}

static void cfg_defaults(Cfg *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->watch, sizeof c->watch, "/mnt/mmc");
    snprintf(c->lut, sizeof c->lut, "/mnt/mmc/lut/look.cube");
    snprintf(c->log, sizeof c->log, "/mnt/mmc/lutd.log");
    snprintf(c->mode, sizeof c->mode, "replace");
    snprintf(c->suffix, sizeof c->suffix, "_LUT");
    snprintf(c->origdir, sizeof c->origdir, "/mnt/mmc/lut/orig");
    c->keep_orig = 1;
    c->copy_app1 = 1;
    c->copy_app2 = 1;
    c->no_jfif = 1;
    c->copy_dri = 1;
    c->thumb = 1;
    c->overlay = 1;
    snprintf(c->led, sizeof c->led, "%s", "/sys/class/leds/card/brightness");
    c->hotkey = 0;
    c->probe_keys = 0;
    c->strength = 100;
    c->srw = 1;
    c->quality = 92;
    c->min_bytes = 100000;
}

static void cfg_load(Cfg *c, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];

    if (!f) { logmsg("no config %s, using defaults", path); return; }

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        char *eq;
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        trim(p);
        eq = trim(eq + 1);

        if (!strcasecmp(p, "WATCH")) snprintf(c->watch, sizeof c->watch, "%s", eq);
        else if (!strcasecmp(p, "LUT")) snprintf(c->lut, sizeof c->lut, "%s", eq);
        else if (!strcasecmp(p, "LOG")) snprintf(c->log, sizeof c->log, "%s", eq);
        else if (!strcasecmp(p, "MODE")) snprintf(c->mode, sizeof c->mode, "%s", eq);
        else if (!strcasecmp(p, "SUFFIX")) snprintf(c->suffix, sizeof c->suffix, "%s", eq);
        else if (!strcasecmp(p, "ORIG_DIR")) snprintf(c->origdir, sizeof c->origdir, "%s", eq);
        else if (!strcasecmp(p, "KEEP_ORIG")) c->keep_orig = atoi(eq) != 0;
        else if (!strcasecmp(p, "KEEP_META")) {
            int v = atoi(eq) != 0;
            c->copy_app1 = v;
            c->copy_app2 = v;
        }
        else if (!strcasecmp(p, "COPY_APP1")) c->copy_app1 = atoi(eq) != 0;
        else if (!strcasecmp(p, "COPY_APP2")) c->copy_app2 = atoi(eq) != 0;
        else if (!strcasecmp(p, "NO_JFIF")) c->no_jfif = atoi(eq) != 0;
        else if (!strcasecmp(p, "COPY_DRI")) c->copy_dri = atoi(eq) != 0;
        else if (!strcasecmp(p, "THUMB")) c->thumb = atoi(eq) != 0;
        else if (!strcasecmp(p, "OVERLAY")) c->overlay = atoi(eq) != 0;
        else if (!strcasecmp(p, "LED")) snprintf(c->led, sizeof c->led, "%s", eq);
        else if (!strcasecmp(p, "HOTKEY")) c->hotkey = atoi(eq);
        else if (!strcasecmp(p, "PROBE_KEYS")) c->probe_keys = atoi(eq) != 0;
        else if (!strcasecmp(p, "LUT_STRENGTH")) {
            c->strength = atoi(eq);
            if (c->strength < 0) c->strength = 0;
            if (c->strength > 100) c->strength = 100;
        }
        else if (!strcasecmp(p, "SRW")) c->srw = atoi(eq) != 0;
        else if (!strcasecmp(p, "QUALITY")) c->quality = atoi(eq);
        else if (!strcasecmp(p, "MIN_BYTES")) c->min_bytes = atol(eq);
        else logmsg("config: unknown key %s", p);
    }
    fclose(f);

    if (c->quality < 10) c->quality = 10;
    if (c->quality > 100) c->quality = 100;
}

/* ------------------------------------------------------------------ */
/* guard: rename() fires IN_MOVED_TO for the target, which would make  */
/* us re-process our own output forever. Remember what we just wrote.  */

#define GUARD_MAX 8
static struct { char path[PATH_MAX2]; time_t t; } g_guard[GUARD_MAX];

static void guard_add(const char *p)
{
    int i, slot = 0;
    time_t now = time(NULL);

    for (i = 0; i < GUARD_MAX; i++) {
        if (!g_guard[i].path[0]) { slot = i; break; }
        if (g_guard[i].t < g_guard[slot].t) slot = i;
    }
    snprintf(g_guard[slot].path, sizeof g_guard[slot].path, "%s", p);
    g_guard[slot].t = now;
}

static int guard_hit(const char *p)
{
    int i;
    time_t now = time(NULL);

    for (i = 0; i < GUARD_MAX; i++) {
        if (g_guard[i].path[0] && !strcmp(g_guard[i].path, p) && now - g_guard[i].t < 120) {
            g_guard[i].path[0] = 0;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* one file                                                            */

static void handle_file(const Cfg *cfg, const char *dir, const char *name)
{
    char in[PATH_BIG], tmp[PATH_BIG], out[PATH_BIG], orig[PATH_BIG];
    struct stat st;
    int rc;

    if (!is_jpeg_name(name)) return;

    /* in suffix mode our own output ends in .JPG too - do not eat it again */
    if (!strcasecmp(cfg->mode, "suffix")) {
        size_t l = strlen(name), s = strlen(cfg->suffix);
        if (l > s + 4 && !strncasecmp(name + l - 4 - s, cfg->suffix, s)) return;
    }

    snprintf(in, sizeof in, "%s/%s", dir, name);
    if (cfg->origdir[0] && strstr(in, cfg->origdir)) return;   /* never touch our own backups */
    if (guard_hit(in)) return;
    if (stat(in, &st) != 0) return;
    if (st.st_size < cfg->min_bytes) return;
    if (!has_eoi(in)) { logmsg("not complete yet, skip: %s", in); return; }

    snprintf(tmp, sizeof tmp, "%s/.lutd-%s.tmp", dir, name);

    if (!strcasecmp(cfg->mode, "suffix")) {
        size_t l = strlen(name);
        snprintf(out, sizeof out, "%s/%.*s%s.JPG", dir, (int)(l - 4), name, cfg->suffix);
    } else {
        snprintf(out, sizeof out, "%s", tmp);
    }

    logmsg("apply %s (%ld bytes) -> %s", in, (long)st.st_size, out);
    overlay("PROCESSING", 30, 210, 150, 30);
    led_set(1);
    {
        Enc e;
        enc_from_cfg(cfg, &e);
        lut_maybe_reload();
        rc = apply_jpeg(in, out, &g_lut, &e, 0, NULL);
    }
    if (rc != 0) {
        logmsg("failed rc=%d: %s", rc, in);
        overlay("FAILED", 5, 200, 50, 50);
        led_blink(3);
        unlink(tmp);
        return;
    }

    if (!strcasecmp(cfg->mode, "suffix")) {
        guard_add(out);
        logmsg("wrote %s", out);
        return;
    }

    if (cfg->keep_orig) {
        mkdir_p(cfg->origdir);
        snprintf(orig, sizeof orig, "%s/%s", cfg->origdir, name);
        if (rename(in, orig) != 0) {
            logmsg("cannot stash original %s: %s", orig, strerror(errno));
            unlink(tmp);
            return;
        }
        guard_add(orig);
        if (rename(tmp, in) != 0) {
            logmsg("cannot replace %s: %s (restoring)", in, strerror(errno));
            rename(orig, in);
            unlink(tmp);
            return;
        }
    } else {
        if (rename(tmp, in) != 0) {
            logmsg("cannot replace %s: %s", in, strerror(errno));
            unlink(tmp);
            return;
        }
    }
    guard_add(in);
    overlay("DONE", 3, 40, 190, 90);
    led_set(0);
    logmsg("done %s", in);
}

/* ------------------------------------------------------------------ */
/* Samsung SRW: the file already carries a full-size JPEG, pull it out */

static int is_srw_name(const char *n)
{
    size_t l = strlen(n);
    if (l < 5 || n[0] == '.') return 0;
    return !strcasecmp(n + l - 4, ".srw");
}

static unsigned long rd16(const unsigned char *p, int le)
{
    return le ? (unsigned long)(p[0] | (p[1] << 8))
              : (unsigned long)((p[0] << 8) | p[1]);
}

static unsigned long rd32(const unsigned char *p, int le)
{
    if (le)
        return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
               ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

#define SRW_HDR_MAX (2u * 1024u * 1024u)

typedef struct {
    long off, len;
    int  orientation;
    char datetime[20];
} SrwInfo;

/* find the biggest JPEGInterchangeFormat(513)/Length(514) pair */
static int srw_probe(const char *path, SrwInfo *si)
{
    FILE *f;
    unsigned char *buf;
    size_t got;
    long fsize;
    int le, guard = 0;
    unsigned long queue[16];
    int qn = 0;
    unsigned long best_off = 0, best_len = 0;
    int orient = 1;
    char dtstr[20];

    dtstr[0] = 0;

    if (!(f = fopen(path, "rb"))) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    fsize = ftell(f);
    if (fsize < 16) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    if (!(buf = malloc(SRW_HDR_MAX))) { fclose(f); return -1; }
    got = fread(buf, 1, SRW_HDR_MAX, f);
    fclose(f);
    if (got < 16) { free(buf); return -1; }

    if (buf[0] == 'I' && buf[1] == 'I') le = 1;
    else if (buf[0] == 'M' && buf[1] == 'M') le = 0;
    else { free(buf); return -1; }

    queue[qn++] = rd32(buf + 4, le);

    while (qn > 0 && guard++ < 64) {
        unsigned long o = queue[--qn];
        unsigned long n, i, jif = 0, jlen = 0;

        if (o + 2 > got) continue;
        n = rd16(buf + o, le);
        if (n > 512 || o + 2 + n * 12 + 4 > got) continue;

        for (i = 0; i < n; i++) {
            unsigned long e = o + 2 + i * 12;
            unsigned long tag = rd16(buf + e, le);
            unsigned long typ = rd16(buf + e + 2, le);
            unsigned long v = (typ == 3) ? rd16(buf + e + 8, le) : rd32(buf + e + 8, le);

            if (tag == 513) jif = v;
            else if (tag == 514) jlen = v;
            else if (tag == 330 || tag == 34665) {
                if (v && qn < 16) queue[qn++] = v;
            }
            else if (tag == 274) {
                if (orient == 1) orient = (int)rd16(buf + e + 8, le);
            }
            else if ((tag == 306 || tag == 36867) && !dtstr[0] && v + 19 <= got) {
                memcpy(dtstr, buf + v, 19);
                dtstr[19] = 0;
            }
        }
        if (jif && jlen && jlen > best_len && jif + 4 < (unsigned long)fsize) {
            best_off = jif;
            best_len = jlen;
        }
        {
            unsigned long next = rd32(buf + o + 2 + n * 12, le);
            if (next && qn < 16) queue[qn++] = next;
        }
    }

    if (best_off && best_off + 2 <= got &&
        (buf[best_off] != 0xFF || buf[best_off + 1] != 0xD8))
        best_off = 0;
    free(buf);

    if (!best_off || best_off + 2 > (unsigned long)fsize) return -1;
    if (best_off + best_len > (unsigned long)fsize) best_len = (unsigned long)fsize - best_off;
    si->off = (long)best_off;
    si->len = (long)best_len;
    si->orientation = (orient >= 1 && orient <= 8) ? orient : 1;
    memcpy(si->datetime, dtstr, sizeof si->datetime);
    return 0;
}

static int srw_extract(const char *path, long off, long len, const char *out)
{
    FILE *fi, *fo;
    unsigned char buf[65536];
    long left = len;

    if (!(fi = fopen(path, "rb"))) return -1;
    if (!(fo = fopen(out, "wb"))) { fclose(fi); return -1; }
    fseek(fi, off, SEEK_SET);

    while (left > 0) {
        size_t want = left > (long)sizeof buf ? sizeof buf : (size_t)left;
        size_t n = fread(buf, 1, want, fi);
        if (n == 0) break;
        if (fwrite(buf, 1, n, fo) != n) break;
        left -= (long)n;
    }
    fclose(fi);
    fflush(fo);
    fsync(fileno(fo));
    fclose(fo);
    return left == 0 ? 0 : -1;
}

static void handle_srw(const Cfg *cfg, const char *dir, const char *name)
{
    char in[PATH_BIG], sib[PATH_BIG], tmp[PATH_BIG], tmp2[PATH_BIG], out[PATH_BIG];
    char base[256];
    struct stat st;
    SrwInfo si;
    size_t l = strlen(name);
    int rc;

    snprintf(in, sizeof in, "%s/%s", dir, name);
    if (guard_hit(in)) return;

    snprintf(base, sizeof base, "%.*s", (int)(l - 4), name);

    /* shooting RAW+JPEG: the camera's own (higher quality) JPEG wins */
    snprintf(sib, sizeof sib, "%s/%s.JPG", dir, base);
    if (stat(sib, &st) == 0) { logmsg("srw: %s has a sibling JPEG, skip", name); return; }
    usleep(1500000);                    /* let the camera finish writing the pair */
    if (stat(sib, &st) == 0) { logmsg("srw: %s has a sibling JPEG, skip", name); return; }
    if (stat(in, &st) != 0) return;

    if (srw_probe(in, &si) != 0) {
        logmsg("srw: no embedded preview in %s", in);
        return;
    }
    if (si.len < 20000) { logmsg("srw: preview too small, skip %s", name); return; }
    logmsg("srw: preview %ld bytes @ %ld, orient %d, date %s",
           si.len, si.off, si.orientation, si.datetime[0] ? si.datetime : "?");

    snprintf(tmp, sizeof tmp, "%s/.lutd-%s.preview.jpg", dir, name);
    if (srw_extract(in, si.off, si.len, tmp) != 0) {
        logmsg("srw: extract failed %s", in);
        unlink(tmp);
        return;
    }

    snprintf(out, sizeof out, "%s/%s.JPG", dir, base);
    snprintf(tmp2, sizeof tmp2, "%s/.lutd-%s.jpg.tmp", dir, base);

    {
        Enc e;
        enc_from_cfg(cfg, &e);
        lut_maybe_reload();
        rc = apply_jpeg(tmp, tmp2, &g_lut, &e,
                        si.orientation, si.datetime[0] ? si.datetime : NULL);
    }
    unlink(tmp);
    if (rc != 0) {
        logmsg("srw: lut failed rc=%d %s", rc, name);
        unlink(tmp2);
        return;
    }
    if (rename(tmp2, out) != 0) {
        logmsg("srw: rename failed: %s", strerror(errno));
        unlink(tmp2);
        return;
    }
    guard_add(out);
    logmsg("srw: wrote %s", out);
}

/* ------------------------------------------------------------------ */
/* inotify daemon                                                      */

#define MAX_WD 128
static struct { int wd; char path[PATH_MAX2]; } g_wd[MAX_WD];
static int g_nwd;

static void wd_add(int fd, const char *path)
{
    int wd;
    if (g_nwd >= MAX_WD) return;
    wd = inotify_add_watch(fd, path, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_ONLYDIR);
    if (wd < 0) return;
    g_wd[g_nwd].wd = wd;
    snprintf(g_wd[g_nwd].path, sizeof g_wd[g_nwd].path, "%s", path);
    g_nwd++;
}

static const char *wd_path(int wd)
{
    int i;
    for (i = 0; i < g_nwd; i++)
        if (g_wd[i].wd == wd) return g_wd[i].path;
    return NULL;
}

static void watch_tree(int fd, const char *dir, int depth)
{
    DIR *d;
    struct dirent *e;
    char sub[PATH_BIG];

    if (depth > 4) return;
    wd_add(fd, dir);
    if (!(d = opendir(dir))) return;
    while ((e = readdir(d))) {
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) watch_tree(fd, sub, depth + 1);
    }
    closedir(d);
}

static int daemon_run(Cfg *cfg)
{
    int fd;
    struct timespec ts;

    lut_set_paths(cfg->lut);
    g_overlay_on = cfg->overlay;
    snprintf(g_led, sizeof g_led, "%s", cfg->led);
    g_hotkey = cfg->hotkey;
    g_probe_keys = cfg->probe_keys;
    g_strength = cfg->strength;
    input_open();
    if (lut_init() != 0) {
        logmsg("cannot load any LUT (tried %s/active and %s)", g_lut_dir, cfg->lut);
        return 1;
    }

    if (cfg->keep_orig) mkdir_p(cfg->origdir);

    if ((fd = inotify_init1(IN_CLOEXEC)) < 0) { logmsg("inotify: %s", strerror(errno)); return 1; }
    cleanup_stale(cfg->watch, 0);
    watch_tree(fd, cfg->watch, 0);
    logmsg("watching %s (%d dirs), mode=%s, quality=%d, keep_orig=%d, srw=%d",
           cfg->watch, g_nwd, cfg->mode, cfg->quality, cfg->keep_orig, cfg->srw);

    for (;;) {
        fd_set rf;
        int n, i, maxfd = fd;
        char buf[8192];
        ssize_t len, off;

        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        for (i = 0; i < g_in_n; i++) {
            FD_SET(g_in_fd[i], &rf);
            if (g_in_fd[i] > maxfd) maxfd = g_in_fd[i];
        }
        ts.tv_sec = 2;
        ts.tv_nsec = 0;
        n = pselect(maxfd + 1, &rf, NULL, NULL, &ts, NULL);
        if (n <= 0) continue;

        if (FD_ISSET(fd, &rf)) {
            len = read(fd, buf, sizeof buf);
        } else {
            len = -1;
        }
        input_handle();
        if (len <= 0) continue;

        for (off = 0; off < len; ) {
            struct inotify_event *ev = (struct inotify_event *)(buf + off);
            const char *dir = wd_path(ev->wd);

            if (dir && ev->len > 0 && ev->name[0]) {
                char sub[PATH_BIG];
                if (ev->mask & IN_ISDIR) {
                    if (ev->mask & IN_CREATE) {
                        snprintf(sub, sizeof sub, "%s/%s", dir, ev->name);
                        watch_tree(fd, sub, 1);
                    }
                } else if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
                    if (is_jpeg_name(ev->name))
                        handle_file(cfg, dir, ev->name);
                    else if (cfg->srw && is_srw_name(ev->name))
                        handle_srw(cfg, dir, ev->name);
                }
            }
            off += (ssize_t)(sizeof(struct inotify_event) + ev->len);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  lutd --once IN.JPG OUT.JPG LOOK.cube [quality]\n"
        "  lutd [CONFIG]\n");
}

int main(int argc, char **argv)
{
    Cfg cfg;

    signal(SIGPIPE, SIG_IGN);

    if (argc >= 5 && !strcmp(argv[1], "--once")) {
        Lut L;
        int q = (argc >= 6) ? atoi(argv[5]) : 95;
        int rc;

        if (q < 10) q = 10;
        if (q > 100) q = 100;
        if (lut_load(argv[4], &L) != 0) return 1;
        lut_prepare(&L);
        {
            Enc e;
            memset(&e, 0, sizeof e);
            e.quality = q;
            e.copy_app1 = 1;
            e.copy_app2 = 1;
            e.regrade_thumb = 1;
            rc = apply_jpeg(argv[2], argv[3], &L, &e, 0, NULL);
        }
        fprintf(stderr, "lutd: %s\n", rc == 0 ? "ok" : "failed");
        return rc == 0 ? 0 : 1;
    }

    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        Cfg c;

        cfg_defaults(&c);
        cfg_load(&c, argc > 2 ? argv[2] : "/mnt/mmc/lut/lutd.conf");
        printf("watch=%s\nlut=%s\nmode=%s\nsrw=%d\n",
               c.watch, c.lut, c.mode, c.srw);
        lut_set_paths(c.lut);
        if (lut_init() != 0) { printf("lut: FAILED\n"); return 1; }
        printf("lut: ok, 3D %d^3, 1D %d, path %s\n", g_lut.n3, g_lut.n1, g_lut_cur);
        printf("selftest ok\n");
        return 0;
    }

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }

    cfg_defaults(&cfg);
    if (argc > 1) cfg_load(&cfg, argv[1]);

    g_log = fopen(cfg.log, "a");
    if (!g_log) logmsg("cannot open log %s, logging to stderr", cfg.log);

    logmsg("lutd starting (pid %d), config watch=%s lut=%s",
           (int)getpid(), cfg.watch, cfg.lut);

    return daemon_run(&cfg);
}

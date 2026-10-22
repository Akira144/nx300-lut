/*
 * lutshow - 在 NX300 屏幕上显示一条状态条，不依赖任何 X11 库。
 *
 * 直接实现 X11 协议（unix socket /tmp/.X11-unix/X0），静态编译，
 * 因此不受相机上老 glibc 的影响（相机是 glibc 2.x，工具链是 2.39）。
 *
 * 用法:
 *   lutshow "LUT 2/5 portra.cube" 3
 *   lutshow --probe                     # 只测试能否连上 X（0=能，1=不能）
 *   lutshow --bar 2 200,40,40           # 只画色条（红），2 秒
 *
 * 选项:  --color R,G,B   背景色，默认 30,160,90（绿）
 *        --bg           不画文字，只画色条
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static int g_fd = -1;
static uint32_t g_id_base = 0;
static uint8_t g_req[4096];
static int g_len = 0;

/* ---------------------------------------------------------------- */

static void p8(unsigned v)  { g_req[g_len++] = (uint8_t)v; }

static void p16(unsigned v)
{
    g_req[g_len++] = (uint8_t)(v & 0xFF);
    g_req[g_len++] = (uint8_t)((v >> 8) & 0xFF);
}

static void p32(uint32_t v)
{
    g_req[g_len++] = (uint8_t)(v & 0xFF);
    g_req[g_len++] = (uint8_t)((v >> 8) & 0xFF);
    g_req[g_len++] = (uint8_t)((v >> 16) & 0xFF);
    g_req[g_len++] = (uint8_t)((v >> 24) & 0xFF);
}

static void patch16(int at, unsigned v)
{
    g_req[at] = (uint8_t)(v & 0xFF);
    g_req[at + 1] = (uint8_t)((v >> 8) & 0xFF);
}

/* X11 请求头固定 4 字节: opcode, data, length(4字节单位, 含自身) */
static void begin(int opcode, int data)
{
    g_len = 0;
    p8(opcode);
    p8(data);
    p16(0);
}

static int end(void)
{
    while (g_len % 4) p8(0);
    patch16(2, (unsigned)(g_len / 4));
    return g_len;
}

static int wr_all(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(g_fd, p, n);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int rd_all(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(g_fd, p, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int send_req(void) { return wr_all(g_req, (size_t)g_len); }

/* ---------------------------------------------------------------- */
/* MIT-MAGIC-COOKIE-1 from $HOME/.Xauthority                         */

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static int read_cookie(uint8_t *out, int *outlen)
{
    char path[512];
    const char *home = getenv("HOME");
    uint8_t *buf;
    struct stat st;
    FILE *f;
    size_t n, off;

    snprintf(path, sizeof path, "%s/.Xauthority", home ? home : "/root");
    if (!(f = fopen(path, "rb"))) return -1;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 65536) { fclose(f); return -1; }
    if (!(buf = malloc((size_t)st.st_size))) { fclose(f); return -1; }
    n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);

    off = 0;
    while (off + 2 <= n) {
        size_t p = off + 2;
        unsigned l[4];
        int i;

        for (i = 0; i < 4; i++) {
            if (p + 2 > n) goto out;
            l[i] = rd16(buf + p);
            p += 2;
            if (p + l[i] > n) goto out;
        }
        if (l[2] == 18 && !memcmp(buf + p + l[0] + l[1], "MIT-MAGIC-COOKIE-1", 18) &&
            l[3] > 0 && l[3] <= 64) {
            memcpy(out, buf + p + l[0] + l[1] + l[2], l[3]);
            *outlen = (int)l[3];
            free(buf);
            return 0;
        }
        p += (size_t)l[0] + l[1] + l[2] + l[3];
        off = p;
    }
out:
    free(buf);
    return -1;
}

/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t root, cmap, white, black;
    int width, height, depth;
} Screen;

static int x_connect(Screen *scr)
{
    struct sockaddr_un sa;
    uint8_t buf[4096];
    uint8_t cookie[64];
    uint32_t id_base = 0;
    int clen = 0;
    unsigned nfmt, nscr, addlen, vendor_len;
    size_t off;

    if ((g_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "/tmp/.X11-unix/X0");
    if (connect(g_fd, (struct sockaddr *)&sa, sizeof sa) != 0) goto fail;

    if (read_cookie(cookie, &clen) != 0) clen = 0;

    g_len = 0;
    p8(0x6C);                      /* little endian */
    p8(0);
    p16(11);
    p16(0);
    p16(clen ? 18u : 0u);
    p16((unsigned)clen);
    p8(0); p8(0);
    if (clen) {
        memcpy(g_req + g_len, "MIT-MAGIC-COOKIE-1", 18);
        g_len += 18;
        while (g_len % 4) p8(0);
        memcpy(g_req + g_len, cookie, (size_t)clen);
        g_len += clen;
        while (g_len % 4) p8(0);
    }
    if (wr_all(g_req, (size_t)g_len) != 0) goto fail;

    if (rd_all(buf, 8) != 0) goto fail;
    if (buf[0] != 1) goto fail;

    addlen = (unsigned)buf[6] | ((unsigned)buf[7] << 8);
    if (addlen * 4u > sizeof buf) goto fail;
    if (rd_all(buf, (size_t)addlen * 4) != 0) goto fail;
    if (addlen * 4u < 40) goto fail;

    id_base = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
              ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    g_id_base = id_base;

    vendor_len = (unsigned)buf[16] | ((unsigned)buf[17] << 8);
    nscr = buf[20];
    nfmt = buf[21];
    if (!nscr) goto fail;

    off = 32 + vendor_len;
    while (off % 4) off++;
    off += (size_t)nfmt * 8;
    if (off + 40 > (size_t)addlen * 4) goto fail;

    scr->root   = (uint32_t)buf[off]     | ((uint32_t)buf[off + 1] << 8) |
                  ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
    scr->cmap   = (uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                  ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
    scr->white  = (uint32_t)buf[off + 8] | ((uint32_t)buf[off + 9] << 8) |
                  ((uint32_t)buf[off + 10] << 16) | ((uint32_t)buf[off + 11] << 24);
    scr->black  = (uint32_t)buf[off + 12];
    scr->width  = (int)((unsigned)buf[off + 20] | ((unsigned)buf[off + 21] << 8));
    scr->height = (int)((unsigned)buf[off + 22] | ((unsigned)buf[off + 23] << 8));
    scr->depth  = buf[off + 38];
    return 0;

fail:
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    return -1;
}

/* ---------------------------------------------------------------- */

static unsigned long x_alloc_color(const Screen *scr, int r, int g, int b)
{
    uint8_t rep[32];

    begin(84, 0);
    p32(scr->cmap);
    p16((unsigned)(r * 257));
    p16((unsigned)(g * 257));
    p16((unsigned)(b * 257));
    p16(0);
    end();
    if (send_req() != 0 || rd_all(rep, 32) != 0) return 0;
    if (rep[0] != 1) return 0;
    return (unsigned long)rep[12] | ((unsigned long)rep[13] << 8) |
           ((unsigned long)rep[14] << 16) | ((unsigned long)rep[15] << 24);
}

static uint32_t x_window(const Screen *scr, int x, int y, int w, int h, unsigned long bg)
{
    uint32_t win = g_id_base | 0x00000100u;

    begin(1, 0);                   /* CreateWindow, depth 0 = copy from parent */
    p32(win);
    p32(scr->root);
    p16((unsigned)x);
    p16((unsigned)y);
    p16((unsigned)w);
    p16((unsigned)h);
    p16(0);                        /* border width */
    p16(1);                        /* class: InputOutput */
    p32(0);                        /* visual: copy from parent */
    p32(2);                        /* CWBackPixel */
    p32((uint32_t)bg);
    end();
    if (send_req() != 0) return 0;

    begin(2, 0);                   /* ChangeWindowAttributes */
    p32(win);
    p32(512);                      /* CWOverrideRedirect */
    p32(1);
    end();
    if (send_req() != 0) return 0;

    begin(8, 0);                   /* MapWindow */
    p32(win);
    end();
    if (send_req() != 0) return 0;

    return win;
}

static uint32_t x_gc(uint32_t win, unsigned long fg, uint32_t font)
{
    uint32_t gc = g_id_base | 0x00000200u;

    begin(55, 0);                  /* CreateGC */
    p32(gc);
    p32(win);
    p32(font ? (4u | 16384u) : 4u);   /* GCForeground | GCFont */
    p32((uint32_t)fg);
    if (font) p32(font);
    end();
    if (send_req() != 0) return 0;
    return gc;
}

static uint32_t x_font(const char *name)
{
    uint32_t fid = g_id_base | 0x00000300u;
    size_t n = strlen(name);

    begin(45, 0);                  /* OpenFont */
    p32(fid);
    p16((unsigned)n);
    p16(0);
    memcpy(g_req + g_len, name, n);
    g_len += (int)n;
    end();
    if (send_req() != 0) return 0;
    return fid;
}

static void x_text(uint32_t win, uint32_t gc, int x, int y, const char *s)
{
    size_t n = strlen(s);

    if (n > 250) n = 250;
    begin(76, (int)n);             /* ImageText8: byte1 = 字符串长度 */
    p32(win);
    p32(gc);
    p16((unsigned)x);
    p16((unsigned)y);
    memcpy(g_req + g_len, s, n);
    g_len += (int)n;
    end();
    send_req();
}

static void x_unmap(uint32_t win)
{
    begin(10, 0);
    p32(win);
    end();
    send_req();
}

static void x_flush(void)
{
    begin(43, 0);
    end();
    send_req();
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    Screen scr;
    uint32_t win, gc, font;
    unsigned long bg, fg;
    const char *text = NULL;
    int secs = 2, r = 30, g = 160, b = 90;
    int probe = 0, bar_only = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--bar")) bar_only = 1;
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d", &r, &g, &b) != 3) { r = 30; g = 160; b = 90; }
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9' && strlen(argv[i]) <= 2) {
            secs = atoi(argv[i]);
        } else {
            text = argv[i];
        }
    }

    if (x_connect(&scr) != 0) return 1;
    if (probe) { close(g_fd); return 0; }

    bg = x_alloc_color(&scr, r, g, b);
    if (!bg) bg = scr.black;

    {
        int w = scr.width  > 0 ? scr.width  : 480;
        int h = (scr.height > 600) ? 64 : 44;
        int y = (scr.height > h) ? scr.height - h : 0;

        win = x_window(&scr, 0, y, w, h, bg);
        if (!win) { close(g_fd); return 1; }
        x_flush();
    }

    if (text && !bar_only) {
        fg = x_alloc_color(&scr, 255, 255, 255);
        if (!fg) fg = scr.white;
        font = x_font("fixed");
        gc = x_gc(win, fg, font);
        if (gc) {
            x_text(win, gc, 10, 26, text);
            x_flush();
        }
    }

    if (secs < 1)  secs = 1;
    if (secs > 30) secs = 30;
    sleep((unsigned)secs);

    x_unmap(win);
    x_flush();
    close(g_fd);
    return 0;
}

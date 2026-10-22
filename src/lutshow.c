/*
 * lutshow - 在 NX300 屏幕上显示状态条 / 文字，零依赖。
 *
 * 不使用任何 X11 库，也不使用服务端字体：文字用内置 5x7 点阵，
 * 静态编译，可运行在相机那套老 glibc 上。
 *
 * 用法:
 *   lutshow "PROCESSING 3/5" 20
 *   lutshow --calib 8                   # 校准：四条边各涂一色
 *   lutshow --probe                     # 只测能否连上 X
 *
 * 选项:
 *   --edge bottom|top|left|right   贴哪条边（默认读配置文件）
 *   --rot 0|90|180|270             文字旋转（默认读配置文件）
 *   --color R,G,B                  背景色
 *   --bg                           只画色条不画字
 *
 * 配置文件: /mnt/mmc/lut/overlay.conf
 *   edge=bottom
 *   rot=0
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* X11 基础                                                            */

static int g_fd = -1;
static uint32_t g_id_base = 0;
static uint8_t g_req[16000];
static int g_len = 0;

static void p8(unsigned v) { g_req[g_len++] = (uint8_t)v; }

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

typedef struct {
    uint32_t root, cmap, white, black;
    int width, height;
} Screen;

static int x_connect(Screen *scr)
{
    struct sockaddr_un sa;
    uint8_t buf[4096];
    uint8_t cookie[64];
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
    p8(0x6C); p8(0); p16(11); p16(0);
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

    g_id_base = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
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
    scr->white  = (uint32_t)buf[off + 8];
    scr->black  = (uint32_t)buf[off + 12];
    scr->width  = (int)((unsigned)buf[off + 20] | ((unsigned)buf[off + 21] << 8));
    scr->height = (int)((unsigned)buf[off + 22] | ((unsigned)buf[off + 23] << 8));
    return 0;

fail:
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    return -1;
}

static unsigned long x_color(const Screen *s, int r, int g, int b)
{
    uint8_t rep[32];

    begin(84, 0);
    p32(s->cmap);
    p16((unsigned)(r * 257)); p16((unsigned)(g * 257)); p16((unsigned)(b * 257));
    p16(0);
    end();
    if (send_req() != 0 || rd_all(rep, 32) != 0) return 0;
    if (rep[0] != 1) return 0;
    return (unsigned long)rep[12] | ((unsigned long)rep[13] << 8) |
           ((unsigned long)rep[14] << 16) | ((unsigned long)rep[15] << 24);
}

static uint32_t x_window(const Screen *s, int x, int y, int w, int h, unsigned long bg)
{
    uint32_t win = g_id_base | 0x00000100u;

    begin(1, 0);
    p32(win); p32(s->root);
    p16((unsigned)x); p16((unsigned)y); p16((unsigned)w); p16((unsigned)h);
    p16(0); p16(1); p32(0);
    p32(2); p32((uint32_t)bg);
    end();
    if (send_req() != 0) return 0;

    begin(2, 0);
    p32(win); p32(512); p32(1);
    end();
    if (send_req() != 0) return 0;

    begin(8, 0);
    p32(win);
    end();
    if (send_req() != 0) return 0;

    return win;
}

static uint32_t x_gc(uint32_t win, unsigned long fg, unsigned slot)
{
    uint32_t gc = g_id_base | (0x00000200u + slot);

    begin(55, 0);
    p32(gc); p32(win); p32(4); p32((uint32_t)fg);
    end();
    if (send_req() != 0) return 0;
    return gc;
}

static void x_rect(uint32_t win, uint32_t gc, int x, int y, int w, int h)
{
    begin(70, 0);
    p32(win); p32(gc);
    p16((unsigned)x); p16((unsigned)y); p16((unsigned)w); p16((unsigned)h);
    end();
    send_req();
}

static void x_map(uint32_t win)
{
    begin(8, 0);
    p32(win);
    end();
    send_req();
}

/* 把窗口提到最上层（ConfigureWindow, stack-mode=Above） */
static void x_raise(uint32_t win)
{
    begin(12, 0);
    p32(win);
    p16(64);        /* CWStackMode */
    p16(0);
    p32(0);         /* Above */
    end();
    send_req();
}

/* 读掉挂起的错误/事件并写日志，方便诊断"为什么看不到" */
static void x_drain(const char *tag)
{
    uint8_t b[64];
    ssize_t n;

    fcntl(g_fd, F_SETFL, fcntl(g_fd, F_GETFL, 0) | O_NONBLOCK);
    while ((n = read(g_fd, b, sizeof b)) > 0) {
        if (b[0] == 0) {
            fprintf(stderr, "lutshow: %s X error code=%d\n", tag, b[1]);
        }
    }
    (void)n;
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

/* ------------------------------------------------------------------ */
/* 内置 5x7 点阵字库（只用到等宽大写字母、数字和几个符号）              */

#define G(a,b,c,d,e,f,g) a b c d e f g

static const char *glyph(char ch)
{
    static const char *F[128] = {
        ['A'] = G(".###.","#...#","#...#","#####","#...#","#...#","#...#"),
        ['B'] = G("####.","#...#","#...#","####.","#...#","#...#","####."),
        ['C'] = G(".###.","#...#","#....","#....","#....","#...#",".###."),
        ['D'] = G("####.","#...#","#...#","#...#","#...#","#...#","####."),
        ['E'] = G("#####","#....","#....","####.","#....","#....","#####"),
        ['F'] = G("#####","#....","#....","####.","#....","#....","#...."),
        ['G'] = G(".###.","#...#","#....","#.###","#...#","#...#",".####"),
        ['H'] = G("#...#","#...#","#...#","#####","#...#","#...#","#...#"),
        ['I'] = G("#####","..#..","..#..","..#..","..#..","..#..","#####"),
        ['J'] = G("..###","...#.","...#.","...#.","...#.","#..#.",".##.."),
        ['K'] = G("#...#","#..#.","#.#..","##...","#.#..","#..#.","#...#"),
        ['L'] = G("#....","#....","#....","#....","#....","#....","#####"),
        ['M'] = G("#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"),
        ['N'] = G("#...#","##..#","#.#.#","#..##","#...#","#...#","#...#"),
        ['O'] = G(".###.","#...#","#...#","#...#","#...#","#...#",".###."),
        ['P'] = G("####.","#...#","#...#","####.","#....","#....","#...."),
        ['Q'] = G(".###.","#...#","#...#","#...#","#.#.#","#..#.",".##.#"),
        ['R'] = G("####.","#...#","#...#","####.","#.#..","#..#.","#...#"),
        ['S'] = G(".####","#....","#....",".###.","....#","....#","####."),
        ['T'] = G("#####","..#..","..#..","..#..","..#..","..#..","..#.."),
        ['U'] = G("#...#","#...#","#...#","#...#","#...#","#...#",".###."),
        ['V'] = G("#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."),
        ['W'] = G("#...#","#...#","#...#","#.#.#","#.#.#","##.##","#...#"),
        ['X'] = G("#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"),
        ['Y'] = G("#...#","#...#",".#.#.","..#..","..#..","..#..","..#.."),
        ['Z'] = G("#####","....#","...#.","..#..",".#...","#....","#####"),
        ['0'] = G(".###.","#...#","#..##","#.#.#","##..#","#...#",".###."),
        ['1'] = G("..#..",".##..","..#..","..#..","..#..","..#..","#####"),
        ['2'] = G(".###.","#...#","....#","...#.","..#..",".#...","#####"),
        ['3'] = G("#####","...#.","..#..","...#.","....#","#...#",".###."),
        ['4'] = G("...#.","..##.",".#.#.","#..#.","#####","...#.","...#."),
        ['5'] = G("#####","#....","####.","....#","....#","#...#",".###."),
        ['6'] = G("..##.",".#...","#....","####.","#...#","#...#",".###."),
        ['7'] = G("#####","....#","...#.","..#..",".#...",".#...",".#..."),
        ['8'] = G(".###.","#...#","#...#",".###.","#...#","#...#",".###."),
        ['9'] = G(".###.","#...#","#...#",".####","....#","...#.",".##.."),
        [' '] = G(".....",".....",".....",".....",".....",".....","....."),
        ['.'] = G(".....",".....",".....",".....",".....",".##..",".##.."),
        ['/'] = G("....#","....#","...#.","..#..",".#...","#....","#...."),
        ['-'] = G(".....",".....",".....","#####",".....",".....","....."),
        [':'] = G(".....",".##..",".##..",".....",".##..",".##..","....."),
    };
    unsigned c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 128 || !F[c]) c = ' ';
    return F[c];
}

/* 画一个字符；返回前进量 */
static int draw_char(uint32_t win, uint32_t gc, int ox, int oy, int scale, int rot, char ch)
{
    const char *g = glyph(ch);
    int c, r, adv = 6 * scale;
    int W = 5 * scale, H = 7 * scale;

    if (!g) return adv;
    for (r = 0; r < 7; r++) {
        for (c = 0; c < 5; c++) {
            int dx = c * scale, dy = r * scale;
            int X, Y;
            if (g[r * 5 + c] != '#') continue;
            switch (rot) {
            case 90:  X = ox + (H - dy - scale); Y = oy + dx; break;
            case 180: X = ox + (W - dx - scale); Y = oy + (H - dy - scale); break;
            case 270: X = ox + dy;               Y = oy + (W - dx - scale); break;
            default:  X = ox + dx;               Y = oy + dy; break;
            }
            x_rect(win, gc, X, Y, scale, scale);
        }
    }
    return adv;
}

static void draw_text(uint32_t win, uint32_t gc, int x, int y, int scale, int rot, const char *s)
{
    int ox = x, oy = y;

    for (; *s; s++) {
        int adv = draw_char(win, gc, ox, oy, scale, rot, *s);
        if (rot == 90 || rot == 270) oy += adv;
        else if (rot == 180)         ox -= adv;
        else                         ox += adv;
    }
}

/* ------------------------------------------------------------------ */

static int load_conf(const char *path, char *edge, size_t en, int *rot)
{
    FILE *f = fopen(path, "r");
    char line[128];

    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "edge=", 5)) {
            char *v = p + 5, *q = v;
            while (*q && *q != '\n' && *q != '\r' && *q != ' ') q++;
            *q = 0;
            snprintf(edge, en, "%s", v);
        } else if (!strncmp(p, "rot=", 4)) {
            *rot = atoi(p + 4);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    Screen scr;
    uint32_t win, gc;
    unsigned long bg = 0, fg = 0;
    char edge[16] = "bottom";
    int rot = 0, scale = 3, secs = 3, probe = 0, bar_only = 0, calib = 0, i;
    int r = 30, g = 160, b = 90;
    const char *text = NULL;

    load_conf("/mnt/mmc/lut/overlay.conf", edge, sizeof edge, &rot);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--bar")) bar_only = 1;
        else if (!strcmp(argv[i], "--calib")) calib = 1;
        else if (!strcmp(argv[i], "--edge") && i + 1 < argc) snprintf(edge, sizeof edge, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d", &r, &g, &b) != 3) { r = 30; g = 160; b = 90; }
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9' && strlen(argv[i]) <= 2) {
            secs = atoi(argv[i]);
        } else {
            text = argv[i];
        }
    }
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;

    if (x_connect(&scr) != 0) return 1;
    if (probe) { close(g_fd); return 0; }

    fprintf(stderr, "lutshow: screen %dx%d root=0x%x base=0x%x\n",
            scr.width, scr.height, scr.root, g_id_base);

    {
        int W = scr.width > 0 ? scr.width : 480;
        int H = scr.height > 0 ? scr.height : 800;
        int th = 22 * scale;
        int x = 0, y = H - th, w = W, h = th;

        if (!strcmp(edge, "top"))       { x = 0; y = 0; w = W; h = th; rot = rot ? rot : 0; }
        else if (!strcmp(edge, "left")) { x = 0; y = 0; w = th; h = H; rot = rot ? rot : 90; }
        else if (!strcmp(edge, "right")){ x = W - th; y = 0; w = th; h = H; rot = rot ? rot : 90; }
        else                            { x = 0; y = H - th; w = W; h = th; }

        if (calib) {
            /* 四条边各一色，用来判断屏幕旋转方向 */
            unsigned long c[4];
            c[0] = x_color(&scr, 230, 40, 40);    /* 上 = 红 */
            c[1] = x_color(&scr, 40, 200, 40);    /* 右 = 绿 */
            c[2] = x_color(&scr, 60, 90, 230);    /* 下 = 蓝 */
            c[3] = x_color(&scr, 230, 200, 40);   /* 左 = 黄 */
            win = x_window(&scr, 0, 0, W, H, c[0]);
            if (!win) return 1;
            x_rect(win, x_gc(win, c[1], 1), W - th, 0, th, H);   /* 右 = 绿 */
            x_rect(win, x_gc(win, c[2], 2), 0, H - th, W, th);   /* 下 = 蓝 */
            x_rect(win, x_gc(win, c[3], 3), 0, 0, th, H);        /* 左 = 黄 */
            for (i = 0; i < (secs > 0 ? secs : 8); i++) {
                x_raise(win);
                x_flush();
                x_drain("calib");
                sleep(1);
            }
            x_unmap(win);
            x_flush();
            close(g_fd);
            return 0;
        }

        bg = x_color(&scr, r, g, b);
        if (!bg) bg = scr.black;

        win = x_window(&scr, x, y, w, h, bg);
        if (!win) return 1;
        x_flush();

        if (text && !bar_only && *text) {
            fg = x_color(&scr, 255, 255, 255);
            gc = x_gc(win, fg, 0);
            if (gc) {
                int tx = 6, ty = 4;
                if (rot == 90)       { tx = 4; ty = 6; }
                else if (rot == 270) { tx = w - 4; ty = 6; }
                else if (rot == 180) { tx = w - 6; ty = h - 4; }
                draw_text(win, gc, tx, ty, scale, rot, text);
                x_flush();
            }
        }

        if (secs < 1)  secs = 1;
        if (secs > 30) secs = 30;
        for (i = 0; i < secs; i++) {
            x_raise(win);
            x_flush();
            x_drain("show");
            sleep(1);
        }
        x_unmap(win);
        x_flush();
    }

    close(g_fd);
    return 0;
}

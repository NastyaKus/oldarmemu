/*
 * Freestanding test /init for the htc-leo machine (no libc: the HD2
 * kernels are 2.6.32/3.0 and modern glibc refuses to start on them).
 * Prints to the ram console, paints colour bars on /dev/graphics/fb0
 * and then echoes touch events.
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static long sys(long n, long a, long b, long c)
{
    register long r7 asm("r7") = n;
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    asm volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}
#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_mknod 14
#define SYS_ioctl 54
#define SYS_mkdir 39
#define SYS_mount 21
#define SYS_nanosleep 162
#define SYS_mmap2 192

int raise(int sig) { (void)sig; return 0; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int kfd = -1;
static void say(const char *s)
{
    if (kfd >= 0) sys(SYS_write, kfd, (long)s, slen(s));
}
static void num(char *b, u32 v)
{
    char t[12]; int i = 0, j = 0;
    do { t[i++] = '0' + v % 10; v /= 10; } while (v);
    while (i) b[j++] = t[--i];
    b[j] = 0;
}

struct fb_var { u32 xres, yres, xres_v, yres_v, xoff, yoff, bpp, gray;
                u32 rest[32]; };

void _start(void)
{
    char buf[64];
    sys(SYS_mkdir, (long)"/dev", 0755, 0);
    sys(SYS_mkdir, (long)"/proc", 0755, 0);
    sys(SYS_mkdir, (long)"/sys", 0755, 0);
    sys(SYS_mount, (long)"proc", (long)"/proc", (long)"proc");
    sys(SYS_mknod, (long)"/dev/kmsg", 020000 | 0600, (1 << 8) | 11);
    sys(SYS_mknod, (long)"/dev/fb0", 020000 | 0600, (29 << 8) | 0);
    sys(SYS_mknod, (long)"/dev/input0", 020000 | 0600, (13 << 8) | 64);
    kfd = sys(SYS_open, (long)"/dev/kmsg", 1, 0);
    say("fbtest: userspace is alive on htc-leo\n");

    int fb = sys(SYS_open, (long)"/dev/fb0", 2, 0);
    struct fb_var var;
    if (fb < 0 || sys(SYS_ioctl, fb, 0x4600 /* FBIOGET_VSCREENINFO */,
                      (long)&var) < 0) {
        say("fbtest: no framebuffer\n");
    } else {
        num(buf, var.xres); say("fbtest: fb "); say(buf);
        num(buf, var.yres); say("x"); say(buf);
        num(buf, var.bpp); say(" bpp "); say(buf); say("\n");
        /* paint row by row with write() */
        static u8 row[480 * 4];
        u32 bpp = var.bpp / 8;
        for (u32 y = 0; y < var.yres; y++) {
            for (u32 x = 0; x < var.xres; x++) {
                u32 bar = x * 8 / var.xres;
                u32 r = (bar & 4) ? 255 : 0, g = (bar & 2) ? 255 : 0,
                    b = (bar & 1) ? 255 : 0;
                if (y > var.yres / 2) { r = r * (var.yres - y) / (var.yres / 2);
                    g = g * (var.yres - y) / (var.yres / 2);
                    b = b * (var.yres - y) / (var.yres / 2); }
                if (bpp == 2) {
                    u16 v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                    row[x * 2] = v; row[x * 2 + 1] = v >> 8;
                } else {
                    row[x * bpp] = b; row[x * bpp + 1] = g;
                    row[x * bpp + 2] = r; if (bpp == 4) row[x * 4 + 3] = 255;
                }
            }
            sys(SYS_write, fb, (long)row, var.xres * bpp);
        }
        say("fbtest: painted colour bars\n");
    }
    int ev = sys(SYS_open, (long)"/dev/input0", 0, 0);
    for (;;) {
        u32 e[4];
        if (ev >= 0 && sys(SYS_read, ev, (long)e, 16) == 16) {
            u16 type = e[2] & 0xffff, code = e[2] >> 16;
            if (type == 3 && (code == 0 || code == 1)) {
                say(code ? " y=" : "touch x="); num(buf, e[3]); say(buf);
                if (code) say("\n");
            }
        } else {
            long ts[2] = { 1, 0 };
            sys(SYS_nanosleep, (long)ts, 0, 0);
        }
    }
}

/* Required for clock_gettime / nanosleep on glibc — macOS exposes them
 * by default but Linux needs the POSIX feature test macro defined before
 * any system header is included. */
#define _POSIX_C_SOURCE 200809L

/*
 * Continuum — terminal_main.c
 *
 * Pure-libc terminal renderer for the Continuum fluid simulator.
 *
 *   * Half-block UTF-8 character (▀, U+2580) so each terminal cell shows
 *     two vertical pixels (foreground = top, background = bottom).
 *   * 24-bit ANSI true-colour escape sequences (\x1b[38;2;R;G;Bm).
 *   * Raw mode termios + SGR mouse tracking (\x1b[?1003h\x1b[?1006h)
 *     for click + drag interaction.
 *   * Single re-used render buffer per frame, written to stdout in one
 *     fwrite to eliminate flicker.
 *
 * Zero non-libc dependencies.
 */
#include "fluid.h"
#include "presets.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---------- terminal mode plumbing -------------------------------------- */

static struct termios       g_orig_tio;
static volatile sig_atomic_t g_tio_saved   = 0;
static volatile sig_atomic_t g_should_quit = 0;

/* Loop on partial writes / EINTR. Used both at startup and shutdown so
 * the terminal protocol is never left half-emitted. */
static void safe_write(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        p   += (size_t)n;
        len -= (size_t)n;
    }
}

static void term_restore(void) {
    /* Idempotent: signal handler + atexit may both run. */
    if (!g_tio_saved) return;
    g_tio_saved = 0;

    static const char cleanup[] =
        "\x1b[?1003l"   /* mouse: any-event off */
        "\x1b[?1006l"   /* mouse: SGR off */
        "\x1b[?25h"     /* show cursor */
        "\x1b[0m"       /* reset attributes */
        "\x1b[?1049l";  /* leave alternate screen */
    safe_write(STDOUT_FILENO, cleanup, sizeof(cleanup) - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_tio);
}

/* Signal handler: ONLY set the atomic flag. Cleanup happens in main()
 * via atexit() once the loop notices and returns. No async-unsafe calls. */
static void on_signal(int sig) {
    (void)sig;
    g_should_quit = 1;
}

static int term_setup(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "continuum: stdin/stdout must be a terminal\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_tio) == -1) {
        perror("tcgetattr");
        return -1;
    }
    g_tio_saved = 1;
    atexit(term_restore);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);

    struct termios raw = g_orig_tio;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        return -1;
    }

    /* Enter alt screen, hide cursor, enable any-event SGR mouse mode. */
    static const char setup[] =
        "\x1b[?1049h"   /* alt screen */
        "\x1b[?25l"     /* hide cursor */
        "\x1b[2J"       /* clear */
        "\x1b[H"        /* home */
        "\x1b[?1003h"   /* any-event mouse */
        "\x1b[?1006h";  /* SGR extended mode */
    safe_write(STDOUT_FILENO, setup, sizeof(setup) - 1);
    return 0;
}

/* ---------- terminal size ----------------------------------------------- */

/* Hard ceiling on terminal dimensions we render. Anything bigger is
 * clamped — both to keep the render buffer bounded and to ensure the
 * size_t multiplication never wraps. 4096 columns × 4096 rows × 64 bytes
 * is well under SIZE_MAX on any 64-bit system. */
#define MAX_TERM_W 4096
#define MAX_TERM_H 4096

static void term_get_size(int *cols, int *rows) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        int c = (int)ws.ws_col;
        int r = (int)ws.ws_row;
        if (c < 1) c = 1;
        if (r < 1) r = 1;
        if (c > MAX_TERM_W) c = MAX_TERM_W;
        if (r > MAX_TERM_H) r = MAX_TERM_H;
        *cols = c;
        *rows = r;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* ---------- timing ------------------------------------------------------- */

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_seconds(double s) {
    if (s <= 0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* ---------- input parser ------------------------------------------------- */

typedef struct {
    int    have_mouse;
    int    button;        /* SGR button code */
    int    mx, my;        /* terminal cell coords (1-based) */
    int    pressed;       /* 1 = press / drag, 0 = release */

    int    have_key;
    char   key;
} InputEvent;

/* Hard cap on how many bytes a single CSI / mouse event may span. Beyond
 * this we treat the leading ESC as garbage and drop it so the parser can
 * never deadlock on a malformed prefix. */
#define MAX_CSI_BYTES 64
/* Cap on digits per numeric SGR field — prevents int overflow from a long
 * digit run on stdin. */
#define MAX_DIGITS    6

/* Returns:
 *   > 0 : number of bytes consumed (event may or may not be filled)
 *     0 : need more bytes
 *    -1 : parse error AND we are confident we should drop a byte
 */
static int parse_one_event(const char *buf, size_t len, InputEvent *out) {
    out->have_mouse = 0;
    out->have_key   = 0;
    if (len == 0) return 0;

    /* SGR mouse: ESC [ < button ; x ; y ; (M|m) */
    if (buf[0] == '\x1b' && len >= 2 && buf[1] == '[' &&
        len >= 3 && buf[2] == '<') {
        size_t i = 3;
        int b = 0, x = 0, y = 0;
        int d;

        d = 0;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') {
            if (d++ >= MAX_DIGITS) return -1;
            b = b * 10 + (buf[i] - '0'); i++;
        }
        if (i >= len) {
            if (i >= MAX_CSI_BYTES) return -1;
            return 0;
        }
        if (buf[i] != ';') return -1;
        i++;

        d = 0;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') {
            if (d++ >= MAX_DIGITS) return -1;
            x = x * 10 + (buf[i] - '0'); i++;
        }
        if (i >= len) { if (i >= MAX_CSI_BYTES) return -1; return 0; }
        if (buf[i] != ';') return -1;
        i++;

        d = 0;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') {
            if (d++ >= MAX_DIGITS) return -1;
            y = y * 10 + (buf[i] - '0'); i++;
        }
        if (i >= len) { if (i >= MAX_CSI_BYTES) return -1; return 0; }
        if (buf[i] != 'M' && buf[i] != 'm') return -1;

        out->have_mouse = 1;
        out->button     = b;
        out->mx         = x;
        out->my         = y;
        out->pressed    = (buf[i] == 'M');
        return (int)(i + 1);
    }

    /* Bare ESC or unknown CSI. */
    if (buf[0] == '\x1b') {
        if (len == 1) {
            out->have_key = 1;
            out->key      = 27;
            return 1;
        }
        if (buf[1] == '[') {
            size_t i = 2;
            size_t scan_max = len < MAX_CSI_BYTES ? len : MAX_CSI_BYTES;
            while (i < scan_max && !((buf[i] >= 'A' && buf[i] <= 'Z') ||
                                     (buf[i] >= 'a' && buf[i] <= 'z') ||
                                     buf[i] == '~')) {
                i++;
            }
            if (i < scan_max) return (int)(i + 1);
            if (i >= MAX_CSI_BYTES) return -1; /* too long, drop */
            return 0;
        }
        /* ESC + char (alt-key); just deliver the char. */
        out->have_key = 1;
        out->key      = buf[1];
        return 2;
    }

    /* Single char keystroke. */
    out->have_key = 1;
    out->key      = buf[0];
    return 1;
}

/* ---------- renderer ----------------------------------------------------- */

#define HALF_BLOCK "\xe2\x96\x80"  /* ▀ U+2580 */

static int clamp255(float x) {
    int v = (int)(x * 255.0f + 0.5f);
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void dye_to_rgb(FluidSim *sim, int sx, int sy,
                       int *r, int *g, int *b) {
    /* Tone-map to keep highlights in range. */
    float rr = fluid_get_dye_r(sim, sx + 1, sy + 1);
    float gg = fluid_get_dye_g(sim, sx + 1, sy + 1);
    float bb = fluid_get_dye_b(sim, sx + 1, sy + 1);
    rr = rr / (1.0f + rr);
    gg = gg / (1.0f + gg);
    bb = bb / (1.0f + bb);
    if (fluid_get_obstacle(sim, sx + 1, sy + 1)) {
        /* Obstacles render as a slate-grey block. */
        *r = 90; *g = 95; *b = 110;
        return;
    }
    *r = clamp255(rr);
    *g = clamp255(gg);
    *b = clamp255(bb);
}

static char *render_frame(FluidSim *sim, char *buf, int term_w, int term_h,
                          int show_status, double fps, PresetID preset,
                          int show_velocity) {
    char *p = buf;

    /* Home cursor (we already cleared via alt screen on entry). */
    p += sprintf(p, "\x1b[H");

    /* Reserve the bottom row for status when requested. */
    int draw_rows = show_status ? term_h - 1 : term_h;
    if (draw_rows < 1) draw_rows = 1;

    int N = fluid_size(sim);
    int last_fr = -1, last_fg = -1, last_fb = -1;
    int last_br = -1, last_bg = -1, last_bb = -1;

    for (int ty = 0; ty < draw_rows; ty++) {
        int sy_top = (int)(((double)(ty * 2)     / (double)(draw_rows * 2)) * (double)N);
        int sy_bot = (int)(((double)(ty * 2 + 1) / (double)(draw_rows * 2)) * (double)N);
        if (sy_top >= N) sy_top = N - 1;
        if (sy_bot >= N) sy_bot = N - 1;

        for (int tx = 0; tx < term_w; tx++) {
            int sx = (int)(((double)tx / (double)term_w) * (double)N);
            if (sx >= N) sx = N - 1;

            int rt, gt, bt, rb, gb, bb;
            dye_to_rgb(sim, sx, sy_top, &rt, &gt, &bt);
            dye_to_rgb(sim, sx, sy_bot, &rb, &gb, &bb);

            if (show_velocity) {
                /* Tint by velocity magnitude. */
                float vm = fluid_get_velocity_mag(sim, sx + 1, sy_top + 1);
                int   tint = clamp255(vm * 0.4f);
                rt = (rt + tint) / 2;
                gt = (gt + tint) / 2;
                bt = (bt + 255) / 2;
            }

            if (rt != last_fr || gt != last_fg || bt != last_fb) {
                p += sprintf(p, "\x1b[38;2;%d;%d;%dm", rt, gt, bt);
                last_fr = rt; last_fg = gt; last_fb = bt;
            }
            if (rb != last_br || gb != last_bg || bb != last_bb) {
                p += sprintf(p, "\x1b[48;2;%d;%d;%dm", rb, gb, bb);
                last_br = rb; last_bg = gb; last_bb = bb;
            }
            memcpy(p, HALF_BLOCK, 3); p += 3;
        }
        p += sprintf(p, "\x1b[0m\r\n");
        last_fr = last_fg = last_fb = -1;
        last_br = last_bg = last_bb = -1;
    }

    if (show_status) {
        const char *names[] = {"none", "smoke", "ink", "karman", "kelvin-helmholtz"};
        const char *pname = (preset >= 0 && preset <= 4) ? names[preset] : "?";
        p += sprintf(p,
            "\x1b[0m\x1b[7m  continuum  \x1b[0m  %5.1f fps  N=%d  preset=%s  "
            "[1-4]preset [r]reset [v]vel [q]quit  ",
            fps, fluid_size(sim), pname);
    }
    return p;
}

/* ---------- main loop ---------------------------------------------------- */

int main(int argc, char **argv) {
    int N = 96;
    if (argc > 1) {
        int n = atoi(argv[1]);
        if (n >= 16 && n <= 512) N = n;
    }

    if (term_setup() != 0) return 1;

    FluidSim *sim = fluid_create(N, 0.1f, 0.00005f, 0.00005f);
    if (!sim) {
        fprintf(stderr, "fluid_create failed\n");
        return 2;
    }
    fluid_set_vorticity(sim, 0.08f);

    PresetID current_preset = PRESET_SMOKE;
    preset_apply(sim, current_preset);

    int term_w = 80, term_h = 24;
    term_get_size(&term_w, &term_h);

    /* Per-cell render budget: ~50 bytes is plenty for the SGR triples.
     * Bounded by MAX_TERM_W × MAX_TERM_H so the multiplication is safe. */
    size_t buf_cap = (size_t)term_w * (size_t)term_h * 64u + 4096u;
    char  *render_buf = (char *)malloc(buf_cap);
    if (!render_buf) {
        fprintf(stderr, "out of memory\n");
        fluid_destroy(sim);
        return 3;
    }

    /* Drag tracking */
    int drag_active   = 0;   /* 1 = left button held, 2 = right button held */
    int last_mx = -1, last_my = -1;
    int show_velocity = 0;

    int    frame      = 0;
    double last_time  = monotonic_seconds();
    double fps_accum  = 0;
    int    fps_frames = 0;
    double fps_disp   = 0;

    const double target_dt = 1.0 / 60.0;

    char inbuf[1024];
    size_t inbuf_len = 0;

    int running = 1;
    while (running && !g_should_quit) {
        /* --- Resize check ----------------------------------------- */
        int new_w, new_h;
        term_get_size(&new_w, &new_h);
        if (new_w != term_w || new_h != term_h) {
            term_w = new_w; term_h = new_h;
            size_t new_cap = (size_t)term_w * (size_t)term_h * 64u + 4096u;
            if (new_cap > buf_cap) {
                char *nb = (char *)realloc(render_buf, new_cap);
                if (nb) { render_buf = nb; buf_cap = new_cap; }
            }
            safe_write(STDOUT_FILENO, "\x1b[2J", 4);
        }

        /* --- Read input via select for a short timeout ------------ */
        fd_set rfds; FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0) {
            ssize_t got = read(STDIN_FILENO, inbuf + inbuf_len,
                               sizeof(inbuf) - inbuf_len);
            if (got > 0) inbuf_len += (size_t)got;
        }

        /* Drain whatever events we can parse. */
        size_t off = 0;
        while (off < inbuf_len) {
            InputEvent ev;
            int rc = parse_one_event(inbuf + off, inbuf_len - off, &ev);
            if (rc == 0) {
                /* Need more bytes — but if the buffer is already huge and
                 * still unparseable, drop the head byte to recover. */
                if (inbuf_len - off >= MAX_CSI_BYTES) { off++; continue; }
                break;
            }
            if (rc < 0) { off++; continue; } /* malformed: drop one byte */
            size_t consumed = (size_t)rc;
            off += consumed;

            if (ev.have_mouse) {
                /* SGR button bits:
                 *   low 2 bits: 0=left, 1=middle, 2=right, 3=release
                 *   bit 5 (32): motion while pressed
                 */
                int btn  = ev.button & 3;
                int drag = (ev.button & 32) ? 1 : 0;

                /* Map terminal cell -> sim cell. */
                int draw_rows = term_h - 1;
                if (draw_rows < 1) draw_rows = 1;
                int sx = (int)((double)(ev.mx - 1) / (double)term_w * (double)N);
                /* Each terminal row has 2 vertical pixels — pick the top one. */
                int sy = (int)((double)((ev.my - 1) * 2) / (double)(draw_rows * 2) * (double)N);
                if (sx < 1) sx = 1;
                if (sx > N) sx = N;
                if (sy < 1) sy = 1;
                if (sy > N) sy = N;

                if (ev.pressed) {
                    if (drag_active == 0) {
                        if (btn == 0) drag_active = 1;
                        else if (btn == 2) drag_active = 2;
                        last_mx = sx; last_my = sy;
                    }
                    if (drag) {
                        float dx = (float)(sx - last_mx);
                        float dy = (float)(sy - last_my);
                        if (drag_active == 1) {
                            float hue = (float)((sx + sy) % 60) / 60.0f;
                            float r = 0.5f + 0.5f * sinf(hue * 6.28f + 0.0f);
                            float g = 0.5f + 0.5f * sinf(hue * 6.28f + 2.09f);
                            float b = 0.5f + 0.5f * sinf(hue * 6.28f + 4.18f);
                            fluid_add_dye(sim, sx, sy, r, g, b, 3.0f);
                        }
                        fluid_add_velocity(sim, sx, sy, dx * 4.0f, dy * 4.0f);
                        last_mx = sx; last_my = sy;
                    } else {
                        last_mx = sx; last_my = sy;
                    }
                } else {
                    drag_active = 0;
                }
            }

            if (ev.have_key) {
                char k = ev.key;
                if (k == 'q' || k == 27 /*ESC*/ || k == 3 /*^C*/) {
                    running = 0;
                } else if (k == 'r') {
                    preset_apply(sim, current_preset);
                } else if (k == 'v') {
                    show_velocity = !show_velocity;
                } else if (k >= '1' && k <= '4') {
                    current_preset = (PresetID)(k - '0');
                    preset_apply(sim, current_preset);
                    frame = 0;
                }
            }
        }
        if (off > 0) {
            memmove(inbuf, inbuf + off, inbuf_len - off);
            inbuf_len -= off;
        }

        /* --- Tick the preset and step --------------------------- */
        preset_tick(sim, current_preset, frame);
        fluid_step(sim);

        /* --- Render --------------------------------------------- */
        char *end = render_frame(sim, render_buf, term_w, term_h,
                                 1, fps_disp, current_preset, show_velocity);
        safe_write(STDOUT_FILENO, render_buf, (size_t)(end - render_buf));

        /* --- FPS accounting + frame pacing ---------------------- */
        double now = monotonic_seconds();
        double dt  = now - last_time;
        last_time  = now;
        fps_accum  += dt;
        fps_frames += 1;
        if (fps_accum >= 0.5) {
            fps_disp   = (double)fps_frames / fps_accum;
            fps_accum  = 0;
            fps_frames = 0;
        }
        double remain = target_dt - dt;
        if (remain > 0) sleep_seconds(remain);

        frame++;
    }

    free(render_buf);
    fluid_destroy(sim);
    term_restore();
    return 0;
}

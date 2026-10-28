/*
 * airscan-cli.c — AirScan-QR 终端发送端（Linux / Windows 双平台）
 *
 * 在终端里循环渲染动态二维码，用手机上的浏览器页（index.html 接收端）扫码接收。
 * 协议与网页版完全一致：tid|文件名|总帧数|帧号|数据
 *
 * 平台行为：
 *   - Linux/Unix：ANSI 转义序列渲染，任意终端可用；
 *   - Windows 真控制台（cmd/conhost）：走 Win32 控制台 API（WriteConsoleOutputW），
 *     用控制台属性画黑白块，XP 到 Win11 全系 cmd 都能正常显示，不依赖 VT 支持；
 *   - Windows 非控制台输出（Git Bash / mintty / 重定向到文件）：自动退回 ANSI 渲染。
 *
 * 编译：
 *   Linux   : musl-gcc -O2 -static -o airscan-cli airscan-cli.c qrcodegen.c   （见 build.sh）
 *   Windows : gcc -O2 -static -o airscan-cli.exe airscan-cli.c qrcodegen.c -lwinmm
 *
 * 二维码编码器：Nayuki QR Code generator（MIT，见 qrcodegen.c/h）。
 *
 * 用法: airscan-cli [选项] 文件        （详见 --help）
 */

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>          /* CommandLineToArgvW：从宽字符命令行还原参数 */
  #include <mmsystem.h>          /* timeBeginPeriod：提高 Sleep 精度 */
  #include <conio.h>
  #include <io.h>
  #include <fcntl.h>
  #include <locale.h>
  #define ISATTY _isatty
  #define FILENO _fileno
#else
  #define _POSIX_C_SOURCE 200809L
  #include <unistd.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <locale.h>
  #include <langinfo.h>
  #include <sys/stat.h>
  #include <sys/ioctl.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #include <termios.h>
  #define ISATTY isatty
  #define FILENO fileno
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <signal.h>          /* sig_atomic_t：两平台都需要 */
#include <getopt.h>

#include "qrcodegen.h"
#include "miniz.h"          /* zlib 压缩（内联，无外部依赖） */

#define PROG_NAME    "airscan-cli"
#define PROG_VERSION "2.0"

#define QUIET_ZONE      4       /* 二维码静区（模块数），4 为标准最小值 */
#define MAX_NAME_LEN    255     /* 载荷中文件名的字节上限（常见文件系统上限）。
                                   真正的约束是"文件名 + chunk 要装得进二维码"，
                                   由 plan_frame 检查，超了会明确报错 */
#define MAX_CHUNK       2400    /* 单帧数据上限（QR v40-L 载荷容量约 2953 字节，
                                   还要给任务号/文件名/帧号留位置） */
#define MIN_CHUNK       16
#define MAX_FILE        ((size_t)256 * 1024 * 1024)
                                /* 文件上限：屏幕→摄像头链路按 8fps×200字符 估算
                                   只有几 KB/s，256MB 要几十小时，属误用；
                                   同时保护 32 位构建（base64 后体积 ×1.33） */
#define FRAME_BUF       (400L * 1024L)  /* ANSI 渲染缓冲：v40 方块模式最坏约 240KB */

/* ============================================================
 * 平台层
 * ============================================================ */

static int g_stdin_interactive = 0;   /* 能从标准输入读按键 */
static volatile sig_atomic_t g_quit = 0;

static void sleep_ms(long ms);

/* ---------- 时间 ---------- */
static double plat_now(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);      /* 不用 clock_gettime：老内核兼容性更好 */
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
#endif
}

static void sleep_ms(long ms)
{
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
#endif
}

/* ---------- 用户可见文本输出（Windows 控制台下做 UTF-8 → 控制台编码转换） ---------- */
#if defined(_WIN32)
static int g_out_is_console = 0;
static int g_err_is_console = 0;

static void win_write(FILE *stream, int is_console, const char *utf8, size_t len)
{
    if (is_console) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
        if (wlen > 0) {
            WCHAR *w = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
            if (w) {
                HANDLE h = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
                DWORD written = 0;
                MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, w, wlen);
                WriteConsoleW(h, w, (DWORD)wlen, &written, NULL);
                free(w);
                return;
            }
        }
    }
    fwrite(utf8, 1, len, stream);
    fflush(stream);
}
#endif

static void out_text(FILE *stream, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
#if defined(_WIN32)
    win_write(stream, stream == stderr ? g_err_is_console : g_out_is_console, buf, (size_t)n);
#else
    fwrite(buf, 1, (size_t)n, stream);
    fflush(stream);
#endif
}

static void msg(const char *fmt, ...)     /* 信息/错误 → stderr */
{
    char buf[4096];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
#if defined(_WIN32)
    win_write(stderr, g_err_is_console, buf, (size_t)n);
#else
    fwrite(buf, 1, (size_t)n, stderr);
    fflush(stderr);
#endif
}

/* ---------- 任务号（随机，每次运行不同；接收端靠它识别新任务） ----------
 * 注意：补发要在同一次运行内做（运行时输入帧号列表），这样任务号不变，
 * 接收端才会接着已收到的帧继续；重开一个进程会是新任务号，接收端按设计清空。 */
static void make_tid(char out[4])
{
    static const char AB[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned char rnd[3] = {0, 0, 0};
    int i;
#if defined(_WIN32)
    srand((unsigned)(GetTickCount() ^ (unsigned)GetCurrentProcessId() ^ (unsigned)time(NULL)));
    for (i = 0; i < 3; i++) rnd[i] = (unsigned char)(rand() & 0xff);
#else
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            ssize_t got = read(fd, rnd, sizeof(rnd));
            (void)got;
            close(fd);
        } else {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            srand((unsigned)tv.tv_usec ^ (unsigned)getpid());
            for (i = 0; i < 3; i++) rnd[i] = (unsigned char)(rand() & 0xff);
        }
    }
#endif
    for (i = 0; i < 3; i++) out[i] = AB[rnd[i] % 36];
    out[3] = '\0';
}

/* ============================================================
 * 终端：初始化 / 尺寸 / 按键 / 渲染
 * ============================================================ */

#if defined(_WIN32)

static HANDLE g_hout = INVALID_HANDLE_VALUE;
static HANDLE g_hin  = INVALID_HANDLE_VALUE;
static int    g_console_mode = 0;        /* stdout 是真控制台 */
static DWORD  g_old_out_mode = 0, g_old_in_mode = 0;
static CONSOLE_CURSOR_INFO g_old_cursor;
static int    g_cursor_saved = 0;
static CHAR_INFO *g_ci = NULL;           /* 控制台字符缓冲 */
static int    g_ci_cols = 0, g_ci_rows = 0;

static void plat_setup(void)
{
    DWORD mode;

    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hin  = GetStdHandle(STD_INPUT_HANDLE);

    g_out_is_console = (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0);
    g_err_is_console = (GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0);

    if (GetConsoleMode(g_hout, &mode)) {
        g_console_mode = 1;
        g_old_out_mode = mode;
        SetConsoleMode(g_hout, mode | ENABLE_PROCESSED_OUTPUT);
    }
    if (GetConsoleMode(g_hin, &mode)) {
        g_old_in_mode = mode;
        /* 关行缓冲与回显，_getch 才能拿到即时按键 */
        SetConsoleMode(g_hin, (mode & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT)) |
                              ENABLE_PROCESSED_INPUT);
        g_stdin_interactive = 1;
    }
    if (g_console_mode) {
        CONSOLE_CURSOR_INFO ci;
        if (GetConsoleCursorInfo(g_hout, &ci)) {
            g_old_cursor = ci;
            g_cursor_saved = 1;
            ci.bVisible = FALSE;
            SetConsoleCursorInfo(g_hout, &ci);
        }
        timeBeginPeriod(1);              /* Sleep 精度提到 1ms，帧率才稳 */
    }
}

static void plat_cleanup(void)
{
    if (g_console_mode) {
        timeEndPeriod(1);
        if (g_cursor_saved) SetConsoleCursorInfo(g_hout, &g_old_cursor);
        SetConsoleMode(g_hout, g_old_out_mode);
    }
    if (g_stdin_interactive) SetConsoleMode(g_hin, g_old_in_mode);
}

static void plat_term_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD mode;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

    *cols = 80;
    *rows = 24;
    /* 注意：这里必须自己取句柄，不能依赖 plat_setup 设置的全局量。
       启动阶段的尺寸检查发生在 plat_setup 之前（否则 --dry-run 和报错
       路径也会被切到原始模式/隐藏光标），之前依赖 g_console_mode 导致
       永远落回 80×24 的默认值。 */
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
        GetConsoleScreenBufferInfo(h, &info)) {
        *cols = info.srWindow.Right - info.srWindow.Left + 1;
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
}

static int plat_wait_key(long ms)
{
    long waited = 0;
    if (!g_stdin_interactive) { sleep_ms(ms); return 0; }
    while (waited < ms) {
        if (_kbhit()) return 1;
        Sleep(10);
        waited += 10;
    }
    return 0;
}

static int plat_read_key(void)
{
    /* 必须先看有没有按键再读：_getch() 在无键可读时会阻塞，
       那样一按键就会卡在读键循环里，屏幕再也不刷新
       （表现为"暂停住了但按空格也恢复不了"） */
    if (!_kbhit()) return -1;
    return _getch();
}

/* ---------- 参数编码还原（Windows）----------
 * argv 是当前 ANSI 代码页（中文系统 = GBK），而协议载荷要求 UTF-8。
 * 与其猜编码，不如直接拿宽字符命令行（GetCommandLineW → UTF-16）重新解码：
 * 无论控制台是 936 / 65001 / 别的代码页，文件名都能拿到正确的 Unicode，
 * 再统一转成 UTF-8 放进二维码。*/
static char **g_u8argv = NULL;

static void build_u8argv(int argc, char **argv)
{
    int wargc = 0;
    WCHAR **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    int i;

    if (!wargv) return;
    g_u8argv = (char **)calloc((size_t)argc, sizeof(char *));
    if (!g_u8argv) { LocalFree(wargv); return; }
    for (i = 0; i < argc && i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) continue;
        g_u8argv[i] = (char *)malloc((size_t)n);
        if (g_u8argv[i]) {
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, g_u8argv[i], n, NULL, NULL);
            /* 关键：直接把 argv 的指针指向 UTF-8 副本。
               不能按"索引"去查宽字符数组——getopt 默认会重排 argv（把文件名挪到
               后面），重排后 optind 对应的下标和原始顺序对不上，会取错参数。
               换指针后 getopt/重排/optarg 全程自然使用 UTF-8。 */
            argv[i] = g_u8argv[i];
        }
    }
    LocalFree(wargv);
}


/* 把 UTF-8 文本放进控制台缓冲（row/col 为窗口内坐标） */
static void ci_put_text(int cols, int rows, int row, int col, const char *utf8, WORD attr)
{
    int wlen, i;
    WCHAR *w;
    if (row < 0 || row >= rows || col >= cols) return;
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return;
    w = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wlen);
    for (i = 0; w[i] != 0 && col + i < cols; i++) {
        int idx = row * cols + col + i;
        g_ci[idx].Char.UnicodeChar = w[i];
        g_ci[idx].Attributes = attr;
    }
    free(w);
}

/* Win32 控制台渲染：用控制台属性画黑白块，不依赖 ANSI/VT */
static int console_draw(const uint8_t *qrcode, int size, int mode,
                        const char *status1, const char *status2,
                        int force_cols, int force_rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    int cols, rows, mods, qrows, qcols, y, x, i;
    WORD white_bg = (WORD)(BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);
    WORD dark_attr = 0;
    WORD light_attr = white_bg;
    WORD text_attr;
    SMALL_RECT region;
    COORD bsize, origin;
    char line[320];

    if (!GetConsoleScreenBufferInfo(g_hout, &info)) return 0;
    cols = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    (void)force_cols; (void)force_rows;

    mods = size + 2 * QUIET_ZONE;
    qrows = (mode == 1) ? mods : (mods + 1) / 2;
    qcols = (mode == 1) ? mods * 2 : mods;

    if (cols != g_ci_cols || rows != g_ci_rows) {
        CHAR_INFO *p = (CHAR_INFO *)realloc(g_ci, (size_t)cols * rows * sizeof(CHAR_INFO));
        if (!p) return 0;
        g_ci = p;
        g_ci_cols = cols;
        g_ci_rows = rows;
    }

    text_attr = info.wAttributes;
    for (i = 0; i < cols * rows; i++) {
        g_ci[i].Char.UnicodeChar = L' ';
        g_ci[i].Attributes = text_attr;
    }

    /*
     * 放不放得下的判定必须和启动检查（plan_frame）用同一个公式：
     * 二维码高 qrows + 状态行 2 行（--quiet 时不占）。
     * 之前这里写的是 qrows+3，比启动检查多要一行，导致"刚好卡在边界"
     * 的窗口会出现"启动放行、渲染报太小"的矛盾。
     */
    if (cols < qcols || rows < qrows + (status1 ? 2 : 0)) {
        /* 窗口太小：不画二维码，但程序仍在播放（调大窗口即刻恢复） */
        ci_put_text(cols, rows, 0, 0,
            "窗口太小，放不下二维码——程序仍在播放，调大窗口即刻恢复显示", text_attr);
        snprintf(line, sizeof(line), "需要 %d 列 x %d 行，当前 %d x %d（或减小 --chunk）",
                 qcols, qrows + 2, cols, rows);
        ci_put_text(cols, rows, 1, 0, line, text_attr);
        if (status1 && status2) {
            ci_put_text(cols, rows, 2, 0, status1, text_attr);
            ci_put_text(cols, rows, 3, 0, status2, text_attr);
        }
    } else {
        if (mode == 1) {
            /* 方块模式：每个模块两格，靠背景属性区分明暗 */
            for (y = 0; y < mods; y++) {
                for (x = 0; x < mods; x++) {
                    int dark = qrcodegen_getModule(qrcode, x - QUIET_ZONE, y - QUIET_ZONE) ? 1 : 0;
                    g_ci[(y * cols + x * 2)].Attributes = dark ? dark_attr : light_attr;
                    g_ci[(y * cols + x * 2 + 1)].Attributes = dark ? dark_attr : light_attr;
                }
            }
        } else {
            /* 半块模式：一个字符承载上下两个模块（黑字白底） */
            for (y = 0; y < mods; y += 2) {
                for (x = 0; x < mods; x++) {
                    int top = qrcodegen_getModule(qrcode, x - QUIET_ZONE, y - QUIET_ZONE) ? 1 : 0;
                    int bot = (y + 1 < mods) &&
                              qrcodegen_getModule(qrcode, x - QUIET_ZONE, y + 1 - QUIET_ZONE) ? 1 : 0;
                    WCHAR ch;
                    int idx2 = (y / 2) * cols + x;
                    if (top && bot)      ch = 0x2588;   /* █ */
                    else if (top)        ch = 0x2580;   /* ▀ */
                    else if (bot)        ch = 0x2584;   /* ▄ */
                    else                 ch = L' ';
                    g_ci[idx2].Char.UnicodeChar = ch;
                    g_ci[idx2].Attributes = light_attr;  /* 黑字白底 */
                }
            }
        }
        if (status1 && status2) {
            /* 状态行紧跟在二维码下方（正好 qrows + 2 行，与启动检查一致） */
            ci_put_text(cols, rows, qrows, 0, status1, text_attr);
            ci_put_text(cols, rows, qrows + 1, 0, status2, text_attr);
        }
    }

    region.Left = info.srWindow.Left;
    region.Top = info.srWindow.Top;
    region.Right = info.srWindow.Right;
    region.Bottom = info.srWindow.Bottom;
    bsize.X = (SHORT)cols;
    bsize.Y = (SHORT)rows;
    origin.X = 0;
    origin.Y = 0;
    if (!WriteConsoleOutputW(g_hout, g_ci, bsize, origin, &region)) return 0;
    return 1;
}

#else  /* ---------------- POSIX ---------------- */

static struct termios g_orig_termios;
static int g_have_termios = 0;
static int g_cursor_hidden = 0;

static void term_cleanup(void)
{
    if (g_have_termios) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_have_termios = 0;
    }
    if (g_cursor_hidden) {
        fputs("\033[?25h\033[0m", stdout);
        fflush(stdout);
        g_cursor_hidden = 0;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void plat_cleanup(void) { term_cleanup(); }

static void plat_setup(void)
{
    struct termios t;

    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
        g_have_termios = 1;
        t = g_orig_termios;
        t.c_lflag &= ~(unsigned)(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    if (ISATTY(STDIN_FILENO)) g_stdin_interactive = 1;
    fputs("\033[?25l", stdout);
    fflush(stdout);
    g_cursor_hidden = 1;

    atexit(term_cleanup);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
}

static void plat_term_size(int *cols, int *rows)
{
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

static int plat_wait_key(long ms)
{
    fd_set rfds;
    struct timeval tv;
    int r;
    if (!g_stdin_interactive) { sleep_ms(ms); return 0; }
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    return r > 0 && FD_ISSET(STDIN_FILENO, &rfds);
}

static int plat_read_key(void)
{
    /* 用零超时 select 保证一定不阻塞（万一终端没切到 VMIN=0 也不会卡住） */
    fd_set rfds;
    struct timeval tv;
    unsigned char c = 0;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) return -1;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return (int)c;
}


#endif

/* ============================================================
 * 渲染（ANSI 路径：Linux 全平台 + Windows 非控制台）
 * ============================================================ */

static void render_qr_ansi(const uint8_t *qrcode, int size, int mode, char *buf, size_t bufsz)
{
    const int mods = size + 2 * QUIET_ZONE;
    char *end = buf + (long)bufsz - 64;
    int x, y;
    size_t o = 0;

    if (mode == 1) {
        for (y = 0; y < mods; y++) {
            int cur = -1;
            for (x = 0; x < mods; x++) {
                int dark = qrcodegen_getModule(qrcode, x - QUIET_ZONE, y - QUIET_ZONE) ? 1 : 0;
                if (o > (size_t)(end - buf)) goto done;
                if (dark != cur) {
                    memcpy(buf + o, dark ? "\033[40m" : "\033[47m", 5);
                    o += 5;
                    cur = dark;
                }
                buf[o++] = ' ';
                buf[o++] = ' ';
            }
            memcpy(buf + o, "\033[0m\033[K\n", 8);
            o += 8;
        }
    } else {
        for (y = 0; y < mods; y += 2) {
            memcpy(buf + o, "\033[30;47m", 8);      /* 8 字节：ESC [ 3 0 ; 4 7 m */
            o += 8;
            for (x = 0; x < mods; x++) {
                int top = qrcodegen_getModule(qrcode, x - QUIET_ZONE, y - QUIET_ZONE) ? 1 : 0;
                int bot = (y + 1 < mods) &&
                          qrcodegen_getModule(qrcode, x - QUIET_ZONE, y + 1 - QUIET_ZONE) ? 1 : 0;
                if (o > (size_t)(end - buf)) goto done;
                /* 空格按 1 字节写：从字符串字面量 memcpy 3 字节会把 '\0' 一起拷进去，
                   那样 fputs 会在第一个空格处截断整帧输出 */
                if (top && bot) {
                    memcpy(buf + o, "\xe2\x96\x88", 3);
                    o += 3;
                } else if (top) {
                    memcpy(buf + o, "\xe2\x96\x80", 3);
                    o += 3;
                } else if (bot) {
                    memcpy(buf + o, "\xe2\x96\x84", 3);
                    o += 3;
                } else {
                    buf[o++] = ' ';
                }
            }
            memcpy(buf + o, "\033[0m\033[K\n", 8);
            o += 8;
        }
    }
done:
    buf[o] = '\0';
}

/* 统一的画帧入口：内部按平台/输出类型分派。
   状态行由主循环组装好后传入（status1/status2 为 NULL 表示 --quiet）。 */
static void draw_frame(const uint8_t *qrcode, int size, int mode, int use_console,
                       int force_cols, int force_rows,
                       const char *status1, const char *status2)
{
    static char *framebuf = NULL;

#if !defined(_WIN32)
    (void)force_cols;      /* 仅 Win32 控制台路径会用到（那里以窗口实际尺寸为准） */
    (void)force_rows;
#endif

    if (use_console) {
#if defined(_WIN32)
        if (console_draw(qrcode, size, mode, status1, status2, force_cols, force_rows)) {
            return;
        }
        /* 控制台绘制失败（例如句柄失效）则退回 ANSI */
#endif
    }

    if (!framebuf) framebuf = (char *)malloc(FRAME_BUF);
    if (!framebuf) return;

    render_qr_ansi(qrcode, size, mode, framebuf, FRAME_BUF);

    fputs("\033[H", stdout);        /* 光标回左上角，整帧重画 */
    fputs(framebuf, stdout);
    if (status1 && status2) {
        fputs("\033[0m", stdout);
        fputs(status1, stdout);
        fputs("\033[K\n", stdout);
        fputs(status2, stdout);
        fputs("\033[K\n", stdout);
    }
    fputs("\033[J", stdout);
    fflush(stdout);
}

/* ============================================================
 * 键盘处理（两平台共用）
 * ============================================================ */

/*
 * 解析补发帧号列表，如 "1,2,3,52,109"，也支持区间写法与混用： "1-5, 8, 20-22"。
 * 帧号是 1 基（与接收端显示一致）。成功返回 malloc 的 0 基帧号数组（长度 *out_len），
 * 失败返回 NULL 并把原因写进 err。
 */
static long *parse_list(const char *s, size_t *out_len, long total,
                        char *err, size_t errsz)
{
    long *arr = NULL;
    size_t n = 0, cap = 0;
    const char *p = s;

    while (*p) {
        char *end;
        long v, v2;

        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        v = strtol(p, &end, 10);
        if (end == p) {
            snprintf(err, errsz, "列表里有无法识别的内容：'%.16s'", p);
            free(arr);
            return NULL;
        }
        p = end;
        if (*p == '-') {                      /* 区间写法：1-5 */
            p++;
            v2 = strtol(p, &end, 10);
            if (end == p) {
                snprintf(err, errsz, "区间写法不完整（示例：1-5）");
                free(arr);
                return NULL;
            }
            p = end;
        } else {
            v2 = v;
        }
        if (v < 1 || v2 < 1 || v > total || v2 > total) {
            snprintf(err, errsz,
                     "帧号超出范围 1-%ld（列表里出现 %ld-%ld）——"
                     "请确认发送端的 --chunk 与接收端一致，否则帧号对不上",
                     total, v, v2);
            free(arr);
            return NULL;
        }
        if (v2 < v) { long t = v; v = v2; v2 = t; }
        for (; v <= v2; v++) {
            if (n == cap) {
                long *p2;
                cap = cap ? cap * 2 : 64;
                p2 = (long *)realloc(arr, cap * sizeof(long));
                if (!p2) {
                    snprintf(err, errsz, "内存不足");
                    free(arr);
                    return NULL;
                }
                arr = p2;
            }
            arr[n++] = v - 1;                 /* 转成 0 基存放 */
        }
    }
    if (n == 0) {
        snprintf(err, errsz, "列表为空");
        free(arr);
        return NULL;
    }
    *out_len = n;
    return arr;
}

/*
 * 往输入缓冲追加一个字符，容量不够就翻倍扩容。
 * 缺帧列表可能很长（粘贴几千上万个帧号），固定缓冲会被静默截断——
 * 而且可能把最后一个数字切一半（...1999,2000 截成 ...1999,20），
 * 补错帧还看不出来。这里动态扩容，杜绝这类问题。
 */
static int jump_append(char **buf, size_t *cap, int *len, char c)
{
    if ((size_t)*len + 2 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 1024;
        char *p;
        while ((size_t)*len + 2 > ncap) ncap *= 2;
        p = (char *)realloc(*buf, ncap);
        if (!p) return 0;
        *buf = p;
        *cap = ncap;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
    return 1;
}

/*
 * 键盘处理。返回 1 表示请求退出。
 * 列表模式下（*list 非空）逐帧范围限定在列表内；回车提交的输入里
 * 带分隔符（逗号/空格/区间）就当作补发列表，单个数字仍是跳帧。
 */
static int handle_key(int c, int *paused, long *idx, long total,
                      char **jump, int *jump_len, size_t *jump_cap,
                      long **list, size_t *list_len, size_t *list_pos,
                      char *notice, size_t notice_sz)
{
    if (c < 0) return 0;
    if (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */) return 1;

    /* 空格：永远只管暂停/继续（最常用的键，不能被输入缓冲抢走）。
       若缓冲里有没提交的半截输入（比如打了一半的帧号），先清掉——
       否则会出现"按空格没反应、被困住"的情况（缓冲把空格当分隔符吃掉了）。
       帧号列表请用逗号分隔、粘贴后直接回车。 */
    if (c == ' ') {
        *jump_len = 0;
        (*jump)[0] = 0;
        *paused = !*paused;
        return 0;
    }
    /* f：退出补发模式，恢复完整播放 */
    if (c == 'f' || c == 'F') {
        if (*list) {
            free(*list);
            *list = NULL;
            *list_len = 0;
            *list_pos = 0;
            snprintf(notice, notice_sz, "已恢复完整播放");
        } else {
            snprintf(notice, notice_sz, "当前已是完整播放");
        }
        return 0;
    }
    if (c == '.' || c == '>' || c == 'n') {
        if (*list) {
            *list_pos = (*list_pos + 1) % *list_len;
            *idx = (*list)[*list_pos];
        } else {
            *idx = (*idx + 1) % total;
        }
        *paused = 1;
        *jump_len = 0;
        (*jump)[0] = 0;
        notice[0] = '\0';
        return 0;
    }
    /* 逗号：正在输入帧号列表时算分隔符，空输入时才当"上一帧" */
    if ((c == ',' && *jump_len == 0) || c == '<' || c == 'p') {
        if (*list) {
            *list_pos = (*list_pos + *list_len - 1) % *list_len;
            *idx = (*list)[*list_pos];
        } else {
            *idx = (*idx - 1 + total) % total;
        }
        *paused = 1;
        *jump_len = 0;
        (*jump)[0] = 0;
        notice[0] = '\0';
        return 0;
    }
    /* 输入缓冲：数字、逗号、区间连字符都可以进（用于粘贴缺帧列表）；
       空格不进缓冲——它是暂停键（见上） */
    if ((c >= '0' && c <= '9') || c == ',' || c == '-') {
        if (!jump_append(jump, jump_cap, jump_len, (char)c)) {
            snprintf(notice, notice_sz, "内存不足，输入被截断");
        }
        *paused = 1;
        return 0;
    }
    if (c == '\r' || c == '\n') {
        if (*jump_len == 0) {
            /* 空输入 + 回车：如果在补发模式，恢复完整播放 */
            if (*list) {
                free(*list);
                *list = NULL;
                *list_len = 0;
                *list_pos = 0;
                snprintf(notice, notice_sz, "已恢复完整播放");
            }
            return 0;
        }
        if (strpbrk(*jump, ",- 	") != NULL) {
            /* 含分隔符：当作补发帧号列表（1 基，来自接收端「导出缺帧列表」） */
            char lerr[256];
            size_t nlen = 0;
            long *nl = parse_list(*jump, &nlen, total, lerr, sizeof(lerr));
            if (nl) {
                free(*list);
                *list = nl;
                *list_len = nlen;
                *list_pos = 0;
                *idx = nl[0];
                *paused = 0;          /* 进补发模式就直接开始播，不用再按空格 */
                snprintf(notice, notice_sz, "补发模式：%lu 帧，f 恢复全部",
                         (unsigned long)nlen);
            } else {
                snprintf(notice, notice_sz, "%s", lerr);
            }
        } else {
            /* 单个数字：跳到该帧（原有行为） */
            long n = atol(*jump);
            if (n >= 1 && n <= total) {
                if (*list) {
                    size_t i;
                    int found = 0;
                    for (i = 0; i < *list_len; i++) {
                        if ((*list)[i] == n - 1) { *list_pos = i; found = 1; break; }
                    }
                    if (found) {
                        *idx = n - 1;
                        notice[0] = '\0';
                    } else {
                        snprintf(notice, notice_sz, "帧 %ld 不在补发列表里", n);
                    }
                } else {
                    *idx = n - 1;
                    notice[0] = '\0';
                }
            } else {
                snprintf(notice, notice_sz, "帧号超出范围（1-%ld）", total);
            }
            *paused = 1;
        }
        *jump_len = 0;
        (*jump)[0] = 0;
        return 0;
    }
    if (c == 8 || c == 127) {
        if (*jump_len > 0) (*jump)[--(*jump_len)] = 0;
        return 0;
    }
    return 0;
}

/* ============================================================
 * 文件读取 / base64 / 规划
 * ============================================================ */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        msg("内存不足（%lu 字节）\n", (unsigned long)n);
        exit(1);
    }
    return p;
}

static void fmt_mmss(double sec, char *out, size_t outsz)
{
    long s = (long)(sec + 0.5);
    if (s < 0) s = 0;
    if (s < 3600) snprintf(out, outsz, "%ld:%02ld", s / 60, s % 60);
    else snprintf(out, outsz, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
}

/* ---------- zlib 压缩（miniz，协议 v2.0 用）---------- */
static unsigned char *try_compress(const unsigned char *data, size_t len, size_t *out_len)
{
    mz_ulong clen = mz_compressBound((mz_ulong)len);
    unsigned char *buf = (unsigned char *)malloc((size_t)clen);
    if (!buf) return NULL;
    /* 压完不比原样小就不值得用（.7z/.zip 这类已压缩文件会走到这里） */
    if (mz_compress2(buf, &clen, data, (mz_ulong)len, MZ_BEST_COMPRESSION) != MZ_OK ||
        (size_t)clen >= len) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)clen;
    return buf;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/*
 * 协议 v2.0 帧载荷（二进制，实测与 jsQR 的 binaryData 对接）：
 *
 *   'B' | flags | tid(3) | nameLen(1) | name(UTF-8) | total(4) | idx(4) |
 *   realLen(4) | origSize(4) | data(补足到 chunk)
 *
 * 多字节整数一律小端。data 段补 0 补足到固定长度，接收端按 realLen 截断。
 * flags bit0：data 段是 zlib 压缩后的数据。
 * 数据段是原始字节——不再经过 base64，省掉 33% 冗余，也不怕任何特殊字符。
 */
static size_t build_frame(uint8_t *out, const char *tid,
                          const char *name, size_t name_len,
                          uint32_t total, uint32_t idx,
                          uint32_t real_len, uint32_t orig_size, int compressed,
                          const unsigned char *data, size_t data_len, size_t padded_len)
{
    size_t h = 0;
    out[h++] = 'B';
    out[h++] = (uint8_t)(compressed ? 1 : 0);
    memcpy(out + h, tid, 3); h += 3;
    out[h++] = (uint8_t)name_len;
    memcpy(out + h, name, name_len); h += name_len;
    put_le32(out + h, total); h += 4;
    put_le32(out + h, idx); h += 4;
    put_le32(out + h, real_len); h += 4;
    put_le32(out + h, orig_size); h += 4;
    memcpy(out + h, data, data_len); h += data_len;
    if (padded_len > data_len) {
        memset(out + h, 0, padded_len - data_len);
        h += padded_len - data_len;
    }
    return h;
}

/*
 * 打开输入文件。Windows 上用宽字符接口 _wfopen：
 * 此时 path 已经是 UTF-8（见 build_u8argv），而系统 fopen 按 ANSI 代码页解释路径，
 * 中文文件名会"文件不存在"；先转回 UTF-16 再开，任何文件名都能打开。
 */
static FILE *open_input(const char *path)
{
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    WCHAR *w;
    FILE *f;
    if (n <= 0) return NULL;
    w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    f = _wfopen(w, L"rb");
    free(w);
    return f;
#else
    return fopen(path, "rb");
#endif
}

static unsigned char *read_all(const char *path, size_t *outlen)
{
    FILE *f = stdin;
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf;

    if (path) {
        f = open_input(path);
        if (!f) {
            msg("无法打开文件 %s：%s\n", path, strerror(errno));
            return NULL;
        }
    }
#if !defined(_WIN32)
    {
        struct stat st;
        if (fstat(FILENO(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            cap = (size_t)st.st_size + 1;
        }
    }
#endif

    buf = (unsigned char *)xmalloc(cap);
    for (;;) {
        size_t n;
        if (len == cap) {
            cap *= 2;
            buf = (unsigned char *)realloc(buf, cap);
            if (!buf) {
                msg("内存不足\n");
                if (path) fclose(f);
                return NULL;
            }
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            if (ferror(f)) {
                msg("读取失败：%s\n", strerror(errno));
                if (path) fclose(f);
                free(buf);
                return NULL;
            }
            break;
        }
    }
    if (path) fclose(f);

    *outlen = len;
    return buf;
}

static const char *base_name(const char *path)
{
    const char *p1 = strrchr(path, '/');
#if defined(_WIN32)
    const char *p2 = strrchr(path, '\\');
    if (p2 && (!p1 || p2 > p1)) p1 = p2;
#endif
    return p1 ? p1 + 1 : path;
}

/*
 * Windows 上命令行参数可能是当前 ANSI 代码页（中文系统 = GBK），而协议载荷要求
 * UTF-8（接收端 jsQR 按 UTF-8 解码）。不转换的话，中文文件名会变成非法 UTF-8，
 * 接收端连帧都解不出来（不是显示乱码，是整帧解码失败）。
 * 注意：系统若开了"UTF-8 代码页"（65001），argv 本来就是 UTF-8，
 * 此时不能再转（会双重转码变乱码）——所以先校验，只有不是合法 UTF-8 才转。
 */
#if defined(_WIN32)
static int is_valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (p[0] < 0x80) { p++; continue; }
        if ((p[0] & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) return 0;
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
            p += 3;
        } else if ((p[0] & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                (p[3] & 0xC0) != 0x80) return 0;
            p += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static void name_to_utf8(char *inout, size_t bufsz)
{
    int wlen;
    WCHAR *w;
    char out[512];
    int n;

    if (is_valid_utf8(inout)) return;      /* 已经是 UTF-8，别动 */
    wlen = MultiByteToWideChar(CP_ACP, 0, inout, -1, NULL, 0);
    if (wlen <= 0) return;
    w = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (!w) return;
    MultiByteToWideChar(CP_ACP, 0, inout, -1, w, wlen);
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)sizeof(out), NULL, NULL);
    free(w);
    if (n > 0) snprintf(inout, bufsz, "%s", out);
}
#endif

static void sanitize_name(char *name)
{
    size_t i, n = strlen(name);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '|' || c < 0x20 || c == 0x7f) name[i] = '_';
    }
    if (n > MAX_NAME_LEN) {
        name[MAX_NAME_LEN] = '\0';
        msg("提示：文件名过长，载荷中截断为前 %d 字节\n", MAX_NAME_LEN);
    }
}

static int plan_frame(size_t name_len, size_t chunk, int mode, size_t payload_len,
                      int *qr_size, int *need_cols, int *need_rows)
{
    size_t est_len, i;
    static uint8_t data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t code[qrcodegen_BUFFER_LEN_MAX];
    int size, mods;

    (void)payload_len;      /* 帧大小只由 chunk 和文件名决定 */

    /* v2.0 二进制帧：帧头固定 22 字节（'B' + flags + tid×3 + nameLen + 四个 uint32）
       + 文件名字节 + 数据段（补足到 chunk 字节） */
    est_len = 22 + name_len + chunk;
    if (est_len >= sizeof(data)) return 0;

    for (i = 0; i < est_len; i++) data[i] = 'A';
    if (!qrcodegen_encodeBinary(data, est_len, code, qrcodegen_Ecc_LOW,
                                qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                qrcodegen_Mask_AUTO, true)) {
        return 0;
    }
    size = qrcodegen_getSize(code);

    *qr_size = size;
    mods = size + 2 * QUIET_ZONE;
    *need_cols = (mode == 1) ? mods * 2 : mods;
    *need_rows = ((mode == 1) ? mods : (mods + 1) / 2) + 2;
    return 1;
}

/* ============================================================
 * 帮助
 * ============================================================ */

static void usage(FILE *out, const char *argv0)
{
    out_text(out,
        "%s %s — AirScan-QR 终端发送端（终端里显示动态二维码，手机扫码接收）\n"
        "\n"
        "用法: %s [选项] 文件\n"
        "      cat 文件 | %s --name 文件名 -\n"
        "\n"
        "选项:\n"
        "  -f, --fps N      帧率 1-60，默认 8\n"
        "  -c, --chunk N    单帧数据字节数 %d-%d，默认 200（数据按原始字节传，不再 base64）\n"
        "  -n, --name NAME  接收端显示并保存的文件名（默认取路径文件名）\n"
        "  -t, --tid XXX    任务号，默认随机三位；接收端靠它识别新任务\n"
        "  -m, --mode M     half=半块字符（默认，需 UTF-8 终端）；block=方块（兼容更旧的终端）\n"
        "  -s, --start N    从第 N 帧开始\n"
        "      --once       每帧播一轮后退出（默认循环）\n"
        "  -q, --quiet      不显示状态行与提示行\n"
        "      --dry-run    只打印传输规划后退出（不发送）\n"
        "      --no-compress  关闭自动压缩（默认会先试压，压得更小才用）\n"
        "      --cols N     手动指定终端列数（默认自动探测）\n"
        "      --rows N     手动指定终端行数（默认自动探测）\n"
        "  -h, --help       显示帮助\n"
        "  -V, --version    显示版本\n"
        "\n"
        "交互按键:\n"
        "  空格           暂停 / 继续\n"
        "  .  或  >       下一帧（自动暂停）\n"
        "  ,  或  <       上一帧（自动暂停）\n"
        "  数字 + 回车    跳到指定帧（自动暂停）\n"
        "  帧号列表+回车  进入补发模式：如 1,2,3,52,109 或 8,20-22\n"
        "                  从接收端「导出缺帧列表」复制粘贴进来即可；\n"
        "                  只循环播放列表内的帧（任务号不变，接收端不清空）\n"
        "  f  或 空回车   补发模式下恢复完整播放\n"
        "  q  或 Ctrl-C   退出\n"
        "\n"
        "说明:\n"
        "  * 接收端：手机浏览器打开 index.html，切到“接收”页用摄像头扫描；\n"
        "  * 协议 v2.0：二进制帧头 + 原始字节，可选 zlib 压缩；\n"
        "    接收端需用配套版本的 index.html；\n"
        "  * 扫码建议：终端字体调大、别用反色/透明背景主题、别让二维码贴边被裁；\n"
        "  * Windows 下真控制台走 Win32 控制台 API 渲染，XP 起的 cmd 均可用。\n",
        PROG_NAME, PROG_VERSION, argv0, argv0, MIN_CHUNK, MAX_CHUNK);
}

/* ============================================================
 * 主流程
 * ============================================================ */

int main(int argc, char *argv[])
{
    double fps = 8.0;
    size_t chunk = 200;
    const char *opt_name = NULL;
    const char *opt_tid = NULL;
    int mode = -1;
    long start_frame = 1;
    int once = 0, quiet = 0, dry_run = 0;
    int use_compress = 1;         /* 默认自动压缩：压完更小才用 */
    int force_cols = 0, force_rows = 0;
    const char *path = NULL;
    int opt;
    int use_console = 0;

    static const struct option longopts[] = {
        {"fps",     required_argument, NULL, 'f'},
        {"chunk",   required_argument, NULL, 'c'},
        {"name",    required_argument, NULL, 'n'},
        {"tid",     required_argument, NULL, 't'},
        {"mode",    required_argument, NULL, 'm'},
        {"start",   required_argument, NULL, 's'},
        {"once",    no_argument,       NULL, '1'},
        {"quiet",   no_argument,       NULL, 'q'},
        {"dry-run", no_argument,       NULL, 'D'},
        {"no-compress", no_argument,   NULL, 'Z'},
        {"compress", no_argument,      NULL, 'z'},
        {"cols",    required_argument, NULL, 'C'},
        {"rows",    required_argument, NULL, 'R'},
        {"help",    no_argument,       NULL, 'h'},
        {"version", no_argument,       NULL, 'V'},
        {NULL, 0, NULL, 0}
    };

#if defined(_WIN32)
    /* 统计/诊断信息走控制台时按控制台编码输出；这里先把两个标准句柄的
       控制台属性探测出来（plat_setup 之前也可能有输出） */
    {
        DWORD m;
        g_out_is_console = (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m) != 0);
        g_err_is_console = (GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &m) != 0);
    }
#endif

#if defined(_WIN32)
    build_u8argv(argc, argv);     /* 必须在 getopt 之前：它会把 argv 重排，之后再按索
                                     换指针就对不上号了。这里先换成 UTF-8，getopt 全程
                                     就都工作在正确的参数上（中文名不乱码） */
#endif
    while ((opt = getopt_long(argc, argv, "f:c:n:t:m:s:1qDhVC:R:Zz", longopts, NULL)) != -1) {
        switch (opt) {
        case 'f': fps = atof(optarg); break;
        case 'c': chunk = (size_t)atol(optarg); break;
        case 'n': opt_name = optarg; break;
        case 't': opt_tid = optarg; break;
        case 'm':
            if (strcmp(optarg, "half") == 0) mode = 0;
            else if (strcmp(optarg, "block") == 0) mode = 1;
            else { msg("未知渲染模式：%s（可选 half / block）\n", optarg); return 1; }
            break;
        case 's': start_frame = atol(optarg); break;
        case '1': once = 1; break;
        case 'q': quiet = 1; break;
        case 'D': dry_run = 1; break;
        case 'Z': use_compress = 0; break;
        case 'z': use_compress = 1; break;
        case 'C': force_cols = atoi(optarg); break;
        case 'R': force_rows = atoi(optarg); break;
        case 'h': usage(stdout, argv[0]); return 0;
        case 'V': out_text(stdout, "%s %s\n", PROG_NAME, PROG_VERSION); return 0;
        default:  usage(stderr, argv[0]); return 1;
        }
    }

    if (optind < argc) path = argv[optind];
    if (!path && ISATTY(FILENO(stdin))) { usage(stderr, argv[0]); return 1; }
    if (path && strcmp(path, "-") == 0) path = NULL;
    if (!path && !opt_name) opt_name = "stdin.bin";

    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    if (chunk < MIN_CHUNK || chunk > MAX_CHUNK) {
        msg("单帧数据量需在 %d-%d 之间（当前 %lu）\n", MIN_CHUNK, MAX_CHUNK, (unsigned long)chunk);
        return 1;
    }

    /* 渲染模式自动选择：UTF-8 终端用半块，否则用方块 */
    if (mode < 0) {
#if defined(_WIN32)
        mode = 0;      /* Windows 控制台是 Unicode，半块字符一定可用；
                          Git Bash/mintty 也是 UTF-8，同样用半块 */
#else
        const char *cs;
        setlocale(LC_CTYPE, "");
        cs = nl_langinfo(CODESET);
        mode = (cs && (strcmp(cs, "UTF-8") == 0 || strcmp(cs, "utf8") == 0)) ? 0 : 1;
#endif
    }

    {
        size_t flen = 0;
        unsigned char *data = read_all(path, &flen);
        char name[512];
        char tid[4];
        unsigned char *payload_data;   /* 实际要传的数据（可能是压缩后的） */
        size_t payload_len;
        int compressed = 0;
        long total;
        int cols, rows, qr_size = 0, need_cols = 0, need_rows = 0;
        char eta[32];
        long idx;
        int paused = 0;
        char *jump = (char *)malloc(1024);  /* 输入缓冲：动态扩容，粘贴上万帧也不截断 */
        size_t jump_cap = 1024;
        int jump_len = 0;
        long shown = 0;
        double deadline;
        long *list = NULL;            /* 补发列表（0 基帧号），NULL = 正常整轮播放 */
        size_t list_len = 0, list_pos = 0;
        char notice[320];             /* 提示行（要放得下 parse_list 的错误信息） */

        if (!data) return 1;
        if (flen == 0) {
            msg("输入为空，没有可发送的内容\n");
            free(data);
            return 1;
        }
        if (flen > MAX_FILE) {
            msg("文件过大（%lu 字节）：屏幕→摄像头链路不适合超过 %lu MB 的数据，\n"
                "请改用其他通道（可用 --dry-run 估算实际耗时）。\n",
                (unsigned long)flen, (unsigned long)(MAX_FILE / (1024 * 1024)));
            free(data);
            return 1;
        }

        if (opt_name) snprintf(name, sizeof(name), "%s", opt_name);
        else          snprintf(name, sizeof(name), "%s", base_name(path));
#if defined(_WIN32)
        name_to_utf8(name, sizeof(name));   /* GBK → UTF-8，否则中文名会让整帧解不出来 */
#endif
        sanitize_name(name);

        if (opt_tid) snprintf(tid, sizeof(tid), "%s", opt_tid);
        else         make_tid(tid);

        /* 压缩（默认自动：压完更小才用；.7z/.zip 这类压不动就原样传） */
        payload_data = data;
        payload_len = flen;
        if (use_compress) {
            size_t clen = 0;
            unsigned char *c = try_compress(data, flen, &clen);
            if (c) { payload_data = c; payload_len = clen; compressed = 1; }
        }
        total = (long)((payload_len + chunk - 1) / chunk);
        if (total < 1) total = 1;

        cols = force_cols;
        rows = force_rows;
        if (cols <= 0 || rows <= 0) {
            int c2, r2;
            plat_term_size(&c2, &r2);
            if (cols <= 0) cols = c2;
            if (rows <= 0) rows = r2;
        }

        if (!plan_frame(strlen(name), chunk, mode, payload_len, &qr_size, &need_cols, &need_rows)) {
            msg("单帧数据过大，无法编码为二维码（chunk=%lu，文件名 %lu 字节）\n",
                (unsigned long)chunk, (unsigned long)strlen(name));
            free(payload_data);
            if (compressed) free(data);
            return 1;
        }

        fmt_mmss((double)total / fps, eta, sizeof(eta));
        msg("文件     : %s（%lu 字节）\n"
            "任务号   : %s\n",
            name, (unsigned long)flen, tid);
        if (compressed) {
            msg("压缩     : 开启，%lu → %lu 字节（原始数据的 %.0f%%）\n",
                (unsigned long)flen, (unsigned long)payload_len,
                100.0 * (double)payload_len / (double)flen);
        } else if (use_compress) {
            msg("压缩     : 跳过（压完不比原样小，改传原始数据）\n");
        } else {
            msg("压缩     : 已关闭（--no-compress）\n");
        }
        msg("分帧     : %ld 帧 × %lu 字节（末帧补足等长，各帧二维码一样大）\n"
            "帧率     : %.1f fps，循环一轮约 %s\n"
            "二维码   : 版本 %d（%d×%d 模块），渲染模式 %s\n"
            "终端需求 : %d 列 × %d 行（当前 %d × %d）\n",
            total, (unsigned long)chunk, fps, eta,
            (qr_size - 17) / 4, qr_size, qr_size, mode == 0 ? "half" : "block",
            need_cols, need_rows, cols, rows);

        if (dry_run) {
            free(payload_data);
            if (compressed) free(data);
            free(list);
            return 0;
        }

        if (need_cols > cols || need_rows > rows) {
            size_t c, suggest = 0;
            int s_qsz = 0, s_cols = 0, s_rows = 0;
            for (c = chunk; c >= MIN_CHUNK; c -= 4) {
                if (plan_frame(strlen(name), c, mode, payload_len, &s_qsz, &s_cols, &s_rows) &&
                    s_cols <= cols && s_rows <= rows) {
                    suggest = c;
                    break;
                }
            }
            msg("\n终端放不下：需要 %d 列 × %d 行，当前只有 %d 列 × %d 行。\n",
                need_cols, need_rows, cols, rows);
            if (suggest) {
                msg("建议：--chunk %lu（二维码缩到 %d×%d 模块），或调大终端窗口/调小字体。\n",
                    (unsigned long)suggest, s_qsz, s_qsz);
            } else {
                msg("即使按最小分帧也放不下，请调大终端窗口或改小字体后再试。\n");
            }
            free(payload_data);
            if (compressed) free(data);
            return 1;
        }

        plat_setup();
#if defined(_WIN32)
        use_console = g_console_mode;
        if (use_console) {
            /* 控制台渲染以窗口实际尺寸为准，重新取一次 */
            int c2, r2;
            plat_term_size(&c2, &r2);
            cols = c2;
            rows = r2;
        }
        atexit(plat_cleanup);
        SetConsoleCtrlHandler(NULL, FALSE);   /* 保持默认 Ctrl-C 行为（进程直接退出） */
#else
        use_console = 0;
#endif

        if (!quiet) {
            msg("开始发送：接收端用手机浏览器打开 index.html 切到“接收”页扫描。\n");
        }

        idx = start_frame - 1;
        if (idx < 0) idx = 0;
        if (idx >= total) idx = total - 1;
        jump[0] = 0;
        notice[0] = '\0';
        deadline = plat_now();

        while (!g_quit) {
            static uint8_t payload[8192];
            static uint8_t qrdata[qrcodegen_BUFFER_LEN_MAX];
            static uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
            int size, n;
            long wait_ms;
            char st1[400], st2[200];

            /* 协议 v2.0：二进制帧头 + 原始字节数据（不再走 base64，
               省掉 33% 冗余，任何"特殊字符"都不会破坏解析）。
               数据段补足到 chunk 字节 → 各帧二维码大小完全一致；
               接收端按 realLen 截断，填充数据不会写进文件。 */
            {
                size_t off = (size_t)idx * chunk;
                size_t remain = payload_len - off;
                size_t real = remain > chunk ? chunk : remain;
                size_t padded = (total > 1) ? chunk : real;   /* 单帧文件不补 */
                n = (int)build_frame(payload, tid, name, strlen(name),
                                     (uint32_t)total, (uint32_t)idx, (uint32_t)real,
                                     (uint32_t)flen, compressed,
                                     payload_data + off, real, padded);
            }
            if (n <= 0 || (size_t)n > sizeof(payload)) {
                msg("载荷过长（%d 字节），请减小 --chunk 或文件名\n", n);
                break;
            }

            memcpy(qrdata, payload, (size_t)n);
            if (!qrcodegen_encodeBinary(qrdata, (size_t)n, qrcode,
                                        qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                        qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
                msg("二维码编码失败：该帧数据或文件名过长\n");
                break;
            }
            size = qrcodegen_getSize(qrcode);

            /* 状态行在这里统一组装，ANSI 与 Win32 控制台两条渲染路径共用 */
            if (quiet) {
                st1[0] = '\0';
                st2[0] = '\0';
            } else {
                if (list) {
                    snprintf(st1, sizeof(st1), "[补发 %lu/%lu] 帧%ld/%ld %.1ffps %luc/f v%d %s",
                             (unsigned long)(list_pos + 1), (unsigned long)list_len,
                             idx + 1, total, fps, (unsigned long)chunk,
                             (size - 17) / 4, paused ? "已暂停" : "");
                } else {
                    snprintf(st1, sizeof(st1), "[%ld/%ld] %.1ffps %luc/f v%d %s",
                             idx + 1, total, fps, (unsigned long)chunk,
                             (size - 17) / 4, paused ? "已暂停" : "");
                }
                if (notice[0]) {
                    strncat(st1, " ", sizeof(st1) - strlen(st1) - 1);
                    strncat(st1, notice, sizeof(st1) - strlen(st1) - 1);
                }
                if (jump_len > 0) {
                    const char *tail = jump + (jump_len > 24 ? jump_len - 24 : 0);
                    strncat(st1, " 输入:", sizeof(st1) - strlen(st1) - 1);
                    strncat(st1, tail, sizeof(st1) - strlen(st1) - 1);
                }
                snprintf(st2, sizeof(st2),
                         "%s空格=暂停  ,=上一帧  .=下一帧  数字/帧号列表+回车=跳帧或补发  %s  q=退出",
                         list ? "(补发列表内移动) " : "",
                         list ? "f=恢复全部" : "f=全部");
            }

            draw_frame(qrcode, size, mode, use_console, cols, rows,
                       quiet ? NULL : st1, quiet ? NULL : st2);
            shown++;

            if (paused) {
                deadline = plat_now();
                wait_ms = 100;
            } else {
                deadline += 1.0 / fps;
                wait_ms = (long)((deadline - plat_now()) * 1000.0 + 0.5);
                if (wait_ms < 1) wait_ms = 1;
            }

            if (plat_wait_key(wait_ms)) {
                int c;
                /* 一次把缓冲里的按键读完：粘贴整段缺帧列表时能一下吃进去 */
                while ((c = plat_read_key()) >= 0) {
                    if (handle_key(c, &paused, &idx, total, &jump, &jump_len, &jump_cap,
                                   &list, &list_len, &list_pos, notice, sizeof(notice))) {
                        g_quit = 1;
                        break;
                    }
                }
                deadline = plat_now();
                if (g_quit) break;
                /* 按键处理的这一帧不自动推进：刚跳到/刚设定的帧要完整显示一帧，
                   否则进补发模式时列表的第一帧会被当场跳过去 */
                continue;
            }

            if (g_quit) break;

            if (!paused) {
                if (list) {
                    /* 补发模式：按列表顺序推进（--once 时只走一遍列表） */
                    if (once) {
                        if (list_pos + 1 >= list_len) break;
                        list_pos++;
                    } else {
                        list_pos = (list_pos + 1) % list_len;
                    }
                    idx = list[list_pos];
                } else if (once) {
                    if (idx + 1 >= total) break;
                    idx++;
                } else {
                    idx = (idx + 1) % total;
                }
            }
        }

        plat_cleanup();
        if (!use_console) {
            /* 控制台渲染模式没有开 VT，ANSI 序列不会被解释，别输出 */
            fputs("\033[H\033[2J\033[?25h\033[0m", stdout);
            fflush(stdout);
        }
        if (!quiet) msg("发送结束（共播放 %ld 帧次）。\n", shown);
        free(payload_data);
        if (compressed) free(data);
        free(list);
    }
    return 0;
}

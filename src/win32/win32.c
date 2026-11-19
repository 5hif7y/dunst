/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file
 * @brief Win32 output backend.
 *
 * X11 concept                         Win32 here
 * ----------------------------------  -----------------------------------------
 * override_redirect window            WS_POPUP + TOPMOST|TOOLWINDOW|NOACTIVATE
 * ARGB visual + compositor            WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA)
 *                                     (cairo ARGB32 is premultiplied BGRA: the exact
 *                                     format UpdateLayeredWindow wants)
 * _NET_WM_WINDOW_OPACITY              BLENDFUNCTION.SourceConstantAlpha
 * Xinerama/RandR screens              EnumDisplayMonitors, work area, GetDpiForMonitor
 * follow = mouse / keyboard           GetCursorPos / GetForegroundWindow -> monitor
 * XScreenSaverQueryInfo (idle)        GetLastInputInfo
 * _NET_WM_STATE_FULLSCREEN            SHQueryUserNotificationState (D3D full screen,
 *                                     presentation mode, busy)
 * X connection fd in the GMainLoop    G_WIN32_MSG_HANDLE poll fd: GLib wakes on
 *                                     thread messages, we PeekMessage them
 * ButtonRelease                       WM_{L,M,R}BUTTONUP
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <math.h>
#include <glib.h>

#include "win32.h"
#include "../draw.h"
#include "../dunst.h"
#include "../input.h"
#include "../log.h"
#include "../settings.h"

#define BTN_LEFT   (0x110)
#define BTN_RIGHT  (0x111)
#define BTN_MIDDLE (0x112)

#define MAX_SCREENS 16

struct window_w32 {
        HWND hwnd;
        cairo_surface_t *measure_srf; /* draw.c only measures text with this context */
        cairo_t *c_ctx;
        bool visible;
        int cur_screen;
};

struct w32_source {
        GSource source;
        GPollFD fd;
};

static struct screen_info screens[MAX_SCREENS];
static int screens_len;
static GSource *msg_source;
static const wchar_t *CLASS_NAME = L"dunst";

/* ---- screens ------------------------------------------------------------ */

static BOOL CALLBACK monitor_enum(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp)
{
        (void)dc; (void)rc; (void)lp;
        if (screens_len >= MAX_SCREENS)
                return FALSE;
        MONITORINFOEXW mi = { .cbSize = sizeof mi };
        if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
                return TRUE;
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);

        struct screen_info *s = &screens[screens_len];
        g_free(s->name);
        s->name = g_utf16_to_utf8((const gunichar2 *)mi.szDevice, -1, NULL, NULL, NULL);
        s->id = screens_len;
        /* Work area: Linux has no taskbar the notifications must avoid. */
        s->x = mi.rcWork.left;
        s->y = mi.rcWork.top;
        s->w = mi.rcWork.right - mi.rcWork.left;
        s->h = mi.rcWork.bottom - mi.rcWork.top;
        s->mmh = 0;
        s->dpi = (int)dx;
        screens_len++;
        return TRUE;
}

static void screens_update(void)
{
        screens_len = 0;
        EnumDisplayMonitors(NULL, NULL, monitor_enum, 0);
}

static int screen_containing(POINT p)
{
        HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { .cbSize = sizeof mi };
        if (!GetMonitorInfoW(m, &mi))
                return 0;
        for (int i = 0; i < screens_len; i++)
                if (screens[i].x == mi.rcWork.left && screens[i].y == mi.rcWork.top)
                        return i;
        return 0;
}

const struct screen_info *w32_get_active_screen(void)
{
        screens_update();
        if (screens_len == 0) {
                static struct screen_info fallback = { .name = "fallback", .w = 1920, .h = 1080, .dpi = 96 };
                return &fallback;
        }

        if (settings.f_mode == FOLLOW_MOUSE) {
                POINT p;
                GetCursorPos(&p);
                return &screens[screen_containing(p)];
        }
        if (settings.f_mode == FOLLOW_KEYBOARD) {
                HWND fg = GetForegroundWindow();
                RECT r;
                if (fg && GetWindowRect(fg, &r)) {
                        POINT p = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
                        return &screens[screen_containing(p)];
                }
        }
        if (settings.monitor_num >= 0 && settings.monitor_num < screens_len)
                return &screens[settings.monitor_num];
        if (settings.monitor)
                for (int i = 0; i < screens_len; i++)
                        if (g_str_has_suffix(screens[i].name, settings.monitor))
                                return &screens[i];

        POINT origin = { 0, 0 }; /* primary monitor */
        return &screens[screen_containing(origin)];
}

double w32_get_scale(void)
{
        if (settings.scale > 0)
                return settings.scale;
        return MAX(1, w32_get_active_screen()->dpi / 96.);
}

bool w32_is_idle(void)
{
        if (settings.idle_threshold == 0)
                return false;
        LASTINPUTINFO li = { .cbSize = sizeof li };
        if (!GetLastInputInfo(&li))
                return false;
        DWORD idle_ms = GetTickCount() - li.dwTime;
        return (gint64)idle_ms > settings.idle_threshold / 1000;
}

bool w32_have_fullscreen_window(void)
{
        QUERY_USER_NOTIFICATION_STATE state;
        if (FAILED(SHQueryUserNotificationState(&state)))
                return false;
        return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN
            || state == QUNS_PRESENTATION_MODE;
}

/* ---- window ------------------------------------------------------------- */

static struct window_w32 *the_window; /* dunst only ever creates one */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
        unsigned int button = 0;
        switch (msg) {
        case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
        case WM_LBUTTONUP: button = BTN_LEFT; break;
        case WM_MBUTTONUP: button = BTN_MIDDLE; break;
        case WM_RBUTTONUP: button = BTN_RIGHT; break;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
        case WM_SETTINGCHANGE: /* work area changed (taskbar moved/resized) */
                if (the_window && the_window->visible)
                        draw();
                break;
        }
        if (button) {
                double scale = w32_get_scale();
                int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                input_handle_click(button, false, (int)(x / scale), (int)(y / scale));
                wake_up();
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
}

window w32_win_create(void)
{
        struct window_w32 *win = g_malloc0(sizeof *win);
        win->hwnd = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                CLASS_NAME, L"dunst", WS_POPUP, 0, 0, 1, 1,
                NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!win->hwnd)
                DIE("CreateWindowEx failed: %lu", GetLastError());
        win->measure_srf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        win->c_ctx = cairo_create(win->measure_srf);
        win->cur_screen = -1;
        the_window = win;
        return win;
}

void w32_win_destroy(window winptr)
{
        struct window_w32 *win = winptr;
        cairo_destroy(win->c_ctx);
        cairo_surface_destroy(win->measure_srf);
        DestroyWindow(win->hwnd);
        if (the_window == win)
                the_window = NULL;
        g_free(win);
}

void w32_win_show(window winptr)
{
        struct window_w32 *win = winptr;
        if (win->visible)
                return;
        ShowWindow(win->hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(win->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        win->visible = true;
}

void w32_win_hide(window winptr)
{
        struct window_w32 *win = winptr;
        ShowWindow(win->hwnd, SW_HIDE);
        win->visible = false;
}

cairo_t *w32_win_get_context(window winptr)
{
        return ((struct window_w32 *)winptr)->c_ctx;
}

void w32_display_surface(cairo_surface_t *srf, window winptr, const struct dimensions *dim)
{
        struct window_w32 *win = winptr;
        const struct screen_info *scr = w32_get_active_screen();
        double scale = w32_get_scale();
        int w = (int)round(dim->w * scale), h = (int)round(dim->h * scale);
        int x, y;
        if (w <= 0 || h <= 0)
                return;
        calc_window_pos(scr, w, h, &x, &y);
        win->cur_screen = scr->id;

        cairo_surface_flush(srf);
        const unsigned char *src = cairo_image_surface_get_data(srf);
        int src_stride = cairo_image_surface_get_stride(srf);
        int sw = cairo_image_surface_get_width(srf), sh = cairo_image_surface_get_height(srf);

        BITMAPINFO bi = { 0 };
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; /* top-down, like cairo */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HDC screen_dc = GetDC(NULL);
        HDC mem_dc = CreateCompatibleDC(screen_dc);
        HBITMAP dib = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!dib) {
                LOG_W("CreateDIBSection(%dx%d) failed", w, h);
                DeleteDC(mem_dc);
                ReleaseDC(NULL, screen_dc);
                return;
        }
        memset(bits, 0, (size_t)w * h * 4);
        for (int row = 0; row < MIN(h, sh); row++)
                memcpy((unsigned char *)bits + (size_t)row * w * 4,
                       src + (size_t)row * src_stride, (size_t)MIN(w, sw) * 4);
        HGDIOBJ old = SelectObject(mem_dc, dib);

        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        if (settings.transparency > 0 && settings.transparency <= 100)
                blend.SourceConstantAlpha = (BYTE)(255 * (100 - settings.transparency) / 100);
        POINT dst = { x, y }, zero = { 0, 0 };
        SIZE size = { w, h };
        if (!UpdateLayeredWindow(win->hwnd, screen_dc, &dst, &size, mem_dc, &zero, 0, &blend, ULW_ALPHA))
                LOG_W("UpdateLayeredWindow failed: %lu", GetLastError());

        SelectObject(mem_dc, old);
        DeleteObject(dib);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
}

/* ---- main loop integration ---------------------------------------------- */

static gboolean msg_prepare(GSource *source, gint *timeout)
{
        (void)source;
        *timeout = -1;
        return GetQueueStatus(QS_ALLINPUT) != 0;
}

static gboolean msg_check(GSource *source)
{
        struct w32_source *s = (struct w32_source *)source;
        return (s->fd.revents & G_IO_IN) || GetQueueStatus(QS_ALLINPUT) != 0;
}

static gboolean msg_dispatch(GSource *source, GSourceFunc cb, gpointer data)
{
        (void)source; (void)cb; (void)data;
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
        }
        return G_SOURCE_CONTINUE;
}

static GSourceFuncs msg_funcs = { msg_prepare, msg_check, msg_dispatch, NULL, NULL, NULL };

bool w32_init(void)
{
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        WNDCLASSEXW wc = { .cbSize = sizeof wc, .lpfnWndProc = wndproc,
                           .hInstance = GetModuleHandleW(NULL),
                           .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND),
                           .lpszClassName = CLASS_NAME };
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;

        screens_update();
        for (int i = 0; i < screens_len; i++)
                LOG_I("Detected screen %d (%s) %ux%u+%d+%d @%d dpi", screens[i].id, screens[i].name,
                      screens[i].w, screens[i].h, screens[i].x, screens[i].y, screens[i].dpi);

        msg_source = g_source_new(&msg_funcs, sizeof(struct w32_source));
        struct w32_source *s = (struct w32_source *)msg_source;
        s->fd.fd = G_WIN32_MSG_HANDLE;
        s->fd.events = G_IO_IN;
        g_source_add_poll(msg_source, &s->fd);
        g_source_attach(msg_source, NULL);
        return true;
}

void w32_deinit(void)
{
        if (msg_source) {
                g_source_destroy(msg_source);
                g_source_unref(msg_source);
                msg_source = NULL;
        }
        UnregisterClassW(CLASS_NAME, GetModuleHandleW(NULL));
}

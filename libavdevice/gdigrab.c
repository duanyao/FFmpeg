/*
 * GDI video grab interface
 *
 * This file is part of FFmpeg.
 *
 * Copyright (C) 2013 Calvin Walton <calvin.walton@kepstin.ca>
 * Copyright (C) 2007-2010 Christophe Gisquet <word1.word2@gmail.com>
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * GDI frame device demuxer
 * @author Calvin Walton <calvin.walton@kepstin.ca>
 * @author Christophe Gisquet <word1.word2@gmail.com>
 */

#include "config.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/thread.h"
#include <windows.h>

/**
 * GDI Device Demuxer context
 */
struct gdigrab {
    const AVClass *class;   /**< Class for private options */

    int        frame_size;  /**< Size in bytes of the frame pixel data */
    int        header_size; /**< Size in bytes of the DIB header */
    int64_t    time_base;   /**< Period of frames (1/framerate) in us */
    int64_t    time_frame;  /**< Time of current/last frame in us */

    int        draw_mouse;  /**< Draw mouse cursor (private option) */
    int        show_region; /**< Draw border (private option) */
    AVRational framerate;   /**< Capture framerate (private option) */
    int        width;       /**< Width of the grab frame (private option) */
    int        height;      /**< Height of the grab frame (private option) */
    int        offset_x;    /**< Capture x offset (private option) */
    int        offset_y;    /**< Capture y offset (private option) */

    HWND       hwnd;        /**< Handle of the window for the grab */
    HDC        source_hdc;  /**< Source device context */
    HDC        dest_hdc;    /**< Destination, source-compatible DC */
    BITMAPINFO bmi;         /**< Information describing DIB format */
    HBITMAP    hbmp[2];     /**< Information on the bitmap captured */
    void      *buffer[2];   /**< The buffer containing the bitmap image data */
    RECT       clip_rect;   /**< The subarea of the screen or window to clip */

    HWND       region_hwnd; /**< Handle of the region border window */

    int cursor_error_printed;

    pthread_t       grab_thread;      /**< Worker thread for grabbing */
    pthread_cond_t  grab_cond;        /**< Signal between the worker and main thread */
    pthread_mutex_t grab_mutex;       /**< Lock of this struct gdigrab */
    int             grab_worker_quit; /**< Boolean, instruct the worker to quit */
    void           *frame_in_stock;   /**< The buffer contains a frame to be consumed */
    int             error_code;       /**< Non-zero: a fatal error happened in the worker thread */
};

#define WIN32_API_ERROR(str)                                            \
    av_log(s1, AV_LOG_ERROR, str " (error %li)\n", GetLastError())

#define REGION_WND_BORDER 3

/**
 * Callback to handle Windows messages for the region outline window.
 *
 * In particular, this handles painting the frame rectangle.
 *
 * @param hwnd The region outline window handle.
 * @param msg The Windows message.
 * @param wparam First Windows message parameter.
 * @param lparam Second Windows message parameter.
 * @return 0 success, !0 failure
 */
static LRESULT CALLBACK
gdigrab_region_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rect;

    switch (msg) {
    case WM_PAINT:
        hdc = BeginPaint(hwnd, &ps);

        GetClientRect(hwnd, &rect);
        FrameRect(hdc, &rect, GetStockObject(BLACK_BRUSH));

        rect.left++; rect.top++; rect.right--; rect.bottom--;
        FrameRect(hdc, &rect, GetStockObject(WHITE_BRUSH));

        rect.left++; rect.top++; rect.right--; rect.bottom--;
        FrameRect(hdc, &rect, GetStockObject(BLACK_BRUSH));

        EndPaint(hwnd, &ps);
        break;
    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
    return 0;
}

/**
 * Initialize the region outline window.
 *
 * @param s1 The format context.
 * @param gdigrab gdigrab context.
 * @return 0 success, !0 failure
 */
static int
gdigrab_region_wnd_init(AVFormatContext *s1, struct gdigrab *gdigrab)
{
    HWND hwnd;
    RECT rect = gdigrab->clip_rect;
    HRGN region = NULL;
    HRGN region_interior = NULL;

    DWORD style = WS_POPUP | WS_VISIBLE;
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT;

    rect.left -= REGION_WND_BORDER; rect.top -= REGION_WND_BORDER;
    rect.right += REGION_WND_BORDER; rect.bottom += REGION_WND_BORDER;

    AdjustWindowRectEx(&rect, style, FALSE, ex);

    // Create a window with no owner; use WC_DIALOG instead of writing a custom
    // window class
    hwnd = CreateWindowEx(ex, WC_DIALOG, NULL, style, rect.left, rect.top,
            rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, NULL, NULL);
    if (!hwnd) {
        WIN32_API_ERROR("Could not create region display window");
        goto error;
    }

    // Set the window shape to only include the border area
    GetClientRect(hwnd, &rect);
    region = CreateRectRgn(0, 0,
            rect.right - rect.left, rect.bottom - rect.top);
    region_interior = CreateRectRgn(REGION_WND_BORDER, REGION_WND_BORDER,
            rect.right - rect.left - REGION_WND_BORDER,
            rect.bottom - rect.top - REGION_WND_BORDER);
    CombineRgn(region, region, region_interior, RGN_DIFF);
    if (!SetWindowRgn(hwnd, region, FALSE)) {
        WIN32_API_ERROR("Could not set window region");
        goto error;
    }
    // The "region" memory is now owned by the window
    region = NULL;
    DeleteObject(region_interior);

    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR) gdigrab_region_wnd_proc);

    ShowWindow(hwnd, SW_SHOW);

    gdigrab->region_hwnd = hwnd;

    return 0;

error:
    if (region)
        DeleteObject(region);
    if (region_interior)
        DeleteObject(region_interior);
    if (hwnd)
        DestroyWindow(hwnd);
    return 1;
}

/**
 * Cleanup/free the region outline window.
 * @param gdigrab gdigrab context.
 */
static void
gdigrab_region_wnd_destroy(struct gdigrab *gdigrab)
{
    if (gdigrab->region_hwnd)
        DestroyWindow(gdigrab->region_hwnd);
    gdigrab->region_hwnd = NULL;
}

/**
 * Process the Windows message queue.
 *
 * This is important to prevent Windows from thinking the window has become
 * unresponsive. As well, things like WM_PAINT (to actually draw the window
 * contents) are handled from the message queue context.
 *
 * @param gdigrab gdigrab context.
 */
static void
gdigrab_region_wnd_update(struct gdigrab *gdigrab)
{
    HWND hwnd = gdigrab->region_hwnd;
    MSG msg;

    while (PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
        DispatchMessage(&msg);
    }
}

/**
 * Destroy condition variable and mutex.
 *
 * @param gdigrab gdigrab context.
 * @return non-zero error, 0 success
 */
static int gdigrab_cond_destroy(struct gdigrab *gdigrab)
{
    pthread_mutex_destroy(&gdigrab->grab_mutex);
    pthread_cond_destroy(&gdigrab->grab_cond);
    return 0;
}

/**
 * Destroy the Win32 windows, device contexts and bitmaps, if any.
 *
 * @param gdigrab gdigrab context.
 * @return non-zero error, 0 success
 */
static int gdigrab_dc_destroy(struct gdigrab *gdigrab)
{
    if (gdigrab->show_region)
        gdigrab_region_wnd_destroy(gdigrab);

    if (gdigrab->source_hdc)
        ReleaseDC(gdigrab->hwnd, gdigrab->source_hdc);
    if (gdigrab->dest_hdc)
        DeleteDC(gdigrab->dest_hdc);
    if (gdigrab->source_hdc)
        DeleteDC(gdigrab->source_hdc);

    for(int i = 0; i < 2; i++) {
        if (gdigrab->hbmp[i]) {
            DeleteObject(gdigrab->hbmp[i]);
            gdigrab->hbmp[i] = NULL;
            gdigrab->buffer[i] = NULL;
        }
    }

    gdigrab->hwnd = NULL;
    gdigrab->source_hdc = NULL;
    gdigrab->dest_hdc = NULL;
    gdigrab->frame_in_stock = NULL;

    return 0;
}

/**
 * Initializes the Win32 windows, device contexts and bitmaps.
 *
 * @param s1 Context from avformat core
 * @param gdigrab gdigrab context.
 * @return AVERROR_IO error, 0 success
 */
static int gdigrab_dc_init(AVFormatContext *s1, struct gdigrab *gdigrab)
{
    pthread_mutex_lock(&gdigrab->grab_mutex);
    av_log(s1, AV_LOG_TRACE, "gdigrab_dc_init: start, locked.\n");

    gdigrab_dc_destroy(gdigrab);

    HWND hwnd = NULL;
    HDC source_hdc = NULL;
    HDC dest_hdc   = NULL;
    BITMAPINFO bmi;

    const char *filename = s1->url;
    const char *name     = NULL;

    int bpp;
    int horzres;
    int vertres;
    int desktophorzres;
    int desktopvertres;
    RECT virtual_rect;
    RECT clip_rect;
    BITMAP bmp;
    int ret = 0;

    if (!strncmp(filename, "title=", 6)) {
        name = filename + 6;
        hwnd = gdigrab->hwnd = FindWindow(NULL, name);
        if (!hwnd) {
            av_log(s1, AV_LOG_ERROR,
                   "Can't find window '%s', aborting.\n", name);
            ret = AVERROR(EIO);
            goto end;
        }
        if (gdigrab->show_region) {
            av_log(s1, AV_LOG_WARNING,
                    "Can't show region when grabbing a window.\n");
            gdigrab->show_region = 0;
        }
    } else if (strcmp(filename, "desktop")) {
        av_log(s1, AV_LOG_ERROR,
               "Please use \"desktop\" or \"title=<windowname>\" to specify your target.\n");
        ret = AVERROR(EIO);
        goto end;
    }

    /* This will get the device context for the selected window, or if
     * none, the primary screen */
    source_hdc = gdigrab->source_hdc = GetDC(hwnd);
    if (!source_hdc) {
        WIN32_API_ERROR("Couldn't get window device context");
        ret = AVERROR(EIO);
        goto end;
    }
    bpp = GetDeviceCaps(source_hdc, BITSPIXEL);

    if (hwnd) {
        GetClientRect(hwnd, &virtual_rect);
    } else {
        /* desktop -- get the right height and width for scaling DPI */
        horzres = GetDeviceCaps(source_hdc, HORZRES);
        vertres = GetDeviceCaps(source_hdc, VERTRES);
        desktophorzres = GetDeviceCaps(source_hdc, DESKTOPHORZRES);
        desktopvertres = GetDeviceCaps(source_hdc, DESKTOPVERTRES);
        virtual_rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtual_rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtual_rect.right = (virtual_rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN)) * desktophorzres / horzres;
        virtual_rect.bottom = (virtual_rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN)) * desktopvertres / vertres;
    }

    /* If no width or height set, use full screen/window area */
    if (!gdigrab->width || !gdigrab->height) {
        clip_rect.left = virtual_rect.left;
        clip_rect.top = virtual_rect.top;
        clip_rect.right = virtual_rect.right;
        clip_rect.bottom = virtual_rect.bottom;
    } else {
        clip_rect.left = gdigrab->offset_x;
        clip_rect.top = gdigrab->offset_y;
        clip_rect.right = gdigrab->width + gdigrab->offset_x;
        clip_rect.bottom = gdigrab->height + gdigrab->offset_y;
    }

    if (clip_rect.left < virtual_rect.left ||
            clip_rect.top < virtual_rect.top ||
            clip_rect.right > virtual_rect.right ||
            clip_rect.bottom > virtual_rect.bottom) {
            av_log(s1, AV_LOG_ERROR,
                    "Capture area (%li,%li),(%li,%li) extends outside window area (%li,%li),(%li,%li)",
                    clip_rect.left, clip_rect.top,
                    clip_rect.right, clip_rect.bottom,
                    virtual_rect.left, virtual_rect.top,
                    virtual_rect.right, virtual_rect.bottom);
            ret = AVERROR(EIO);
            goto end;
    }


    if (name) {
        av_log(s1, AV_LOG_INFO,
               "Found window %s, capturing %lix%lix%i at (%li,%li)\n",
               name,
               clip_rect.right - clip_rect.left,
               clip_rect.bottom - clip_rect.top,
               bpp, clip_rect.left, clip_rect.top);
    } else {
        av_log(s1, AV_LOG_INFO,
               "Capturing whole desktop as %lix%lix%i at (%li,%li)\n",
               clip_rect.right - clip_rect.left,
               clip_rect.bottom - clip_rect.top,
               bpp, clip_rect.left, clip_rect.top);
    }

    if (clip_rect.right - clip_rect.left <= 0 ||
            clip_rect.bottom - clip_rect.top <= 0 || bpp%8) {
        av_log(s1, AV_LOG_ERROR, "Invalid properties, aborting\n");
        ret = AVERROR(EIO);
        goto end;
    }

    dest_hdc = gdigrab->dest_hdc = CreateCompatibleDC(source_hdc);
    if (!dest_hdc) {
        WIN32_API_ERROR("Screen DC CreateCompatibleDC");
        ret = AVERROR(EIO);
        goto end;
    }

    /* Create a DIB and select it into the dest_hdc */
    bmi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth         = clip_rect.right - clip_rect.left;
    bmi.bmiHeader.biHeight        = -(clip_rect.bottom - clip_rect.top);
    bmi.bmiHeader.biPlanes        = 1;
    bmi.bmiHeader.biBitCount      = bpp;
    bmi.bmiHeader.biCompression   = BI_RGB;
    bmi.bmiHeader.biSizeImage     = 0;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed       = 0;
    bmi.bmiHeader.biClrImportant  = 0;

    for(int i = 0; i < 2; i++) {
        gdigrab->hbmp[i] = CreateDIBSection(dest_hdc, &bmi, DIB_RGB_COLORS,
            &gdigrab->buffer[i], NULL, 0);
        if (!gdigrab->hbmp[i]) {
            WIN32_API_ERROR("Creating DIB Section");
            ret = AVERROR(EIO);
            goto end;
        }
    }

    /* Get info from the bitmap */
    GetObject(gdigrab->hbmp[0], sizeof(BITMAP), &bmp);

    gdigrab->frame_size  = bmp.bmWidthBytes * bmp.bmHeight * bmp.bmPlanes;
    gdigrab->header_size = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) +
                           (bpp <= 8 ? (1 << bpp) : 0) * sizeof(RGBQUAD) /* palette size */;
    gdigrab->time_base   = (int64_t)(1000000.0 / av_q2d(gdigrab->framerate));

    gdigrab->bmi        = bmi;
    gdigrab->clip_rect  = clip_rect;

    gdigrab->cursor_error_printed = 0;

    if (gdigrab->show_region) {
        if (gdigrab_region_wnd_init(s1, gdigrab)) {
            ret = AVERROR(EIO);
            goto end;
        }
    }

    av_log(s1, AV_LOG_TRACE, "gdigrab_dc_init: ok\n");

end:
    if (ret)
        gdigrab_dc_destroy(gdigrab);

    gdigrab->error_code = ret;
    pthread_cond_broadcast(&gdigrab->grab_cond);
    pthread_mutex_unlock(&gdigrab->grab_mutex);
    return ret;
}

/**
 * Paints a mouse pointer in a Win32 image.
 *
 * @param s1 Context of the log information
 * @param gdigrab gdigrab context.
 */
static void paint_mouse_pointer(AVFormatContext *s1, struct gdigrab *gdigrab)
{
    CURSORINFO ci = {0};

#define CURSOR_ERROR(str)                 \
    if (!gdigrab->cursor_error_printed) {       \
        WIN32_API_ERROR(str);             \
        gdigrab->cursor_error_printed = 1;      \
    }

    ci.cbSize = sizeof(ci);

    if (GetCursorInfo(&ci)) {
        HCURSOR icon = CopyCursor(ci.hCursor);
        ICONINFO info;
        POINT pos;
        RECT clip_rect = gdigrab->clip_rect;
        HWND hwnd = gdigrab->hwnd;
        int horzres = GetDeviceCaps(gdigrab->source_hdc, HORZRES);
        int vertres = GetDeviceCaps(gdigrab->source_hdc, VERTRES);
        int desktophorzres = GetDeviceCaps(gdigrab->source_hdc, DESKTOPHORZRES);
        int desktopvertres = GetDeviceCaps(gdigrab->source_hdc, DESKTOPVERTRES);
        info.hbmMask = NULL;
        info.hbmColor = NULL;

        if (ci.flags != CURSOR_SHOWING)
            return;

        if (!icon) {
            /* Use the standard arrow cursor as a fallback.
             * You'll probably only hit this in Wine, which can't fetch
             * the current system cursor. */
            icon = CopyCursor(LoadCursor(NULL, IDC_ARROW));
        }

        if (!GetIconInfo(icon, &info)) {
            CURSOR_ERROR("Could not get icon info");
            goto icon_error;
        }

        pos.x = ci.ptScreenPos.x - clip_rect.left - info.xHotspot;
        pos.y = ci.ptScreenPos.y - clip_rect.top - info.yHotspot;

        if (hwnd) {
            RECT rect;

            if (GetWindowRect(hwnd, &rect)) {
                pos.x -= rect.left;
                pos.y -= rect.top;
            } else {
                CURSOR_ERROR("Couldn't get window rectangle");
                goto icon_error;
            }
        }

        //that would keep the correct location of mouse with hidpi screens
        pos.x = pos.x * desktophorzres / horzres;
        pos.y = pos.y * desktopvertres / vertres;

        av_log(s1, AV_LOG_DEBUG, "Cursor pos (%li,%li) -> (%li,%li)\n",
                ci.ptScreenPos.x, ci.ptScreenPos.y, pos.x, pos.y);

        if (pos.x >= 0 && pos.x <= clip_rect.right - clip_rect.left &&
                pos.y >= 0 && pos.y <= clip_rect.bottom - clip_rect.top) {
            if (!DrawIcon(gdigrab->dest_hdc, pos.x, pos.y, icon))
                CURSOR_ERROR("Couldn't draw icon");
        }

icon_error:
        if (info.hbmMask)
            DeleteObject(info.hbmMask);
        if (info.hbmColor)
            DeleteObject(info.hbmColor);
        if (icon)
            DestroyCursor(icon);
    } else {
        CURSOR_ERROR("Couldn't get cursor info");
    }
}

/**
 * Worker thread entry function.
 *
 * @param v AVFormatContext
 */
static void * attribute_align_arg gdigrab_worker(void *v)
{
    AVFormatContext *s1 = v;
    struct gdigrab *gdigrab = s1->priv_data;

    av_log(s1, AV_LOG_TRACE, "gdigrab_worker: start.\n");

    if (gdigrab_dc_init(s1, gdigrab))
        goto end;

    const HDC        dest_hdc   = gdigrab->dest_hdc;
    const HDC        source_hdc = gdigrab->source_hdc;
    const RECT       clip_rect  = gdigrab->clip_rect;
    const int64_t    time_base  = gdigrab->time_base;

    int64_t    time_start, time_grab_end, time_sleep_start, time_end,
               time_sleep, time_request_sleep, time_actual_sleep,
               sleep_balance = 0;
    int        error = 0;
    void      *frame_in_stock = NULL;

    av_log(s1, AV_LOG_TRACE, "gdigrab_worker: time_base:%.3f.\n", time_base / 1000000.0);

    time_end = av_gettime();
    for(int i = 0, sn = 0; !gdigrab->grab_worker_quit; i = (i + 1) % 2, sn++) {
        /* time_start of this frame is time_end of last frame */
        time_start = time_end;
        av_log(s1, AV_LOG_TRACE, "gdigrab_worker: sn:%04d, index:%d\n", sn, i);

        /* Run Window message processing queue */
        if (gdigrab->show_region)
            gdigrab_region_wnd_update(gdigrab);
        /* Blit screen grab */
        if (!SelectObject(dest_hdc, gdigrab->hbmp[i])) {
            WIN32_API_ERROR("SelectObject");
            error = AVERROR(EIO);
            frame_in_stock = NULL;
        } else {
            if (!BitBlt(dest_hdc, 0, 0,
                        clip_rect.right - clip_rect.left,
                        clip_rect.bottom - clip_rect.top,
                        source_hdc,
                        clip_rect.left, clip_rect.top, SRCCOPY | CAPTUREBLT)) {
                WIN32_API_ERROR("Failed to capture image");
                error = AVERROR(EIO);
                frame_in_stock = NULL;
            } else {
                error = 0;
                frame_in_stock = gdigrab->buffer[i];
                if (gdigrab->draw_mouse)
                    paint_mouse_pointer(s1, gdigrab);
            }
        }
        time_grab_end = av_gettime();

        pthread_mutex_lock(&gdigrab->grab_mutex);
        /* Quit if error happened on first try */
        if (error && sn == 0) {
            gdigrab->error_code = error;
            break;
        }
        if (gdigrab->grab_worker_quit)
            break;

        /* Wait for a frame being comsumed */
        while(gdigrab->frame_in_stock) {
            av_log(s1, AV_LOG_TRACE, "gdigrab_worker: wait frame_in_stock.\n");
            pthread_cond_wait(&gdigrab->grab_cond, &gdigrab->grab_mutex);
            av_log(s1, AV_LOG_TRACE, "gdigrab_worker: wait frame_in_stock continue.\n");
        }

        if (gdigrab->grab_worker_quit)
            break;

        gdigrab->time_frame = time_start;
        gdigrab->frame_in_stock = frame_in_stock;
        pthread_cond_broadcast(&gdigrab->grab_cond);
        av_log(s1, AV_LOG_TRACE, "gdigrab_worker: a frame posted, sn:%04d\n", sn);
        pthread_mutex_unlock(&gdigrab->grab_mutex);

        /* sleep based on the frame rate */
        time_sleep_start = av_gettime();
        time_sleep = time_base - (time_sleep_start - time_start);
        time_request_sleep = time_sleep + sleep_balance;
        if (time_request_sleep > 0)
            av_usleep(time_request_sleep);

        time_end = av_gettime();
        time_actual_sleep = time_end - time_sleep_start;
        sleep_balance += time_sleep - time_actual_sleep;
        /* restrict the minimum of sleep_balance */
        if (sleep_balance < -time_base)
            sleep_balance = -time_base;

        av_log(s1, AV_LOG_DEBUG, "gdigrab_worker: a frame finished, sn:%04d, "
            "time_used:%.3f, grab:%.3f, wait:%.3f, sleep:%.3f, balance:%.3f\n",
            sn,
            (time_end - time_start) / 1000000.0,
            (time_grab_end - time_start) / 1000000.0,
            (time_sleep_start - time_grab_end) / 1000000.0,
            time_actual_sleep / 1000000.0,
            sleep_balance / 1000000.0
            );
    }

end:
    av_log(s1, AV_LOG_TRACE, "gdigrab_worker: exiting.\n");
    gdigrab_dc_destroy(gdigrab);
    pthread_cond_broadcast(&gdigrab->grab_cond);
    pthread_mutex_unlock(&gdigrab->grab_mutex);
    gdigrab_cond_destroy(gdigrab);
    return NULL;
}

/**
 * Start the worker thread.
 *
 * @param v Context from avformat core
 * @return AVERROR_IO error, 0 success
 */
static int gdigrab_worker_start(AVFormatContext *s1)
{
    av_log(s1, AV_LOG_TRACE, "gdigrab_worker_start\n");
    struct gdigrab *gdigrab = s1->priv_data;

    pthread_mutex_init(&gdigrab->grab_mutex, NULL);
    pthread_cond_init(&gdigrab->grab_cond, NULL);
    gdigrab->error_code = 0;

    if (pthread_create(&gdigrab->grab_thread, NULL, gdigrab_worker, s1)) {
        gdigrab_cond_destroy(gdigrab);
        return AVERROR(EIO);
    }
    av_log(s1, AV_LOG_TRACE, "gdigrab_worker_start: pthread_create() ok.\n");

    pthread_mutex_lock(&gdigrab->grab_mutex);
    if (!gdigrab->error_code)
        pthread_cond_wait(&gdigrab->grab_cond, &gdigrab->grab_mutex);
    av_log(s1, AV_LOG_TRACE, "gdigrab_worker_start: error_code: %d.\n", gdigrab->error_code);
    pthread_mutex_unlock(&gdigrab->grab_mutex);
    return gdigrab->error_code;
}

/**
 * Initializes the gdi grab device demuxer (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @return AVERROR_IO error, 0 success
 */
static int gdigrab_read_header(AVFormatContext *s1)
{
    int ret;
    struct gdigrab *gdigrab = s1->priv_data;
    AVStream *st = avformat_new_stream(s1, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    if (gdigrab_worker_start(s1)) {
        ret = AVERROR(EIO);
        goto error;
    }

    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */
    st->avg_frame_rate = gdigrab->framerate;

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_BMP;
    st->codecpar->bit_rate   = (gdigrab->header_size + gdigrab->frame_size) * av_q2d(gdigrab->framerate) * 8;

    return 0;

error:
    return ret;
}

/**
 * Copy a grabbed frame to a AVPacket.
 *
 * @param gdigrab gdigrab context.
 * @param pkt Packet holding the grabbed frame
 * @return no-zero error, 0 success
 */
static int gdigrab_copy_frame(struct gdigrab *gdigrab, AVPacket *pkt)
{
    BITMAPFILEHEADER bfh;
    int file_size = gdigrab->header_size + gdigrab->frame_size;

    if (av_new_packet(pkt, file_size) < 0)
        return AVERROR(ENOMEM);

    pkt->pts = gdigrab->time_frame;

    /* Copy bits to packet data */
    bfh.bfType = 0x4d42; /* "BM" in little-endian */
    bfh.bfSize = file_size;
    bfh.bfReserved1 = 0;
    bfh.bfReserved2 = 0;
    bfh.bfOffBits = gdigrab->header_size;

    memcpy(pkt->data, &bfh, sizeof(bfh));

    memcpy(pkt->data + sizeof(bfh), &gdigrab->bmi.bmiHeader, sizeof(gdigrab->bmi.bmiHeader));

    if (gdigrab->bmi.bmiHeader.biBitCount <= 8)
        GetDIBColorTable(gdigrab->dest_hdc, 0, 1 << gdigrab->bmi.bmiHeader.biBitCount,
                (RGBQUAD *) (pkt->data + sizeof(bfh) + sizeof(gdigrab->bmi.bmiHeader)));

    memcpy(pkt->data + gdigrab->header_size, gdigrab->frame_in_stock, gdigrab->frame_size);

    gdigrab->frame_in_stock = NULL;

    return gdigrab->header_size + gdigrab->frame_size;
}

/**
 * Grabs a frame from gdi (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @param pkt Packet holding the grabbed frame
 * @return frame size in bytes
 */
static int gdigrab_read_packet(AVFormatContext *s1, AVPacket *pkt)
{
    av_log(s1, AV_LOG_TRACE, "gdigrab_read_packet: start.\n");
    struct gdigrab *gdigrab = s1->priv_data;
    int ret;
    pthread_mutex_lock(&gdigrab->grab_mutex);
    if (gdigrab->error_code) {
        ret = AVERROR(EIO);
    } else if (gdigrab->frame_in_stock) {
        ret = gdigrab_copy_frame(gdigrab, pkt);
    } else if (s1->flags & AVFMT_FLAG_NONBLOCK) {
-       ret = AVERROR(EAGAIN);
    } else {
        av_log(s1, AV_LOG_TRACE, "gdigrab_read_packet: wait.\n");
        pthread_cond_wait(&gdigrab->grab_cond, &gdigrab->grab_mutex);
        av_log(s1, AV_LOG_TRACE, "gdigrab_read_packet: continue.\n");
        if (gdigrab->frame_in_stock) {
            ret = gdigrab_copy_frame(gdigrab, pkt);
        } else {
            ret = AVERROR(EIO);
            av_log(s1, AV_LOG_ERROR, "gdigrab_read_packet: no captured image\n");
        }
    }
    pthread_cond_broadcast(&gdigrab->grab_cond);
    pthread_mutex_unlock(&gdigrab->grab_mutex);
    av_log(s1, AV_LOG_TRACE, "gdigrab_read_packet: end.\n");
    return ret;
}

/**
 * Closes gdi frame grabber (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @return 0 success, !0 failure
 */
static int gdigrab_read_close(AVFormatContext *s1)
{
    struct gdigrab *gdigrab = s1->priv_data;
    pthread_mutex_lock(&gdigrab->grab_mutex);
    gdigrab->grab_worker_quit = TRUE;
    pthread_cond_broadcast(&gdigrab->grab_cond);
    pthread_mutex_unlock(&gdigrab->grab_mutex);
    return 0;
}

#define OFFSET(x) offsetof(struct gdigrab, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "draw_mouse", "draw the mouse pointer", OFFSET(draw_mouse), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, DEC },
    { "show_region", "draw border around capture area", OFFSET(show_region), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, DEC },
    { "framerate", "set video frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "ntsc"}, 0, INT_MAX, DEC },
    { "video_size", "set video frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "offset_x", "capture area x offset", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "offset_y", "capture area y offset", OFFSET(offset_y), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { NULL },
};

static const AVClass gdigrab_class = {
    .class_name = "GDIgrab indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/** gdi grabber device demuxer declaration */
AVInputFormat ff_gdigrab_demuxer = {
    .name           = "gdigrab",
    .long_name      = NULL_IF_CONFIG_SMALL("GDI API Windows frame grabber"),
    .priv_data_size = sizeof(struct gdigrab),
    .read_header    = gdigrab_read_header,
    .read_packet    = gdigrab_read_packet,
    .read_close     = gdigrab_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &gdigrab_class,
};

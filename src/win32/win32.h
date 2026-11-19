/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file
 * @brief Win32 output backend (third implementation of struct output, next
 *        to X11 and Wayland).
 */
#ifndef DUNST_WIN32_H
#define DUNST_WIN32_H

#include <stdbool.h>
#include <cairo.h>
#include "../output.h"

bool w32_init(void);
void w32_deinit(void);

window w32_win_create(void);
void w32_win_destroy(window win);
void w32_win_show(window win);
void w32_win_hide(window win);

void w32_display_surface(cairo_surface_t *srf, window win, const struct dimensions *dim);
cairo_t *w32_win_get_context(window win);

const struct screen_info *w32_get_active_screen(void);
bool w32_is_idle(void);
bool w32_have_fullscreen_window(void);
double w32_get_scale(void);

#endif

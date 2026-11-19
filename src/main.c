/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file
 * @ingroup main
 * @brief Glue that starts dunst_main
 * @copyright Copyright 2026 Dunst contributors
 * @license BSD-3-Clause
 */

#include "dunst.h"

/**
 * @defgroup main Initialization and event loop
 * Contains the core logic for Dunst.
 *
 * @defgroup config Settings and options
 * Parsing and validation of configuration options.
 *
 * @defgroup graphics Graphics and rendering
 * Drawing logic and generic output code.
 *
 * @defgroup utils Utilities
 * String helpers, icon lookup, and logging.
 *
 * @defgroup x11 Xorg backend
 * Subsystem for supporting X11 input and rendering.
 *
 * @defgroup wayland Wayland backend
 * Subsystem for supporting Wayland input and rendering.
 *
 * @defgroup input Input subsystem
 * Support for mouse events and action menus.
 *
 * @defgroup notify Notifications
 * Code related to notifications and DBus.
 */

#ifdef _WIN32
#include <glib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
        /* argv arrives in the ANSI code page; dunst (GLib) wants UTF-8. */
        SetConsoleOutputCP(CP_UTF8);
        gchar **utf8_argv = g_win32_get_command_line();
        int ret = dunst_main((int)g_strv_length(utf8_argv), utf8_argv);
        g_strfreev(utf8_argv);
        (void)argc; (void)argv;
        return ret;
#else
        return dunst_main(argc, argv);
#endif
}

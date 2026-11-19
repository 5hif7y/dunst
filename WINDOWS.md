# dunst for Windows (MSVC + vcpkg)

Native port: a third output backend next to X11 and Wayland, the
`org.freedesktop.Notifications` interface over a named pipe instead of the
D-Bus session bus, and a small POSIX compatibility layer. Rendering is
unchanged: the same cairo/pango code draws the notifications.

## Build

```bat
vcpkg install glib cairo pango gdk-pixbuf --triplet x64-windows
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-win          :: build-win\dunst.exe + the vcpkg DLLs
```

(from an "x64 Native Tools" prompt; a `pkg-config`/`pkgconf` must be on PATH or
passed as `-DPKG_CONFIG_EXECUTABLE=...`). The Linux build still uses meson.

## Run

```powershell
build-win\dunst.exe                        # config: %LOCALAPPDATA%\dunst\dunstrc
notify-send -u critical "Build FAILED" "3 tests"   # notify-send from LowLevel-Katas N4-notifications
.\dunstctl.ps1 history | ConvertFrom-Json
.\dunstctl.ps1 set-paused toggle
```

Any wnotify client works (protocol: LowLevel-Katas `N4-notifications/wnotify/PROTOCOL.md`).
To keep a permanent SQLite history, put `notifyd --forward` in front:
`WNOTIFY_PIPE=\\.\pipe\dunst-down dunst` + `notifyd --forward \\.\pipe\dunst-down`.

## What changed

| Piece | Linux | Windows |
|---|---|---|
| output | `src/x11`, `src/wayland` | `src/win32/win32.c`: layered `WS_POPUP` window, `UpdateLayeredWindow` with cairo's premultiplied ARGB32, monitors/work area/DPI, idle via `GetLastInputInfo`, fullscreen via `SHQueryUserNotificationState`, window messages through `G_WIN32_MSG_HANDLE` in the GMainLoop |
| IPC | GDBus on the session bus | `src/win32/gdbus_shim.h` redirects `GDBusMethodInvocation`, `return_value`, `emit_signal` to `src/win32/dbus_win32.inc` (named pipe); every request is marshalled to the main loop. `dbus.c` method bodies are unchanged |
| dunstctl | `dunstctl` (sh + busctl) | `dunstctl.ps1`; `history`/`rules` come back as JSON (`json:` request prefix) |
| signals | `SIGUSR1/2` pause, `SIGINT/TERM` | `dunstctl set-paused`; `SetConsoleCtrlHandler` → `quit_signal` on the main loop |
| scripts | `fork` + `execlp` with env | `g_spawn_async` with an env block; `.ps1` scripts run through PowerShell |
| POSIX | regex.h, fnmatch, wordexp, scandir, pwd, libgen, getline | `src/win32/compat/` (regex.h on GRegex, own fnmatch/wordexp, scandir on GDir), force-included `dunst_compat.h` |
| argv | char** | `g_win32_get_command_line()` (UTF-8) |

## Verified

notify-send → three notifications drawn with urgency colours, pango markup,
accents and the progress bar; `GetServerInformation`/`GetCapabilities`;
`dunstctl` count, close, close-all, history (JSON), history-pop, is-paused /
set-paused, debug; `dunstctl action 0` delivers `ActionInvoked(id, "default")`
to a waiting `notify-send -A`.

Not tested yet: context menu (needs a dmenu-like program on Windows, e.g.
`fzf` in a console), `script =` rules, icons from a Windows icon theme, keyboard
shortcuts (X11-only upstream, not implemented here), the test suite under `test/`.

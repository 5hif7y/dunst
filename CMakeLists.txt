# Windows (MSVC + vcpkg) build of dunst. Linux keeps using meson.build.
#
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
#   cmake --build build-win --config Release
#
# vcpkg packages: glib cairo pango gdk-pixbuf (x64-windows)
cmake_minimum_required(VERSION 3.20)
project(dunst C)

if(NOT WIN32)
  message(FATAL_ERROR "This CMake build is the Windows port; use meson on Linux/BSD.")
endif()

set(CMAKE_C_STANDARD 11)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DUNST_DEPS REQUIRED IMPORTED_TARGET
  glib-2.0 gio-2.0 cairo pangocairo gdk-pixbuf-2.0)

execute_process(COMMAND git describe --tags
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE DUNST_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT DUNST_VERSION)
  set(DUNST_VERSION "1.13.2")
endif()

set(DUNST_CORE
  src/dbus.c src/draw.c src/dunst.c src/icon-lookup.c src/icon.c src/ini.c
  src/input.c src/log.c src/markup.c src/menu.c src/notification.c
  src/option_parser.c src/output.c src/queues.c src/rules.c src/settings.c
  src/utils.c)
set(DUNST_WIN32
  src/win32/win32.c src/win32/wnotify.c src/win32/compat/compat.c)

add_executable(dunst src/main.c ${DUNST_CORE} ${DUNST_WIN32})
target_include_directories(dunst PRIVATE src src/win32/compat)
target_compile_definitions(dunst PRIVATE
  VERSION="${DUNST_VERSION}-win"
  SYSCONFDIR="C:/ProgramData"
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(dunst PRIVATE /utf-8 /W3
  /FI${CMAKE_CURRENT_SOURCE_DIR}/src/win32/compat/dunst_compat.h
  /wd4244 /wd4267 /wd4018) # size/sign narrowing noise from GLib-style code
target_link_libraries(dunst PRIVATE PkgConfig::DUNST_DEPS
  user32 gdi32 shcore shell32 advapi32)

# Bring the vcpkg DLLs next to dunst.exe so it runs from the build dir
# (pkg-config imported targets carry no DLL locations for TARGET_RUNTIME_DLLS).
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  file(GLOB DUNST_RUNTIME_DLLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll")
  add_custom_command(TARGET dunst POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DUNST_RUNTIME_DLLS} $<TARGET_FILE_DIR:dunst>)
endif()

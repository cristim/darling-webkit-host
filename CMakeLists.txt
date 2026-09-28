cmake_minimum_required(VERSION 3.16)
project(darling-webkit-host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Core: GTK is needed for cairo (frame export) and by the webkitgtk backend.
# Arch ships gtk3 without a CMake config package, so go through pkg-config.
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_executable(darling-webkit-host
	src/main.c
	src/registry.c
	src/serve.c
)

target_include_directories(darling-webkit-host PRIVATE src)
target_link_libraries(darling-webkit-host PRIVATE PkgConfig::GTK3)

# Backends are compiled in only when their dependencies are present, so the
# registry table can never advertise an engine this build cannot start.
pkg_check_modules(WEBKITGTK IMPORTED_TARGET webkit2gtk-4.1)
if (WEBKITGTK_FOUND)
	target_sources(darling-webkit-host PRIVATE src/backends/webkitgtk.c)
	target_link_libraries(darling-webkit-host PRIVATE PkgConfig::WEBKITGTK)
	message(STATUS "darling-webkit-host: webkitgtk backend enabled")
else()
	message(WARNING "darling-webkit-host: webkit2gtk-4.1 not found, webkitgtk backend disabled")
endif()

# The chromium backend is always built: it only needs a POSIX spawn and the
# sockets, and probe() checks for the binary at runtime. That keeps the
# "use whatever is available" promise working on hosts without a Chromium.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(darling-webkit-host PRIVATE src/backends/chromium.c)
	message(STATUS "darling-webkit-host: chromium backend enabled")
else()
	message(WARNING "darling-webkit-host: non-Linux, chromium backend disabled")
endif()

# Backends write CDP/HTML strings and fork a browser; warnings that matter here
# are the uninitialised and format ones.
target_compile_options(darling-webkit-host PRIVATE
	-Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
)

# Portable guest transport and its test. Kept in this tree so the guest half
# can be exercised off-target; WKWebView.m is a thin shell over it.
add_library(dwb_client STATIC src/dwb_client.c)
target_include_directories(dwb_client PUBLIC src)
target_link_libraries(dwb_client PUBLIC glib-2.0)

add_executable(clienttest clienttest.c)
target_link_libraries(clienttest PRIVATE dwb_client)

add_executable(shmtest shmtest.c)
target_link_libraries(shmtest PRIVATE dwb_client)

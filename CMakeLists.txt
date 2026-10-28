# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: LGPL-3.0-or-later

#===----------------------------------------------------------------------===#
#
# Agora's build for the change stream (libagora-watch).
#
# ONE PLATFORM BACKEND IS CHOSEN BY THE BUILD: `inotify` on Linux, `kqueue` on the BSDs. Neither is a
# library and neither is systemd's — they are the kernel's own interfaces, which is what the no-systemd and
# Linux-plus-BSDs rules require. The engine (`EventStream.c`) is platform-independent.
#
# Core Foundation alone is linked, for the `CFArrayRef` of paths and the `CFTimeInterval` latency.
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.24)

project(AgoraWatch LANGUAGES C)

# **EVERY LIBRARY FINDS ITS NEIGHBOURS, AND THE INSTALLED BUNDLE NEEDS NO ENVIRONMENT.** A `DT_RUNPATH` is NOT
# inherited by a dependency's own dependencies: whatever a program puts on its search path covers the libraries IT
# names and nothing below them. So an application whose runpath is the prefix still fails to resolve
# `libagora-base.so`'s `libagora-core.so`, because that lookup asks the LIBRARY for its path and it had none —
# MEASURED as an installed bundle that only started with `LD_LIBRARY_PATH` set to the directory its libraries were
# already in. `$ORIGIN` says "the directory I am in", which is where this tree installs every one of them, so the
# chain resolves on its own and a relocatable install is genuinely relocatable.
set(CMAKE_INSTALL_RPATH "\$ORIGIN")

include(GNUInstallDirs)

set(CMAKE_POSITION_INDEPENDENT_CODE YES)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(CoreFoundation CONFIG REQUIRED)
find_package(Threads REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_agora_watch_backend "${CMAKE_CURRENT_SOURCE_DIR}/EventStream-inotify.c")
  message(STATUS "libagora-watch: inotify backend (Linux)")
else()
  set(_agora_watch_backend "${CMAKE_CURRENT_SOURCE_DIR}/EventStream-kqueue.c")
  message(STATUS "libagora-watch: kqueue backend (BSD)")
endif()

# ---------------------------------------------------------------------------
# Include shim: consumers write <Watch/...>.
# ---------------------------------------------------------------------------
set(_agora_include_dir "${CMAKE_BINARY_DIR}/agora-include")
file(REMOVE_RECURSE "${_agora_include_dir}")
file(MAKE_DIRECTORY "${_agora_include_dir}")
file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}" "${_agora_include_dir}/Watch" SYMBOLIC)

add_library(Watch SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/EventStream.c"
    "${_agora_watch_backend}")

set_target_properties(Watch PROPERTIES OUTPUT_NAME agora-watch)

target_compile_options(Watch PRIVATE -fconstant-cfstrings)

target_include_directories(Watch
    PUBLIC
        "$<BUILD_INTERFACE:${_agora_include_dir}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

# Threads is PRIVATE: the reader thread and its lock are the library's own business, and no type from it
# appears in a public header.
target_link_libraries(Watch
    PUBLIC
        CoreFoundation::CoreFoundation
    PRIVATE
        Threads::Threads)

# **THE TESTS ARE A DEVELOPMENT ARTEFACT, AND A RELEASE DOES NOT BUILD THEM.** `include(CTest)` is what defines
# `BUILD_TESTING` — ON when a configure says nothing, which is what a developer's build wants — and turns testing on with
# it; the subdirectory is added only when it is on, so `-DBUILD_TESTING=OFF`, which is what the release driver passes,
# configures and compiles no test executable at all.
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(DIRECTORY . DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Watch
        FILES_MATCHING PATTERN "*.h" PATTERN "*_internal.h" EXCLUDE)
install(TARGETS Watch
    EXPORT AgoraWatchTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CMakePackageConfigHelpers)

install(EXPORT AgoraWatchTargets
    FILE AgoraWatchTargets.cmake
    NAMESPACE AgoraWatch::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWatch)

configure_package_config_file(cmake/AgoraWatchConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWatchConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWatch)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWatchConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWatchConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWatchConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWatch)

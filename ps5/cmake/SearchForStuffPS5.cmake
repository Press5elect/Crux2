# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the dependencies, in place of cmake/SearchForStuff.cmake.
#
# The libraries the console build cannot take from a system are static archives in the port's
# prefix (.deps/ports, made by ps5/tools/build-deps.sh) and are linked into the title by the port's
# own link step (ps5/tools/link-title.sh). The targets below therefore carry include directories
# and the names upstream's lists expect, and nothing to link. 3rdparty/ is built as upstream builds
# it, but for what the console has no use for: cubeb (audio goes to the console's AudioOut), SDL
# (the pad is read with scePad), libpcap, FFmpeg, OpenGL, Qt and the window systems.

get_filename_component(PS5PX_SYSROOT "${PS5PX_ROOT}/../.deps/ports" ABSOLUTE)
get_filename_component(PS5PX_DEPS_SRC "${PS5PX_ROOT}/../.deps/src" ABSOLUTE)
if(NOT EXISTS "${PS5PX_SYSROOT}/lib/libryml.a")
	message(FATAL_ERROR "The port's libraries are not built: run ps5/tools/build-deps.sh")
endif()

find_package(Git)

function(ps5_header_target name)
	add_library(${name} INTERFACE IMPORTED GLOBAL)
	if(ARGN)
		set_target_properties(${name} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ARGN}")
	endif()
endfunction()

add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)

ps5_header_target(ZLIB::ZLIB "${PS5PX_SYSROOT}/include")
ps5_header_target(Zstd::Zstd "${PS5PX_SYSROOT}/include")
ps5_header_target(LZ4::LZ4 "${PS5PX_SYSROOT}/include")
ps5_header_target(PNG::PNG "${PS5PX_SYSROOT}/include")
ps5_header_target(JPEG::JPEG "${PS5PX_SYSROOT}/include")
ps5_header_target(WebP::libwebp "${PS5PX_SYSROOT}/include")
ps5_header_target(Freetype::Freetype "${PS5PX_SYSROOT}/include/freetype2")
ps5_header_target(plutovg::plutovg "${PS5PX_SYSROOT}/include/plutovg")
ps5_header_target(plutosvg::plutosvg "${PS5PX_SYSROOT}/include/plutosvg;${PS5PX_SYSROOT}/include/plutovg")
ps5_header_target(ryml::ryml "${PS5PX_SYSROOT}/include")
ps5_header_target(Shaderc::shaderc_shared "${PS5PX_SYSROOT}/include")
ps5_header_target(CURL::libcurl "${PS5PX_SYSROOT}/include")
# Headers only: the SDL input source and audio stream are not built, but the files that share
# their types with them include <SDL3/SDL.h>.
ps5_header_target(SDL3::SDL3 "${PS5PX_DEPS_SRC}/sdl3/include")
ps5_header_target(PCAP::PCAP)
ps5_header_target(cubeb)
set_target_properties(plutovg::plutovg plutosvg::plutosvg PROPERTIES
	INTERFACE_COMPILE_DEFINITIONS "PLUTOVG_BUILD_STATIC;PLUTOSVG_BUILD_STATIC")
set_target_properties(ryml::ryml PROPERTIES INTERFACE_COMPILE_DEFINITIONS "RYML_DEFAULT_CALLBACK_USES_EXCEPTIONS=0")

# FFmpeg is loaded at run time on the other platforms, for video capture only. The console has no
# library to load, so capture is compiled out (ps5/frontend/ps5_capture_stub.cpp).
set(FFMPEG_INCLUDE_DIRS "")
set(LIBC_LIBRARIES "")

# The console's kernel pages are 16 KiB, and its CPU's cache lines 64 bytes.
list(APPEND PCSX2_DEFS OVERRIDE_HOST_PAGE_SIZE=0x4000 OVERRIDE_HOST_CACHE_LINE_SIZE=64)

# The port layer's headers come first: a <malloc.h> stand-in, and the prefix that routes stdio
# through the port's virtual file layer (network shares).
include_directories(BEFORE "${PS5PX_ROOT}/frontend/compat")
include("${PS5PX_ROOT}/frontend/compat/ps5_stdio_redirect.cmake")

# cpuinfo: its header as PCSX2 has it, and the console's one fixed CPU behind it (the library's
# FreeBSD back end asks the kernel for a topology the title's sandbox does not give out).
add_library(cpuinfo STATIC "${PS5PX_ROOT}/frontend/cpuinfo_ps5.c")
target_include_directories(cpuinfo PUBLIC "${CMAKE_SOURCE_DIR}/3rdparty/cpuinfo/include")

add_subdirectory(3rdparty/fast_float EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/lzma EXCLUDE_FROM_ALL)
# 7-Zip's AES code is for encrypted archives, which nothing here reads, and its 256-bit path
# uses VAES intrinsics the console's compiler target does not declare.
get_target_property(_lzma_sources pcsx2-lzma SOURCES)
list(REMOVE_ITEM _lzma_sources src/Aes.c src/AesOpt.c)
set_target_properties(pcsx2-lzma PROPERTIES SOURCES "${_lzma_sources}")
add_subdirectory(3rdparty/libchdr EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(libchdr)
# CHD images on a network share are read through the port's virtual file layer.
ps5_redirect_stdio(libchdr)
add_subdirectory(3rdparty/soundtouch EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/simpleini EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/imgui EXCLUDE_FROM_ALL)
# No program to hand a link to: ImGui's default opens one with fork() and execvp().
target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
add_subdirectory(3rdparty/libzip EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rcheevos EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rapidjson EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/discord-rpc EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/freesurround EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/vulkan EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/demangler EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/ccc EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/zydis EXCLUDE_FROM_ALL)

# Prevent fmt from being built with exceptions, or being thrown at call sites.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DFMT_USE_EXCEPTIONS=0 -DFMT_USE_RTTI=0")
add_subdirectory(3rdparty/fmt EXCLUDE_FROM_ALL)

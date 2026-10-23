# Crux2 - libzip for the updater's PC test, from PCSX2's copy (vendor/pcsx2/3rdparty/libzip), for a
# PC without libzip-dev. Deflate (zlib) only: PCSX2's own CMakeLists asks for zstd as well, whose
# headers a PC may not have, and a release ZIP is deflated.
# SPDX-License-Identifier: GPL-3.0+
#
#   cmake -S ps5/tests/update/libzip-pc -B <work>/libzip -DLIBZIP_SOURCE=<vendor/pcsx2/3rdparty/libzip>
cmake_minimum_required(VERSION 3.16)
project(libzip_pc C)

if(NOT LIBZIP_SOURCE)
	message(FATAL_ERROR "LIBZIP_SOURCE: the folder of PCSX2's libzip")
endif()
find_package(ZLIB REQUIRED)

include(CheckTypeSize)
include(CheckFunctionExists)
check_type_size("off_t" SIZEOF_OFF_T)
check_type_size("size_t" SIZEOF_SIZE_T)

set(libzip_VERSION 1.10.1)
set(libzip_VERSION_MAJOR 1)
set(libzip_VERSION_MINOR 10)
set(libzip_VERSION_PATCH 1)
set(CMAKE_PROJECT_NAME libzip)
set(CMAKE_PROJECT_VERSION 1.10.1)
set(ZIP_STATIC TRUE)
set(ENABLE_FDOPEN ON)
set(HAVE_FCHMOD TRUE)
set(HAVE_FILENO TRUE)
set(HAVE_FSEEKO TRUE)
set(HAVE_FTELLO TRUE)
set(HAVE_LOCALTIME_R TRUE)
set(HAVE_RANDOM TRUE)
set(HAVE_SNPRINTF TRUE)
set(HAVE_STRCASECMP TRUE)
set(HAVE_STRDUP TRUE)
set(HAVE_STRTOLL TRUE)
set(HAVE_STRTOULL TRUE)
set(HAVE_STDBOOL_H TRUE)
set(HAVE_STRINGS_H TRUE)
set(HAVE_UNISTD_H TRUE)
set(HAVE_NULLABLE TRUE)
set(HAVE_MKSTEMP TRUE)
set(HAVE_EXPLICIT_BZERO TRUE)
set(LIBZIP_TYPES_INCLUDE "#if !defined(__STDC_FORMAT_MACROS)
#define __STDC_FORMAT_MACROS 1
#endif
#include <inttypes.h>")
foreach(bits 8 16 32 64)
	set(ZIP_INT${bits}_T int${bits}_t)
	set(ZIP_UINT${bits}_T uint${bits}_t)
endforeach()
set(ZIP_NULLABLE_DEFINES "")

configure_file(${LIBZIP_SOURCE}/cmake-config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)
configure_file(${LIBZIP_SOURCE}/cmake-zipconf.h.in ${CMAKE_CURRENT_BINARY_DIR}/zipconf.h)
execute_process(COMMAND ${CMAKE_COMMAND} -DPROJECT_SOURCE_DIR=${LIBZIP_SOURCE}
	-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
	-P ${LIBZIP_SOURCE}/cmake/GenerateZipErrorStrings.cmake
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} RESULT_VARIABLE generated)
if(NOT generated EQUAL 0)
	message(FATAL_ERROR "zip_err_str.c could not be generated")
endif()

file(GLOB sources ${LIBZIP_SOURCE}/lib/zip_*.c)
list(FILTER sources EXCLUDE REGEX "(win32|uwp|crypto_|winzip_aes|algorithm_(bzip2|xz|zstd)|zip_random_win)")
add_library(zip STATIC ${sources} ${CMAKE_CURRENT_BINARY_DIR}/zip_err_str.c)
target_include_directories(zip PUBLIC ${LIBZIP_SOURCE}/lib ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(zip PUBLIC ZLIB::ZLIB)
target_compile_options(zip PRIVATE -w)

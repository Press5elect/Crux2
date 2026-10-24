# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
# Crux2 - paraLLEl-GS (Arntzen Software, LGPL-3.0-or-later) on Granite's Vulkan backend (Hans-Kristian
# Arntzen, MIT) with its own volk (MIT), and def-jam-vendetta-recomp's renderer for it (jcsix694,
# GPL-3.0; gs_gpu_parallel.cpp, with Crux2 Recomp's changes), as one relocatable object whose names are
# all its own but makeGpuRenderer: its volk and its newer Vulkan headers never meet PCSX2's.
# The sources are Crux2 Recomp's (PS5_Crux2Recomp/vendor, fetched by its tools/fetch-parallel-gs.sh and
# tools/fetch-ps2recomp.sh at the revisions they pin). Made by ps5/tools/build-parallel-gs.sh.
cmake_minimum_required(VERSION 3.20)
project(crux2_parallel_gs C CXX)

if(NOT PGS OR NOT PS2R OR NOT PS5_PAYLOAD_SDK)
	message(FATAL_ERROR "PGS, PS2R and PS5_PAYLOAD_SDK are set by ps5/tools/build-parallel-gs.sh")
endif()
set(GRANITE ${PGS}/Granite)

set(sources
	${PGS}/gs/gs_interface.cpp ${PGS}/gs/gs_util.cpp ${PGS}/gs/page_tracker.cpp ${PGS}/gs/gs_renderer.cpp
	${PGS}/dump/gs_dump_generator.cpp
	${GRANITE}/third_party/volk/volk.c
	${PS2R}/ps2xRuntime/src/lib/gs/gs_gpu_parallel.cpp)
foreach(f context device wsi wsi_pacer buffer_pool image cookie sampler command_pool fence_manager descriptor_set
		semaphore_manager command_buffer shader render_pass buffer rtas indirect_layout pipeline_cache semaphore
		memory_allocator fence event_manager breadcrumbs pipeline_event query_pool texture/texture_format)
	list(APPEND sources ${GRANITE}/vulkan/${f}.cpp)
endforeach()
foreach(f logging aligned_alloc timer thread_id string_helpers timeline_trace_file thread_name thread_priority
		cli_parser dynamic_library message_queue arena_allocator environment slab_allocator)
	list(APPEND sources ${GRANITE}/util/${f}.cpp)
endforeach()
foreach(f math frustum aabb interpolation muglm/muglm transforms)
	list(APPEND sources ${GRANITE}/math/${f}.cpp)
endforeach()

add_library(paragsobj OBJECT ${sources})
target_include_directories(paragsobj PRIVATE
	${PGS}/gs ${PGS}/dump ${GRANITE}/vulkan ${GRANITE}/vulkan/texture ${GRANITE}/util ${GRANITE}/math
	${GRANITE}/application/global ${GRANITE}/third_party/volk
	${GRANITE}/third_party/khronos/vulkan-headers/include
	${PS2R}/ps2xRuntime/include ${PS2R}/ps2xRuntime/src/lib ${PS2R}/ps2xRuntime/src/lib/gs)
target_compile_definitions(paragsobj PRIVATE GRANITE_SHIPPING VK_NO_PROTOTYPES NDEBUG CRUX2_RECOMP_HOST)
set_target_properties(paragsobj PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 11)
target_compile_options(paragsobj PRIVATE -O3 -g1 -march=znver2 -w -ffunction-sections -fno-omit-frame-pointer
	-fvisibility=hidden -fvisibility-inlines-hidden)

add_custom_command(
	OUTPUT ${CMAKE_BINARY_DIR}/parallel-gs.o
	COMMAND ${PS5_PAYLOAD_SDK}/bin/ld.lld -r -o ${CMAKE_BINARY_DIR}/parallel-gs.o "$<TARGET_OBJECTS:paragsobj>"
	COMMAND ${PS5_PAYLOAD_SDK}/bin/llvm-objcopy --keep-global-symbol=_Z15makeGpuRendererRK12GSGpuOptionsRNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE ${CMAKE_BINARY_DIR}/parallel-gs.o
	DEPENDS paragsobj "$<TARGET_OBJECTS:paragsobj>"
	COMMAND_EXPAND_LISTS
	VERBATIM)
add_custom_target(parallelgs ALL DEPENDS ${CMAKE_BINARY_DIR}/parallel-gs.o)

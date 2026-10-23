# Crux2 - the port layer: the host (launch, main loop), the interface, the pad, audio
# output, the crash report and the virtual file layer.

# The virtual file layer is plain C++ with no PCSX2 in it (ps5/tests builds it for the PC).
add_library(ps5vfs STATIC
	vfs/ps5_vfs.cpp
	vfs/ps5_vfs_smb.cpp
	vfs/ps5_vfs_ftp.cpp
	vfs/ps5_vfs_nfs.cpp
)
target_include_directories(ps5vfs PUBLIC vfs)
# (libnfs's headers are in include/nfsc there, libsmb2's in include/smb2.)
target_include_directories(ps5vfs PRIVATE "${PS5PX_SYSROOT}/include")
target_compile_options(ps5vfs PRIVATE -fexceptions)
# libnfs, built by tools/build-deps.sh; it calls back into this library for getservbyport, getuid
# and getgid (ps5_vfs_nfs.cpp).
target_link_libraries(ps5vfs INTERFACE "${PS5PX_SYSROOT}/lib/libnfs.a")

set(PS5PX_FRONTEND_SOURCES
	../src/platform.c
	../elevation/elevation.cpp
	ps5_audio_stream.cpp
	ps5_backup.cpp
	ps5_cards.cpp
	ps5_build_stamp.cpp
	ps5_crash.cpp
	ps5_disc.cpp
	ps5_disc_image.cpp
	ps5_about.cpp
	ps5_native.cpp
	ps5_native_loader.cpp
	ps5_extras.cpp
	ps5_host.cpp
	ps5_input_source.cpp
	ps5_jobs.cpp
	ps5_libc.c
	ps5_clock.c
	ps5_pgo.cpp
	ps5_logs.cpp
	ps5_net.cpp
	ps5_online.cpp
	ps5_pacing.cpp
	ps5_boot_step.cpp
	ps5_pgs.cpp
	ps5_thread_layout.cpp
	ps5_free_bios.cpp
	ps5_auto_quality.cpp
	ps5_card_sync.cpp
	ps5_texture_packs.cpp
	ps5_texture_packs_extract.cpp
	ps5_ps2_ftp.cpp
	ps5_mic.cpp
	ps5_priority.cpp
	ps5_services.cpp
	ps5_update.cpp
	ps5_web.cpp
	ps5_keyboard.cpp
	ps5_usbpad.cpp
	ps5_usbpad_console.cpp
	third_party/qrcodegen/qrcodegen.c
	ps5_platform.cpp
	ps5_profiler.cpp
	ps5_stubs.cpp
	ps5_ui_audio.cpp
	ps5_ui_audio_decoders.cpp
	ps5_ui_audio_dsp.cpp
	# The interface: the screens (no PCSX2 in them; ps5/tests/bigpicture builds them for the PC)
	# and their Backend over PCSX2, which is also PCSX2's FullscreenUI on the console.
	bigpicture/bp_draw.cpp
	bigpicture/bp_library.cpp
	bigpicture/bp_options.cpp
	bigpicture/bp_pcsx2.cpp
	bigpicture/bp_care.cpp
	bigpicture/bp_cards.cpp
	bigpicture/bp_quick.cpp
	bigpicture/bp_settings.cpp
	bigpicture/bp_splash.cpp
)

add_library(pcsx2-ps5 STATIC ${PS5PX_FRONTEND_SOURCES})
target_include_directories(pcsx2-ps5 PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/../src"
	"${PS5PX_SYSROOT}/include"
	"${CMAKE_BINARY_DIR}/ps5px-generated" # ps5_resource_manifest.inc (tools/build.sh)
)
target_link_libraries(pcsx2-ps5 PRIVATE PCSX2_FLAGS PCSX2 ps5vfs libzip::zip rapidjson LZMA::LZMA Zstd::Zstd ZLIB::ZLIB)
# (The QR code generator asserts with a function no system module exports.)
set_source_files_properties(third_party/qrcodegen/qrcodegen.c PROPERTIES COMPILE_DEFINITIONS NDEBUG)

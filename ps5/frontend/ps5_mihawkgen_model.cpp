// Crux2 - MihawkGen's trained network, assembled into the title.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// third_party/mihawkgen/model/network.fgw is Mihawk's network (432,308 parameters,
// GPL-3.0-or-later, https://github.com/mihawk-99/PS5_MihawkGen), read by MihawkGen's own loader
// (fg_context_load_network) from this blob.
#include <cstddef>

#ifndef MIHAWKGEN_MODEL_PATH
#error "MIHAWKGEN_MODEL_PATH is the network's file (vendor PCSX2's CMakeLists.txt, ps5.patch)"
#endif

__asm__(".section .rodata\n"
		".balign 16\n"
		".global crux2_mihawkgen_model\n"
		"crux2_mihawkgen_model:\n"
		".incbin \"" MIHAWKGEN_MODEL_PATH "\"\n"
		".global crux2_mihawkgen_model_end\n"
		"crux2_mihawkgen_model_end:\n"
		".byte 0\n"
		".previous\n");

extern "C" const unsigned char crux2_mihawkgen_model[];
extern "C" const unsigned char crux2_mihawkgen_model_end[];

namespace PS5MihawkGen
{
const void* Model()
{
	return crux2_mihawkgen_model;
}

size_t ModelSize()
{
	return static_cast<size_t>(crux2_mihawkgen_model_end - crux2_mihawkgen_model);
}
} // namespace PS5MihawkGen

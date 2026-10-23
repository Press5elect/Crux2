// Crux2 - AMD FidelityFX Super Resolution 1.0 for PCSX2's Vulkan renderer: the two passes.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// FSR_PASS 0 is EASU, the edge-adaptive upscale: it reads the game's picture as it was drawn and
// writes it at the size it is shown at. FSR_PASS 1 is RCAS, the sharpening that follows it, at
// that size. The filters themselves are AMD's (shaders/common/fsr1/ffx_a.h and ffx_fsr1.h, MIT,
// taken unchanged from FidelityFX-FSR v1.0.2); this file gives them their inputs, the way
// PCSX2's cas.glsl does for AMD's CAS. The vertex shader is convert.glsl's. The constants are
// worked out on the processor (GSDevice::FSR), as FsrEasuConOffset and FsrRcasCon would.
//
// The picture is in the colour space a television is sent, which is what both filters expect.

#ifdef FRAGMENT_SHADER

layout(push_constant) uniform cb_fsr
{
	uvec4 u_con0;
	uvec4 u_con1;
	uvec4 u_con2;
	uvec4 u_con3;
};

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(location = 0) out vec4 o_col0;

#define A_GPU 1
#define A_GLSL 1

#include "fsr1/ffx_a.h"

#if FSR_PASS == 0

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(samp0, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(samp0, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(samp0, p, 2); }

#include "fsr1/ffx_fsr1.h"

void main()
{
	AF3 c;
	FsrEasuF(c, AU2(gl_FragCoord.xy), u_con0, u_con1, u_con2, u_con3);
	o_col0 = vec4(c, 1.0f);
}

#else

#define FSR_RCAS_F 1
// (At the picture's edge the filter asks for the pixel beyond it: the edge's own is given.)
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(samp0, clamp(p, ASU2(0), textureSize(samp0, 0) - ASU2(1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "fsr1/ffx_fsr1.h"

void main()
{
	AF3 c;
	FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), u_con0);
	o_col0 = vec4(c, 1.0f);
}

#endif

#endif

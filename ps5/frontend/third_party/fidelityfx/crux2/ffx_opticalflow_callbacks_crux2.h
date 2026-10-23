// Crux2 - the resources AMD's FSR 3 optical flow passes read and write, as Crux2 binds them.
// SPDX-License-Identifier: GPL-3.0+
//
// Derived from AMD's ffx_opticalflow_callbacks_glsl.h (FidelityFX SDK 1.1.4, MIT; the passes it
// serves are byte for byte those of FidelityFX SDK 2.3.0), whose notice follows:
//
//   This file is part of the FidelityFX SDK.
//   Copyright (C) 2024 Advanced Micro Devices, Inc.
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files(the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and /or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions: The above copyright
//   notice and this permission notice shall be included in all copies or substantial portions of
//   the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
//
// What is Crux2's own here, and why:
//  * The constants are push constants (one block for every pass, 48 bytes), not uniform buffers:
//    the title records a pass per pyramid level and a push is all that changes between them.
//  * Every load clamps to the picture and every store is dropped outside it. AMD's passes read a
//    texel past the edge in places (the 3x3 filter, the scale pass's neighbours) and rely on
//    Direct3D's rule that such a read gives zero; Vulkan without robustImageAccess gives no such
//    promise, and a repeated edge is a better answer than zero anyway.
//  * The colour is read through a linear sampler at the luma's own size, so the flow can run on a
//    smaller picture than the game's (Crux2's quality setting) with each luma texel the mean of
//    what it covers.
//  * Scene change detection compares the two pictures it is given, not this frame with the last:
//    the module runs the histogram and divergence passes once for the older picture and once for
//    the newer. The divergence pass of the SDK writes the "previous" histogram and clears the
//    histogram from some workgroups while other workgroups of the same dispatch still read them;
//    here the previous histogram is read from one image and written to another (the two swap
//    roles between the two runs), and the histogram is cleared by the host before it is filled,
//    so no workgroup can see another's write. Only the newest result counts for a cut (bit 0 of
//    the history), and the SDK's "the first five frames are a cut" rule is gone: every call is a
//    pair of pictures of its own. The search and scale passes do not stop at a cut (IsSceneChanged
//    is false for them): Crux2's verdict pass decides from the detector and the vectors together.
//  * The search pass's wave reductions are reductions over its 64-thread workgroup (the SDK
//    assumes waves of 64, or two of 32). They are done here with subgroup operations and shared
//    memory for any subgroup size, so the pass gives the same answer on RADV (64) and on a
//    software driver (8).
//  * The second picture's luma is scaled to the first one's mean brightness before it is compared
//    (a gain each way, measured by Crux2's pass OF_PASS 7 on a coarse pyramid level): the SDK
//    compares raw luma, and a picture that fades by a quarter then matches nowhere.
//  * The luma pictures are R32_UINT and the vectors RGBA16_SINT (the SDK: R8_UINT, R16G16_SINT).
//    Those two are storage formats only with shaderStorageImageExtendedFormats, which PCSX2 does
//    not enable; these are storage formats on every Vulkan device. (The memory this costs is a
//    few megabytes at the sizes the flow runs at.)

#ifndef CRUX2_OPTICALFLOW_CALLBACKS_H
#define CRUX2_OPTICALFLOW_CALLBACKS_H

#include "opticalflow/ffx_opticalflow_resources.h"

// GLSL has no msad4; the rest are the SDK's GLSL settings.
#define FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION 0
#define FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS 1
#define FFX_OPTICALFLOW_USE_HEURISTICS 1
#define FFX_OPTICALFLOW_BLOCK_SIZE 8
#define FFX_LOCAL_SEARCH_FALLBACK 1

#include "ffx_core.h"

#ifndef FFX_PREFER_WAVE64
#define FFX_PREFER_WAVE64
#endif

#include "opticalflow/ffx_opticalflow_common.h"

// ---- constants: AMD's cbOF and cbOF_SPD in one push constant block.
layout(push_constant, std430) uniform Crux2OpticalFlowConstants
{
	FfxInt32x2 iInputLumaResolution; // the luma picture's size (level 0), not its padded image's
	FfxUInt32 uOpticalFlowPyramidLevel;
	FfxUInt32 uOpticalFlowPyramidLevelCount;
	FfxUInt32 uDirection; // (the SDK's frame index, which no pass reads): 0 or 1, the luma gain to use
	FfxUInt32 backbufferTransferFunction; // 0: SDR (what a PS2 picture is)
	FfxFloat32x2 minMaxLuminance;
	FfxUInt32 mips;
	FfxUInt32 numWorkGroups;
	FfxUInt32x2 workGroupOffset;
} cbOF;

FfxInt32x2 DisplaySize()
{
	return cbOF.iInputLumaResolution;
}

FfxUInt32 FrameIndex()
{
	return 0u;
}

FfxUInt32 BackbufferTransferFunction()
{
	return cbOF.backbufferTransferFunction;
}

FfxFloat32x2 MinMaxLuminance()
{
	return cbOF.minMaxLuminance;
}

FfxBoolean CrossedSceneChangeThreshold(FfxFloat32 sceneChangeValue)
{
	// The SDK's threshold.
	return sceneChangeValue > 0.45f;
}

FfxUInt32 OpticalFlowPyramidLevel()
{
	return cbOF.uOpticalFlowPyramidLevel;
}

FfxUInt32 OpticalFlowPyramidLevelCount()
{
	return cbOF.uOpticalFlowPyramidLevelCount;
}

FfxInt32x2 OpticalFlowHistogramMaxVelocity()
{
	const FfxInt32 searchRadius = 8;
	FfxInt32 scale = FfxInt32(1) << (OpticalFlowPyramidLevelCount() - 1 - OpticalFlowPyramidLevel());
	FfxInt32 maxVelocity = searchRadius * scale;
	return FfxInt32x2(maxVelocity, maxVelocity);
}

FfxUInt32 NumWorkGroups()
{
	return cbOF.numWorkGroups;
}

// ---- resources

#if defined FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR) uniform texture2D r_input_color;
layout(set = 0, binding = CRUX2_OPTICALFLOW_BIND_LINEAR_SAMPLER) uniform sampler s_linear_clamp;
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT) uniform utexture2D r_optical_flow_input;
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT) uniform utexture2D r_optical_flow_previous_input;
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW) uniform itexture2D r_optical_flow;
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS) uniform itexture2D r_optical_flow_previous;
#endif
#if defined CRUX2_OPTICALFLOW_BIND_SRV_LUMA_GAIN
layout(set = 0, binding = CRUX2_OPTICALFLOW_BIND_SRV_LUMA_GAIN) uniform texture2D r_luma_gain;
#endif

#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT, r32ui) uniform uimage2D rw_optical_flow_input;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_1
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_1, r32ui) uniform uimage2D rw_optical_flow_input_level_1;
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_2, r32ui) uniform uimage2D rw_optical_flow_input_level_2;
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_3, r32ui) uniform uimage2D rw_optical_flow_input_level_3;
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_4, r32ui) uniform uimage2D rw_optical_flow_input_level_4;
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_5, r32ui) uniform uimage2D rw_optical_flow_input_level_5;
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_6, r32ui) uniform uimage2D rw_optical_flow_input_level_6;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW, rgba16i) uniform iimage2D rw_optical_flow;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL, rgba16i) uniform iimage2D rw_optical_flow_next_level;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM, r32ui) uniform uimage2D rw_optical_flow_scd_histogram;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM
// Read from one image, written to the other (see the top of this file).
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM, r32f) uniform readonly image2D r_optical_flow_scd_previous_histogram;
layout(set = 0, binding = CRUX2_OPTICALFLOW_BIND_UAV_SCD_NEXT_HISTOGRAM, r32f) uniform writeonly image2D w_optical_flow_scd_next_histogram;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP
// (coherent: the last workgroup to finish reads what the others added.)
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP, r32ui) coherent uniform uimage2D rw_optical_flow_scd_temp;
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT
layout(set = 0, binding = FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT, r32ui) coherent uniform uimage2D rw_optical_flow_scd_output;
#endif

FfxInt32x2 Crux2Clamp(FfxInt32x2 pos, FfxInt32x2 size)
{
	return clamp(pos, FfxInt32x2(0, 0), size - FfxInt32x2(1, 1));
}

FfxBoolean Crux2Inside(FfxInt32x2 pos, FfxInt32x2 size)
{
	return all(greaterThanEqual(pos, FfxInt32x2(0, 0))) && all(lessThan(pos, size));
}

#if defined(FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR)
FfxFloat32x4 LoadInputColor(FfxUInt32x2 iPxHistory)
{
	// Each luma texel is the colour picture under it: a bilinear read in the middle of the texel,
	// which at a smaller luma size is the mean of the four nearest texels. Positions past the
	// picture (the padding up to a whole SPD tile) repeat its edge.
	const FfxFloat32x2 uv = (FfxFloat32x2(min(FfxInt32x2(iPxHistory), DisplaySize() - 1)) + 0.5) / FfxFloat32x2(DisplaySize());
	return textureLod(sampler2D(r_input_color, s_linear_clamp), uv, 0.0);
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT)
void StoreOpticalFlowInput(FfxInt32x2 iPxPos, FfxUInt32 fLuma)
{
	if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input)))
		imageStore(rw_optical_flow_input, iPxPos, FfxUInt32x4(min(fLuma, 255u), 0, 0, 0));
}

FfxUInt32 LoadRwOpticalFlowInput(FfxInt32x2 iPxPos)
{
	return imageLoad(rw_optical_flow_input, Crux2Clamp(iPxPos, imageSize(rw_optical_flow_input))).x;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT)
FfxUInt32 LoadOpticalFlowInput(FfxInt32x2 iPxPos)
{
	return texelFetch(r_optical_flow_input, Crux2Clamp(iPxPos, textureSize(r_optical_flow_input, 0)), 0).x;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT)
FfxUInt32 LoadOpticalFlowPreviousInput(FfxInt32x2 iPxPos)
{
	return texelFetch(r_optical_flow_previous_input, Crux2Clamp(iPxPos, textureSize(r_optical_flow_previous_input, 0)), 0).x;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW)
FfxInt32x2 LoadOpticalFlow(FfxInt32x2 iPxPos)
{
	return texelFetch(r_optical_flow, Crux2Clamp(iPxPos, textureSize(r_optical_flow, 0)), 0).xy;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW)
FfxInt32x2 LoadRwOpticalFlow(FfxInt32x2 iPxPos)
{
	return imageLoad(rw_optical_flow, Crux2Clamp(iPxPos, imageSize(rw_optical_flow))).xy;
}

void StoreOpticalFlow(FfxInt32x2 iPxPos, FfxInt32x2 motionVector)
{
	if (Crux2Inside(iPxPos, imageSize(rw_optical_flow)))
		imageStore(rw_optical_flow, iPxPos, FfxInt32x4(motionVector, 0, 0));
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS)
FfxInt32x2 LoadPreviousOpticalFlow(FfxInt32x2 iPxPos)
{
	return texelFetch(r_optical_flow_previous, Crux2Clamp(iPxPos, textureSize(r_optical_flow_previous, 0)), 0).xy;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL)
void StoreOpticalFlowNextLevel(FfxInt32x2 iPxPos, FfxInt32x2 motionVector)
{
	if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_next_level)))
		imageStore(rw_optical_flow_next_level, iPxPos, FfxInt32x4(motionVector, 0, 0));
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM)
FfxUInt32 LoadRwSCDHistogram(FfxInt32 iIndex)
{
	return imageLoad(rw_optical_flow_scd_histogram, FfxInt32x2(iIndex, 0)).x;
}

void StoreSCDHistogram(FfxInt32 iIndex, FfxUInt32 value)
{
	// The divergence pass clears the histogram for the next frame from some of its workgroups
	// while others still read it; the host clears it before it is filled instead.
}

void AtomicIncrementSCDHistogram(FfxInt32 iIndex, FfxUInt32 valueToAdd)
{
	imageAtomicAdd(rw_optical_flow_scd_histogram, FfxInt32x2(iIndex, 0), valueToAdd);
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM)
FfxFloat32 LoadRwSCDPreviousHistogram(FfxInt32 iIndex)
{
	return imageLoad(r_optical_flow_scd_previous_histogram, FfxInt32x2(iIndex, 0)).x;
}

void StoreSCDPreviousHistogram(FfxInt32 iIndex, FfxFloat32 value)
{
	imageStore(w_optical_flow_scd_next_histogram, FfxInt32x2(iIndex, 0), FfxFloat32x4(value, 0.0, 0.0, 0.0));
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP)
FfxUInt32 LoadRwSCDTemp(FfxInt32 iIndex)
{
	return imageLoad(rw_optical_flow_scd_temp, FfxInt32x2(iIndex, 0)).x;
}

void AtomicIncrementSCDTemp(FfxInt32 iIndex, FfxUInt32 valueToAdd)
{
	imageAtomicAdd(rw_optical_flow_scd_temp, FfxInt32x2(iIndex, 0), valueToAdd);
}

void ResetSCDTemp()
{
	imageStore(rw_optical_flow_scd_temp, FfxInt32x2(0, 0), FfxUInt32x4(0, 0, 0, 0));
	imageStore(rw_optical_flow_scd_temp, FfxInt32x2(1, 0), FfxUInt32x4(0, 0, 0, 0));
	imageStore(rw_optical_flow_scd_temp, FfxInt32x2(2, 0), FfxUInt32x4(0, 0, 0, 0));
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT)
FfxUInt32 LoadRwSCDOutput(FfxInt32 iIndex)
{
	return imageLoad(rw_optical_flow_scd_output, FfxInt32x2(iIndex, 0)).x;
}

void StoreSCDOutput(FfxInt32 iIndex, FfxUInt32 value)
{
	imageStore(rw_optical_flow_scd_output, FfxInt32x2(iIndex, 0), FfxUInt32x4(value, 0, 0, 0));
}

FfxUInt32 AtomicIncrementSCDOutput(FfxInt32 iIndex, FfxUInt32 valueToAdd)
{
	return imageAtomicAdd(rw_optical_flow_scd_output, FfxInt32x2(iIndex, 0), valueToAdd);
}

FfxFloat32 GetSceneChangeValue()
{
	return ffxAsFloat(LoadRwSCDOutput(SCD_OUTPUT_SCENE_CHANGE_SLOT));
}

FfxBoolean IsSceneChanged()
{
	// The search and scale passes zero every vector after a cut. Here the flow is always found:
	// FSR 3's detector also fires on a fade or a flash, which the vectors (and Crux2's verdict
	// pass, fg_warp.comp, that weighs them) see through, and a record costs the same either way.
	return false;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT)
FfxUInt32 LoadFirstImagePackedLuma(FfxInt32x2 iPxPos)
{
	const FfxInt32 lumaTextureWidth = DisplaySize().x >> OpticalFlowPyramidLevel();
	const FfxInt32 lumaTextureHeight = DisplaySize().y >> OpticalFlowPyramidLevel();
	FfxInt32x2 adjustedPos = FfxInt32x2(ffxClamp(iPxPos.x, 0, lumaTextureWidth - 4), ffxClamp(iPxPos.y, 0, lumaTextureHeight - 1));
	FfxUInt32 luma0 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(0, 0));
	FfxUInt32 luma1 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(1, 0));
	FfxUInt32 luma2 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(2, 0));
	FfxUInt32 luma3 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(3, 0));
	return GetPackedLuma(lumaTextureWidth, iPxPos.x, luma0, luma1, luma2, luma3);
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT)
#if defined CRUX2_OPTICALFLOW_BIND_SRV_LUMA_GAIN
// The second picture's luma brought to the first one's brightness (the gain pass measured both):
// the search's sum of absolute differences then finds the match across a fade or a flash.
FfxUInt32 Crux2Gain(FfxUInt32 luma, FfxFloat32 gain)
{
	return min(FfxUInt32(FfxFloat32(luma) * gain + 0.5), 255u);
}
#endif

FfxUInt32 LoadSecondImagePackedLuma(FfxInt32x2 iPxPos)
{
	const FfxInt32 lumaTextureWidth = DisplaySize().x >> OpticalFlowPyramidLevel();
	const FfxInt32 lumaTextureHeight = DisplaySize().y >> OpticalFlowPyramidLevel();
	FfxInt32x2 adjustedPos = FfxInt32x2(ffxClamp(iPxPos.x, 0, lumaTextureWidth - 4), ffxClamp(iPxPos.y, 0, lumaTextureHeight - 1));
	FfxUInt32 luma0 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(0, 0));
	FfxUInt32 luma1 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(1, 0));
	FfxUInt32 luma2 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(2, 0));
	FfxUInt32 luma3 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(3, 0));
#if defined CRUX2_OPTICALFLOW_BIND_SRV_LUMA_GAIN
	const FfxFloat32 gain = texelFetch(r_luma_gain, FfxInt32x2(0, 0), 0)[cbOF.uDirection & 1u];
	if (gain != 1.0)
	{
		luma0 = Crux2Gain(luma0, gain);
		luma1 = Crux2Gain(luma1, gain);
		luma2 = Crux2Gain(luma2, gain);
		luma3 = Crux2Gain(luma3, gain);
	}
#endif
	return GetPackedLuma(lumaTextureWidth, iPxPos.x, luma0, luma1, luma2, luma3);
}
#endif

#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_1
void SPD_SetMipmap(FfxInt32x2 iPxPos, FfxUInt32 index, FfxFloat32 value)
{
	FfxUInt32x4 value4 = FfxUInt32x4(value, 0.0, 0.0, 0.0);
	switch (index)
	{
		case 0:
			if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input_level_1)))
				imageStore(rw_optical_flow_input_level_1, iPxPos, value4);
			break;
		case 1:
			if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input_level_2)))
				imageStore(rw_optical_flow_input_level_2, iPxPos, value4);
			break;
		case 2:
			if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input_level_3)))
				imageStore(rw_optical_flow_input_level_3, iPxPos, value4);
			break;
		case 3:
			if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input_level_4)))
				imageStore(rw_optical_flow_input_level_4, iPxPos, value4);
			break;
		case 4:
			if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input_level_5)))
				imageStore(rw_optical_flow_input_level_5, iPxPos, value4);
			break;
		case 5:
			if (Crux2Inside(iPxPos, imageSize(rw_optical_flow_input_level_6)))
				imageStore(rw_optical_flow_input_level_6, iPxPos, value4);
			break;
	}
}
#endif

#if defined(CRUX2_WORKGROUP_REDUCTIONS)
// The search pass's "wave" sum and minimum, over its whole 64-thread workgroup whatever the
// subgroup size; every invocation of the workgroup calls them together, as the SDK's do.
shared FfxUInt32 s_crux2_reduce[64];

FfxUInt32 Crux2WorkgroupReduce(FfxUInt32 value, FfxBoolean isMin)
{
	FfxUInt32 part = isMin ? subgroupMin(value) : subgroupAdd(value);
	if (gl_NumSubgroups == 1u)
		return part;
	barrier(); // (the last call's reads are done)
	if (subgroupElect())
		s_crux2_reduce[gl_SubgroupID] = part;
	barrier();
	FfxUInt32 total = s_crux2_reduce[0];
	for (FfxUInt32 i = 1u; i < gl_NumSubgroups; i++)
		total = isMin ? min(total, s_crux2_reduce[i]) : total + s_crux2_reduce[i];
	return total;
}

FfxUInt32 Crux2WorkgroupSum(FfxUInt32 value)
{
	return Crux2WorkgroupReduce(value, false);
}

FfxUInt32 Crux2WorkgroupMin(FfxUInt32 value)
{
	return Crux2WorkgroupReduce(value, true);
}

#define ffxWaveSum Crux2WorkgroupSum
#define ffxWaveMin Crux2WorkgroupMin
#define ffxWaveLaneCount() 64u
#endif

#endif // CRUX2_OPTICALFLOW_CALLBACKS_H

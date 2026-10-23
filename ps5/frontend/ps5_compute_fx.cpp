// Crux2 - frame generation from AMD's FSR 3 optical flow, and NVIDIA Image Scaling, as Vulkan compute.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The host side of ps5_compute_fx.h. For the optical flow it does what AMD's ffx_opticalflow.cpp
// (FidelityFX SDK 2.3.0, MIT) does for one frame: the same passes in the same order with the
// same sizes and dispatch counts, written against Vulkan directly instead of through the SDK's
// backend interface, and run for a pair of pictures rather than for "this frame and the last"
// (see third_party/fidelityfx/crux2/ffx_opticalflow_callbacks_crux2.h for every difference).
// For NIS it does what NVIDIA's samples do (NVIDIAImageScaling v1.0.3, MIT): NVScalerUpdateConfig
// for the constants, NVIDIA's coefficient tables in two small textures, a dispatch per block.
//
// Every image the effects own lives in VK_IMAGE_LAYOUT_GENERAL from the first record on; between
// two passes that depend on each other there is one global memory barrier (compute writes before
// compute reads), which on AMD hardware is what an image barrier without a layout change costs
// anyway.

#include "ps5_compute_fx.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/nis/NIS_Config.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace PS5ComputeFX
{
	namespace
	{
#include "ps5_compute_fx_spirv.inc"

		// ---- small helpers

		struct Spirv
		{
			const uint32_t* code;
			size_t bytes;
		};
#define CRUX2_SPIRV(name) Spirv{kSpirv##name, sizeof(kSpirv##name)}

		// A pipeline's bindings, one letter each: S a sampled image, X a sampler, I a storage image.
		// (Binding numbers count from `first`.)
		bool MakeSetLayout(const Device& d, const char* spec, uint32_t first, VkDescriptorSetLayout* out)
		{
			VkDescriptorSetLayoutBinding bindings[16];
			uint32_t count = 0;
			for (const char* c = spec; *c && count < 16; c++, count++)
			{
				bindings[count] = {};
				bindings[count].binding = first + count;
				bindings[count].descriptorCount = 1;
				bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				bindings[count].descriptorType = (*c == 'S') ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
												 (*c == 'X') ? VK_DESCRIPTOR_TYPE_SAMPLER :
															   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			}
			VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			info.bindingCount = count;
			info.pBindings = bindings;
			return vkCreateDescriptorSetLayout(d.device, &info, d.allocator, out) == VK_SUCCESS;
		}

		bool MakePipeline(const Device& d, VkDescriptorSetLayout setLayout, uint32_t pushBytes, const Spirv& spirv, VkPipelineLayout* layout,
			VkPipeline* pipeline)
		{
			const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
			VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layoutInfo.setLayoutCount = 1;
			layoutInfo.pSetLayouts = &setLayout;
			layoutInfo.pushConstantRangeCount = pushBytes ? 1 : 0;
			layoutInfo.pPushConstantRanges = &range;
			if (vkCreatePipelineLayout(d.device, &layoutInfo, d.allocator, layout) != VK_SUCCESS)
				return false;
			VkShaderModuleCreateInfo moduleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			moduleInfo.codeSize = spirv.bytes;
			moduleInfo.pCode = spirv.code;
			VkShaderModule module;
			if (vkCreateShaderModule(d.device, &moduleInfo, d.allocator, &module) != VK_SUCCESS)
				return false;
			VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			info.stage.module = module;
			info.stage.pName = "main";
			info.layout = *layout;
			const VkResult result = vkCreateComputePipelines(d.device, d.pipelineCache, 1, &info, d.allocator, pipeline);
			vkDestroyShaderModule(d.device, module, d.allocator);
			return result == VK_SUCCESS;
		}

		bool MakeLinearSampler(const Device& d, VkSampler* out)
		{
			VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			info.magFilter = VK_FILTER_LINEAR;
			info.minFilter = VK_FILTER_LINEAR;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.maxLod = 0.0f;
			return vkCreateSampler(d.device, &info, d.allocator, out) == VK_SUCCESS;
		}

		bool MemoryType(const Device& d, uint32_t bits, VkMemoryPropertyFlags wanted, uint32_t* out)
		{
			VkPhysicalDeviceMemoryProperties props;
			vkGetPhysicalDeviceMemoryProperties(d.physicalDevice, &props);
			for (uint32_t i = 0; i < props.memoryTypeCount; i++)
			{
				if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
				{
					*out = i;
					return true;
				}
			}
			return false;
		}

		bool HasFormat(const Device& d, VkFormat format, VkFormatFeatureFlags wanted)
		{
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(d.physicalDevice, format, &props);
			return (props.optimalTilingFeatures & wanted) == wanted;
		}

		// One descriptor set's contents, in binding order: an image and its layout, or the sampler.
		struct Bind
		{
			VkImageView view;
			VkImageLayout layout;
		};

		void WriteSet(const Device& d, VkDescriptorSet set, const char* spec, uint32_t first, const Bind* binds, VkSampler sampler)
		{
			VkDescriptorImageInfo images[16];
			VkWriteDescriptorSet writes[16];
			uint32_t count = 0;
			for (const char* c = spec; *c && count < 16; c++, count++)
			{
				images[count] = {};
				if (*c == 'X')
					images[count].sampler = sampler;
				else
				{
					images[count].imageView = binds[count].view;
					images[count].imageLayout = binds[count].layout;
				}
				writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				writes[count].dstSet = set;
				writes[count].dstBinding = first + count;
				writes[count].descriptorCount = 1;
				writes[count].descriptorType = (*c == 'S') ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
											   (*c == 'X') ? VK_DESCRIPTOR_TYPE_SAMPLER :
															 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				writes[count].pImageInfo = &images[count];
			}
			vkUpdateDescriptorSets(d.device, count, writes, 0, nullptr);
		}

		bool MakePool(const Device& d, uint32_t sets, uint32_t sampled, uint32_t storage, uint32_t samplers, VkDescriptorPool* out)
		{
			const VkDescriptorPoolSize sizes[3] = {
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage}, {VK_DESCRIPTOR_TYPE_SAMPLER, samplers}};
			VkDescriptorPoolCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			info.maxSets = sets;
			info.poolSizeCount = 3;
			info.pPoolSizes = sizes;
			return vkCreateDescriptorPool(d.device, &info, d.allocator, out) == VK_SUCCESS;
		}

		VkDescriptorSet AllocateSet(const Device& d, VkDescriptorPool pool, VkDescriptorSetLayout layout)
		{
			VkDescriptorSetAllocateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			info.descriptorPool = pool;
			info.descriptorSetCount = 1;
			info.pSetLayouts = &layout;
			VkDescriptorSet set = VK_NULL_HANDLE;
			if (vkAllocateDescriptorSets(d.device, &info, &set) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			return set;
		}

		// Compute writes (and transfers) before compute reads and writes: between two dependent
		// passes.
		void ComputeBarrier(VkCommandBuffer cmd)
		{
			VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 1, &barrier, 0, nullptr, 0, nullptr);
		}

		// On the way in: whatever drew or copied the caller's pictures (and the last record of this
		// effect) is done and visible to the passes and to the clears; the output image may have
		// been read by anything.
		void EntryBarrier(VkCommandBuffer cmd, const VkImageMemoryBarrier* images, uint32_t imageCount)
		{
			VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask =
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
					VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, imageCount, images);
		}

		// On the way out: the output's writes are visible to whatever reads it next.
		void ExitBarrier(VkCommandBuffer cmd)
		{
			VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		}

		uint32_t DivideUp(uint32_t a, uint32_t b)
		{
			return (a + b - 1) / b;
		}

		// ---- the passes' constants, laid out as the shaders' push constant blocks

		struct FlowPush // ffx_opticalflow_callbacks_crux2.h
		{
			int32_t lumaWidth, lumaHeight;
			uint32_t level, levelCount;
			uint32_t direction, transferFunction; // (direction: the SDK's frame index, which no pass reads)
			float minLuminance, maxLuminance;
			uint32_t mips, numWorkGroups;
			uint32_t workGroupOffset[2];
		};
		static_assert(sizeof(FlowPush) == 48);

		struct RefinePush // fg_warp.comp, FG_PASS 0
		{
			float pictureWidth, pictureHeight;
			float lumaScaleX, lumaScaleY;
			float blockPxX, blockPxY;
			int32_t fieldWidth, fieldHeight;
			int32_t radius, direction;
		};
		static_assert(sizeof(RefinePush) == 40);

		struct CompositePush // fg_warp.comp, FG_PASS 1
		{
			float pictureWidth, pictureHeight;
			float blockPxX, blockPxY;
			int32_t fieldWidth, fieldHeight;
			float t;
			float oneSidedCost;
			uint32_t flags;
			uint32_t neighbours;
			float tolerance;
			float giveUp;
			float otherCost;
			float pad0, pad1, pad2;
		};
		static_assert(sizeof(CompositePush) == 64);

		struct SizePush // fg_warp.comp, FG_PASS 3
		{
			int32_t fieldWidth, fieldHeight;
		};

		struct CameraPush // fg_warp.comp, FG_PASS 2
		{
			int32_t fieldWidth, fieldHeight;
			int32_t lumaWidth, lumaHeight;
		};

		// The SDK's constants for the scene change detection (ffx_opticalflow.cpp).
		constexpr uint32_t kHistogramBins = 256;
		constexpr uint32_t kHistogramsPerDim = 3;
		constexpr uint32_t kHistogramShifts = 3;

		// Each pass's bindings (MakeSetLayout's letters) and push constant size.
		struct PassSpec
		{
			const char* name;
			const char* bindings;
			uint32_t pushBytes;
		};
		constexpr PassSpec kPassSpecs[] = {
			{"OF prepare luma", "SXI", sizeof(FlowPush)},
			{"OF luma pyramid", "IIIIIII", sizeof(FlowPush)},
			{"OF SCD histogram", "SI", sizeof(FlowPush)},
			{"OF SCD divergence", "IIIII", sizeof(FlowPush)},
			{"OF search", "SSIIS", sizeof(FlowPush)},
			{"OF filter", "SI", sizeof(FlowPush)},
			{"OF scale", "SSSIIS", sizeof(FlowPush)},
			{"FG refine", "SSSXIS", sizeof(RefinePush)},
			{"FG composite", "SSXSSIIS", sizeof(CompositePush)},
			{"FG camera", "SSI", sizeof(CameraPush)},
			{"FG verdict", "SII", sizeof(SizePush)},
			{"OF luma gain", "SSI", sizeof(FlowPush)},
		};

		// The largest luma picture the flow runs on at each quality, in lines.
		constexpr uint32_t kFlowLines[3] = {540, 720, 1080};
	} // namespace

	// ======================================================================================
	// OpticalFlowFG
	// ======================================================================================

	const char* OpticalFlowFG::TimestampName(uint32_t index)
	{
		static const char* const names[kTimestampCount] = {
			"start", "luma + scene change", "flow, current's blocks", "flow, previous's blocks", "camera + refine + verdict", "composite"};
		return (index < kTimestampCount) ? names[index] : "";
	}

	OpticalFlowFG::OpticalFlowFG() = default;

	OpticalFlowFG::~OpticalFlowFG()
	{
		Destroy();
	}

	bool OpticalFlowFG::Fail(const char* error)
	{
		m_error = error;
		Destroy();
		return false;
	}

	bool OpticalFlowFG::MakeImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage)
	{
		image.width = width ? width : 1;
		image.height = height ? height : 1;
		image.format = format;
		image.usage = usage;
		VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = {image.width, image.height, 1};
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_device.device, &info, m_device.allocator, &image.image) != VK_SUCCESS)
			return false;
		m_images.push_back(&image);
		return true;
	}

	// All the images in as few allocations as there are memory types they need (one, on RADV),
	// then their views.
	bool OpticalFlowFG::AllocateImages()
	{
		std::vector<VkMemoryRequirements> reqs(m_images.size());
		for (size_t i = 0; i < m_images.size(); i++)
			vkGetImageMemoryRequirements(m_device.device, m_images[i]->image, &reqs[i]);
		std::vector<bool> placed(m_images.size(), false);
		for (size_t first = 0; first < m_images.size(); first++)
		{
			if (placed[first])
				continue;
			uint32_t type;
			if (!MemoryType(m_device, reqs[first].memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
				!MemoryType(m_device, reqs[first].memoryTypeBits, 0, &type))
				return false;
			// Every image that can live in this type goes in this allocation.
			std::vector<VkDeviceSize> offsets(m_images.size(), 0);
			VkDeviceSize size = 0;
			for (size_t i = first; i < m_images.size(); i++)
			{
				if (placed[i] || !(reqs[i].memoryTypeBits & (1u << type)))
					continue;
				size = (size + reqs[i].alignment - 1) / reqs[i].alignment * reqs[i].alignment;
				offsets[i] = size;
				size += reqs[i].size;
			}
			VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			alloc.allocationSize = size;
			alloc.memoryTypeIndex = type;
			VkDeviceMemory memory;
			if (vkAllocateMemory(m_device.device, &alloc, m_device.allocator, &memory) != VK_SUCCESS)
				return false;
			m_memory.push_back(memory);
			m_memoryBytes += size;
			for (size_t i = first; i < m_images.size(); i++)
			{
				if (placed[i] || !(reqs[i].memoryTypeBits & (1u << type)))
					continue;
				if (vkBindImageMemory(m_device.device, m_images[i]->image, memory, offsets[i]) != VK_SUCCESS)
					return false;
				placed[i] = true;
			}
		}
		for (Image* image : m_images)
		{
			VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = image->image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = image->format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (vkCreateImageView(m_device.device, &view, m_device.allocator, &image->view) != VK_SUCCESS)
				return false;
		}
		return true;
	}

	bool OpticalFlowFG::MakePipelines()
	{
		Spirv composite = CRUX2_SPIRV(FgCompositeUnformatted);
		if (m_outputFormat == VK_FORMAT_R8G8B8A8_UNORM)
			composite = CRUX2_SPIRV(FgCompositeRgba8);
		else if (m_outputFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
			composite = CRUX2_SPIRV(FgCompositeRgba16f);
		const Spirv spirv[kPipelineCount] = {CRUX2_SPIRV(OfPrepareLuma), CRUX2_SPIRV(OfLumaPyramid), CRUX2_SPIRV(OfScdHistogram),
			CRUX2_SPIRV(OfScdDivergence), CRUX2_SPIRV(OfSearch), CRUX2_SPIRV(OfFilter), CRUX2_SPIRV(OfScale), CRUX2_SPIRV(FgRefine), composite,
			CRUX2_SPIRV(FgCamera), CRUX2_SPIRV(FgVerdict), CRUX2_SPIRV(OfLumaGain)};
		for (uint32_t i = 0; i < kPipelineCount; i++)
		{
			if (!MakeSetLayout(m_device, kPassSpecs[i].bindings, 0, &m_setLayouts[i]) ||
				!MakePipeline(m_device, m_setLayouts[i], kPassSpecs[i].pushBytes, spirv[i], &m_pipelineLayouts[i], &m_pipelines[i]))
				return false;
		}
		return true;
	}

	VkDescriptorSet OpticalFlowFG::StaticSet(Pipeline pipeline)
	{
		return AllocateSet(m_device, m_staticPool, m_setLayouts[pipeline]);
	}

	// The sets of the passes that read and write only the effect's own images: made once.
	bool OpticalFlowFG::MakeStaticSets()
	{
		const uint32_t sets = 9 + 3 * 2 * kMaxLevels;
		if (!MakePool(m_device, sets, sets * 3, sets * 7, 1, &m_staticPool))
			return false;
		const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
		for (uint32_t picture = 0; picture < 2; picture++)
		{
			m_pyramidSet[picture] = StaticSet(kLumaPyramid);
			m_histogramSet[picture] = StaticSet(kScdHistogram);
			m_divergenceSet[picture] = StaticSet(kScdDivergence);
			if (!m_pyramidSet[picture] || !m_histogramSet[picture] || !m_divergenceSet[picture])
				return false;
			Bind pyramid[7];
			for (uint32_t level = 0; level < 7; level++)
				pyramid[level] = {m_luma[picture][level < m_levels ? level : m_levels - 1].view, G};
			WriteSet(m_device, m_pyramidSet[picture], kPassSpecs[kLumaPyramid].bindings, 0, pyramid, m_linear);
			const Bind histogram[2] = {{m_luma[picture][0].view, G}, {m_histogram[picture].view, G}};
			WriteSet(m_device, m_histogramSet[picture], kPassSpecs[kScdHistogram].bindings, 0, histogram, m_linear);
			// Run 0 (the previous picture) reads filtered[0] and leaves its own in filtered[1];
			// run 1 (the current picture) reads that and writes filtered[0].
			const Bind divergence[5] = {{m_histogram[picture].view, G}, {m_filtered[picture].view, G}, {m_scdTemp.view, G},
				{m_scdOutput.view, G}, {m_filtered[picture ^ 1].view, G}};
			WriteSet(m_device, m_divergenceSet[picture], kPassSpecs[kScdDivergence].bindings, 0, divergence, m_linear);
		}
		m_cameraSet = StaticSet(kCamera);
		m_verdictSet = StaticSet(kVerdict);
		if (!m_cameraSet || !m_verdictSet)
			return false;
		const Bind camera[3] = {{m_flowFinal[0].view, G}, {m_flowFinal[1].view, G}, {m_camera.view, G}};
		WriteSet(m_device, m_cameraSet, kPassSpecs[kCamera].bindings, 0, camera, m_linear);
		const Bind verdict[3] = {{m_field[0].view, G}, {m_scdOutput.view, G}, {m_verdict.view, G}};
		WriteSet(m_device, m_verdictSet, kPassSpecs[kVerdict].bindings, 0, verdict, m_linear);
		// The two pictures' brightness from a coarse level (the 8th of the size or the coarsest).
		m_lumaGainSet = StaticSet(kLumaGain);
		if (!m_lumaGainSet)
			return false;
		m_gainLevel = (m_levels > 4) ? 3 : m_levels - 1;
		const Bind gain[3] = {{m_luma[0][m_gainLevel].view, G}, {m_luma[1][m_gainLevel].view, G}, {m_lumaGain.view, G}};
		WriteSet(m_device, m_lumaGainSet, kPassSpecs[kLumaGain].bindings, 0, gain, m_linear);
		// Direction 0 looks for the current picture's blocks in the previous picture; 1 the other
		// way. The search refines flowA in place (it holds the guess the coarser level scaled
		// down), the filter writes flowB (or the direction's result), the scale reads flowB and
		// writes the next level's flowA.
		for (uint32_t direction = 0; direction < 2; direction++)
		{
			const uint32_t first = (direction == 0) ? 1 : 0, second = first ^ 1;
			for (uint32_t level = 0; level < m_levels; level++)
			{
				m_searchSet[direction][level] = StaticSet(kSearch);
				m_filterSet[direction][level] = StaticSet(kFilter);
				if (!m_searchSet[direction][level] || !m_filterSet[direction][level])
					return false;
				const Bind search[5] = {{m_luma[first][level].view, G}, {m_luma[second][level].view, G}, {m_flowA[level].view, G},
					{m_scdOutput.view, G}, {m_lumaGain.view, G}};
				WriteSet(m_device, m_searchSet[direction][level], kPassSpecs[kSearch].bindings, 0, search, m_linear);
				const Bind filter[2] = {{m_flowA[level].view, G}, {(level == 0) ? m_flowFinal[direction].view : m_flowB[level].view, G}};
				WriteSet(m_device, m_filterSet[direction][level], kPassSpecs[kFilter].bindings, 0, filter, m_linear);
				if (level > 0)
				{
					m_scaleSet[direction][level] = StaticSet(kScale);
					if (!m_scaleSet[direction][level])
						return false;
					const Bind scale[6] = {{m_luma[first][level].view, G}, {m_luma[second][level].view, G}, {m_flowB[level].view, G},
						{m_flowA[level - 1].view, G}, {m_scdOutput.view, G}, {m_lumaGain.view, G}};
					WriteSet(m_device, m_scaleSet[direction][level], kPassSpecs[kScale].bindings, 0, scale, m_linear);
				}
			}
		}
		return true;
	}

	bool OpticalFlowFG::Create(const Device& device, uint32_t width, uint32_t height, VkFormat outputFormat, Quality quality)
	{
		Destroy();
		m_error = "";
		if (!device.device || !device.physicalDevice || width < 64 || height < 64 || width > 16384 || height > 16384)
		{
			m_error = "no device, or a picture size out of range (64..16384)";
			return false;
		}
		m_device = device;
		if (m_device.framesInFlight == 0)
			m_device.framesInFlight = 1;
		m_width = width;
		m_height = height;
		m_outputFormat = outputFormat;
		m_quality = quality;

		// What the passes need of the device: subgroup sums and minimums in compute shaders (the
		// search), and the formats.
		VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
		VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		props.pNext = &subgroup;
		vkGetPhysicalDeviceProperties2(m_device.physicalDevice, &props);
		if (!(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
			(subgroup.supportedOperations & (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)) !=
				(VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT))
			return Fail("the device has no subgroup arithmetic in compute shaders");
		if (props.properties.limits.maxPushConstantsSize < 128)
			return Fail("the device has under 128 bytes of push constants");
		const VkFormatFeatureFlags storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if (!HasFormat(m_device, VK_FORMAT_R32_UINT, storage | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) ||
			!HasFormat(m_device, VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
			!HasFormat(m_device, VK_FORMAT_R16G16B16A16_SINT, storage) ||
			!HasFormat(m_device, VK_FORMAT_R16G16B16A16_SFLOAT, storage | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
			!HasFormat(m_device, VK_FORMAT_R32G32B32A32_SFLOAT, storage))
			return Fail("the device lacks a storage format the passes use");
		if (!HasFormat(m_device, outputFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
			return Fail("the output format is not a storage format on this device");
		if (outputFormat != VK_FORMAT_R8G8B8A8_UNORM && outputFormat != VK_FORMAT_R16G16B16A16_SFLOAT &&
			!m_device.shaderStorageImageWriteWithoutFormat)
			return Fail("an output format other than RGBA8_UNORM and RGBA16_SFLOAT needs shaderStorageImageWriteWithoutFormat");

		// The luma picture: the game's picture, no taller than the quality's lines, its width in
		// proportion; padded to whole 64x64 tiles for the single pass downsampler (the padding
		// repeats the picture's edge).
		const uint32_t lines = kFlowLines[static_cast<uint32_t>(quality) < 3 ? static_cast<uint32_t>(quality) : 2];
		m_lumaHeight = (height < lines) ? height : lines;
		m_lumaWidth = static_cast<uint32_t>(std::lround(static_cast<double>(width) * m_lumaHeight / height));
		if (m_lumaWidth < 64)
			m_lumaWidth = 64;
		m_lumaPadWidth = DivideUp(m_lumaWidth, 64) * 64;
		m_lumaPadHeight = DivideUp(m_lumaHeight, 64) * 64;
		// FSR 3 searches 7 levels; fewer when the picture is small, so the coarsest luma level is
		// still 16 pixels across (each level reaches 8 of its pixels around its guess).
		m_levels = 1;
		while (m_levels < kMaxLevels && ((m_lumaWidth < m_lumaHeight ? m_lumaWidth : m_lumaHeight) >> m_levels) >= 16)
			m_levels++;
		m_flowWidth[0] = DivideUp(m_lumaWidth, 8);
		m_flowHeight[0] = DivideUp(m_lumaHeight, 8);
		for (uint32_t level = 1; level < kMaxLevels; level++)
		{
			m_flowWidth[level] = DivideUp(m_flowWidth[level - 1], 2);
			m_flowHeight[level] = DivideUp(m_flowHeight[level - 1], 2);
		}
		m_fieldWidth = m_flowWidth[0];
		m_fieldHeight = m_flowHeight[0];

		const VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		const VkImageUsageFlags debug = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		const VkImageUsageFlags cleared = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		bool ok = true;
		for (uint32_t picture = 0; picture < 2; picture++)
		{
			for (uint32_t level = 0; level < m_levels; level++)
				ok = ok && MakeImage(m_luma[picture][level], m_lumaPadWidth >> level, m_lumaPadHeight >> level, VK_FORMAT_R32_UINT, rw);
			ok = ok && MakeImage(m_flowFinal[picture], m_fieldWidth, m_fieldHeight, VK_FORMAT_R16G16B16A16_SINT, rw | debug);
			ok = ok && MakeImage(m_field[picture], m_fieldWidth, m_fieldHeight, VK_FORMAT_R16G16B16A16_SFLOAT, rw | debug);
			ok = ok && MakeImage(m_histogram[picture], kHistogramBins * kHistogramsPerDim * kHistogramsPerDim, 1, VK_FORMAT_R32_UINT, cleared);
			ok = ok && MakeImage(m_filtered[picture], kHistogramBins * kHistogramsPerDim * kHistogramsPerDim, 1, VK_FORMAT_R32_SFLOAT, cleared);
		}
		for (uint32_t level = 0; level < m_levels; level++)
		{
			ok = ok && MakeImage(m_flowA[level], m_flowWidth[level], m_flowHeight[level], VK_FORMAT_R16G16B16A16_SINT, rw);
			ok = ok && MakeImage(m_flowB[level], m_flowWidth[level], m_flowHeight[level], VK_FORMAT_R16G16B16A16_SINT, rw);
		}
		ok = ok && MakeImage(m_scdTemp, 3, 1, VK_FORMAT_R32_UINT, cleared);
		ok = ok && MakeImage(m_scdOutput, 3, 1, VK_FORMAT_R32_UINT, cleared | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		ok = ok && MakeImage(m_camera, 2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, rw | debug);
		ok = ok && MakeImage(m_verdict, 4, 1, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | debug);
		ok = ok && MakeImage(m_lumaGain, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, rw | debug);
		if (!ok || !AllocateImages())
			return Fail("could not make the images");

		// The scene change detection's results and the verdict come back to the host through a
		// small buffer, a slot of 32 bytes for each record that can be in flight.
		VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bufferInfo.size = 32 * m_device.framesInFlight;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (vkCreateBuffer(m_device.device, &bufferInfo, m_device.allocator, &m_readback) != VK_SUCCESS)
			return Fail("could not make the readback buffer");
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(m_device.device, m_readback, &req);
		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize = req.size;
		void* mapped = nullptr;
		if (!MemoryType(m_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				&alloc.memoryTypeIndex) ||
			vkAllocateMemory(m_device.device, &alloc, m_device.allocator, &m_readbackMemory) != VK_SUCCESS ||
			vkBindBufferMemory(m_device.device, m_readback, m_readbackMemory, 0) != VK_SUCCESS ||
			vkMapMemory(m_device.device, m_readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
			return Fail("could not make the readback buffer");
		m_readbackData = static_cast<const uint32_t*>(mapped);
		m_memoryBytes += req.size;

		if (!MakeLinearSampler(m_device, &m_linear) || !MakePipelines())
			return Fail("could not make the pipelines");
		if (!MakeStaticSets())
			return Fail("could not make the descriptor sets");
		m_framePools.resize(m_device.framesInFlight, VK_NULL_HANDLE);
		for (VkDescriptorPool& pool : m_framePools)
		{
			if (!MakePool(m_device, 5, 16, 6, 5, &pool))
				return Fail("could not make the descriptor pools");
		}
		m_slotRecord.assign(m_device.framesInFlight, ~0ull);
		m_records = 0;
		m_initialised = false;
		return true;
	}

	void OpticalFlowFG::Destroy()
	{
		if (!m_device.device)
			return;
		const VkDevice d = m_device.device;
		const VkAllocationCallbacks* a = m_device.allocator;
		for (VkDescriptorPool pool : m_framePools)
			vkDestroyDescriptorPool(d, pool, a);
		m_framePools.clear();
		vkDestroyDescriptorPool(d, m_staticPool, a);
		m_staticPool = VK_NULL_HANDLE;
		for (uint32_t i = 0; i < kPipelineCount; i++)
		{
			vkDestroyPipeline(d, m_pipelines[i], a);
			vkDestroyPipelineLayout(d, m_pipelineLayouts[i], a);
			vkDestroyDescriptorSetLayout(d, m_setLayouts[i], a);
			m_pipelines[i] = VK_NULL_HANDLE;
			m_pipelineLayouts[i] = VK_NULL_HANDLE;
			m_setLayouts[i] = VK_NULL_HANDLE;
		}
		vkDestroySampler(d, m_linear, a);
		m_linear = VK_NULL_HANDLE;
		for (Image* image : m_images)
		{
			vkDestroyImageView(d, image->view, a);
			vkDestroyImage(d, image->image, a);
			*image = Image{};
		}
		m_images.clear();
		for (VkDeviceMemory memory : m_memory)
			vkFreeMemory(d, memory, a);
		m_memory.clear();
		vkDestroyBuffer(d, m_readback, a);
		vkFreeMemory(d, m_readbackMemory, a); // (unmaps it)
		m_readback = VK_NULL_HANDLE;
		m_readbackMemory = VK_NULL_HANDLE;
		m_readbackData = nullptr;
		m_memoryBytes = 0;
		std::memset(m_pyramidSet, 0, sizeof(m_pyramidSet));
		std::memset(m_histogramSet, 0, sizeof(m_histogramSet));
		std::memset(m_divergenceSet, 0, sizeof(m_divergenceSet));
		std::memset(m_searchSet, 0, sizeof(m_searchSet));
		std::memset(m_filterSet, 0, sizeof(m_filterSet));
		std::memset(m_scaleSet, 0, sizeof(m_scaleSet));
		m_cameraSet = m_verdictSet = m_lumaGainSet = VK_NULL_HANDLE;
		m_slotRecord.clear();
		m_device = Device{};
	}

	void OpticalFlowFG::Dispatch(VkCommandBuffer cmd, Pipeline pipeline, VkDescriptorSet set, const void* constants, uint32_t size, uint32_t x,
		uint32_t y, uint32_t z)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[pipeline]);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayouts[pipeline], 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, m_pipelineLayouts[pipeline], VK_SHADER_STAGE_COMPUTE_BIT, 0, size, constants);
		vkCmdDispatch(cmd, x ? x : 1, y ? y : 1, z ? z : 1);
	}

	void OpticalFlowFG::FlowConstants(void* out, uint32_t level, uint32_t direction) const
	{
		FlowPush push = {};
		push.lumaWidth = static_cast<int32_t>(m_lumaWidth);
		push.lumaHeight = static_cast<int32_t>(m_lumaHeight);
		push.level = level;
		push.levelCount = m_levels;
		push.direction = direction;
		push.transferFunction = 0; // a PS2 picture is SDR
		push.minLuminance = 0.0f;
		push.maxLuminance = 1.0f;
		// The single pass downsampler over the padded luma picture (ffxSpdSetup with 4 mips, as
		// the SDK calls it; the pass makes its 6 levels whatever it is told).
		push.mips = 4;
		push.numWorkGroups = (m_lumaPadWidth / 64) * (m_lumaPadHeight / 64);
		std::memcpy(out, &push, sizeof(push));
	}

	bool OpticalFlowFG::Record(VkCommandBuffer cmd, const FrameGenInput& in, const Timestamps* timestamps)
	{
		if (!m_device.device || !cmd || !in.previous || !in.current || !in.output)
			return false;
		const uint32_t slot = static_cast<uint32_t>(m_records % m_device.framesInFlight);
		const VkDevice d = m_device.device;
		if (vkResetDescriptorPool(d, m_framePools[slot], 0) != VK_SUCCESS)
			return false;
		const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		const VkImageView pictures[2] = {in.previous, in.current};

		// The sets that hold the caller's images: made again for every record.
		VkDescriptorSet prepareSet[2], refineSet[2], compositeSet;
		for (uint32_t picture = 0; picture < 2; picture++)
		{
			prepareSet[picture] = AllocateSet(m_device, m_framePools[slot], m_setLayouts[kPrepareLuma]);
			refineSet[picture] = AllocateSet(m_device, m_framePools[slot], m_setLayouts[kRefine]);
			if (!prepareSet[picture] || !refineSet[picture])
				return false;
			const Bind prepare[3] = {{pictures[picture], R}, {VK_NULL_HANDLE, G}, {m_luma[picture][0].view, G}};
			WriteSet(m_device, prepareSet[picture], kPassSpecs[kPrepareLuma].bindings, 0, prepare, m_linear);
		}
		// Refine 0: the current picture's blocks against the previous picture; 1 the other way.
		for (uint32_t direction = 0; direction < 2; direction++)
		{
			const uint32_t anchor = (direction == 0) ? 1 : 0;
			const Bind refine[6] = {{m_flowFinal[direction].view, G}, {pictures[anchor], R}, {pictures[anchor ^ 1], R}, {VK_NULL_HANDLE, G},
				{m_field[direction].view, G}, {m_camera.view, G}};
			WriteSet(m_device, refineSet[direction], kPassSpecs[kRefine].bindings, 0, refine, m_linear);
		}
		compositeSet = AllocateSet(m_device, m_framePools[slot], m_setLayouts[kComposite]);
		if (!compositeSet)
			return false;
		const Bind composite[8] = {{in.previous, R}, {in.current, R}, {VK_NULL_HANDLE, G}, {m_field[0].view, G}, {m_field[1].view, G},
			{m_verdict.view, G}, {in.output, G}, {m_camera.view, G}};
		WriteSet(m_device, compositeSet, kPassSpecs[kComposite].bindings, 0, composite, m_linear);

		auto stamp = [&](uint32_t index) {
			if (timestamps && timestamps->pool)
				vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps->pool, timestamps->first + index);
		};
		if (timestamps && timestamps->pool)
			vkCmdResetQueryPool(cmd, timestamps->pool, timestamps->first, kTimestampCount);

		// In: the pictures are ready; the effect's images are GENERAL from the first record on.
		std::vector<VkImageMemoryBarrier> layouts;
		if (!m_initialised)
		{
			for (Image* image : m_images)
			{
				VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout = G;
				barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = image->image;
				barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				layouts.push_back(barrier);
			}
			m_initialised = true;
		}
		EntryBarrier(cmd, layouts.data(), static_cast<uint32_t>(layouts.size()));
		stamp(0);

		// The scene change detection's histograms and counters start from nothing; the first
		// run's "previous" histogram is any flat one (its answer is not used).
		const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkClearColorValue zero = {};
		VkClearColorValue flat = {};
		flat.float32[0] = 1.0f / kHistogramBins;
		vkCmdClearColorImage(cmd, m_histogram[0].image, G, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, m_histogram[1].image, G, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, m_scdTemp.image, G, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, m_scdOutput.image, G, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, m_filtered[0].image, G, &flat, 1, &whole);

		FlowPush push;
		// Luma of both pictures (the padding too), then their pyramids and histograms.
		FlowConstants(&push, 0, 0);
		for (uint32_t picture = 0; picture < 2; picture++)
			Dispatch(cmd, kPrepareLuma, prepareSet[picture], &push, sizeof(push), DivideUp(DivideUp(m_lumaPadWidth, 2), 16),
				DivideUp(DivideUp(m_lumaPadHeight, 2), 16), 1);
		ComputeBarrier(cmd);
		for (uint32_t picture = 0; picture < 2; picture++)
		{
			if (m_levels > 1)
				Dispatch(cmd, kLumaPyramid, m_pyramidSet[picture], &push, sizeof(push), m_lumaPadWidth / 64, m_lumaPadHeight / 64, 1);
			const uint32_t strataWidth = (m_lumaWidth / 4) / kHistogramsPerDim;
			Dispatch(cmd, kScdHistogram, m_histogramSet[picture], &push, sizeof(push), DivideUp(strataWidth, 32), 16,
				kHistogramsPerDim * kHistogramsPerDim);
		}
		ComputeBarrier(cmd);
		// The previous picture's histogram against a flat one (to keep its filtered histogram), then
		// the current picture's against that: the second is the answer. With the first, the two
		// pictures' brightness (the pyramids are done).
		FlowConstants(&push, m_gainLevel, 0);
		Dispatch(cmd, kLumaGain, m_lumaGainSet, &push, sizeof(push), 1, 1, 1);
		FlowConstants(&push, 0, 0);
		for (uint32_t picture = 0; picture < 2; picture++)
		{
			Dispatch(cmd, kScdDivergence, m_divergenceSet[picture], &push, sizeof(push), kHistogramsPerDim * kHistogramsPerDim, kHistogramShifts, 1);
			ComputeBarrier(cmd);
		}
		stamp(1);

		// The flow, coarsest level first, each way.
		for (uint32_t direction = 0; direction < 2; direction++)
		{
			for (uint32_t level = m_levels; level-- > 0;)
			{
				FlowConstants(&push, level, direction);
				const uint32_t lumaW = m_lumaWidth >> level, lumaH = m_lumaHeight >> level;
				Dispatch(cmd, kSearch, m_searchSet[direction][level], &push, sizeof(push), DivideUp(DivideUp(lumaW, 4) * 16, 64), DivideUp(lumaH, 16),
					1);
				ComputeBarrier(cmd);
				Dispatch(cmd, kFilter, m_filterSet[direction][level], &push, sizeof(push), DivideUp(m_flowWidth[level], 16),
					DivideUp(m_flowHeight[level], 4), 1);
				ComputeBarrier(cmd);
				if (level > 0)
				{
					Dispatch(cmd, kScale, m_scaleSet[direction][level], &push, sizeof(push), DivideUp(m_flowWidth[level - 1], 4),
						DivideUp(m_flowHeight[level - 1], 4), 1);
					ComputeBarrier(cmd);
				}
			}
			stamp(2 + direction);
		}

		// The camera's move each way, from the flow's vectors.
		const SizePush sizes = {static_cast<int32_t>(m_fieldWidth), static_cast<int32_t>(m_fieldHeight)};
		const CameraPush cameraPush = {sizes.fieldWidth, sizes.fieldHeight, static_cast<int32_t>(m_lumaWidth), static_cast<int32_t>(m_lumaHeight)};
		Dispatch(cmd, kCamera, m_cameraSet, &cameraPush, sizeof(cameraPush), 2, 1, 1);
		ComputeBarrier(cmd);

		// Each block's vector made sure of, and to a fraction of an output pixel.
		const float lumaScaleX = static_cast<float>(m_width) / m_lumaWidth, lumaScaleY = static_cast<float>(m_height) / m_lumaHeight;
		RefinePush refine = {};
		refine.pictureWidth = static_cast<float>(m_width);
		refine.pictureHeight = static_cast<float>(m_height);
		refine.lumaScaleX = lumaScaleX;
		refine.lumaScaleY = lumaScaleY;
		refine.blockPxX = 8.0f * lumaScaleX;
		refine.blockPxY = 8.0f * lumaScaleY;
		refine.fieldWidth = static_cast<int32_t>(m_fieldWidth);
		refine.fieldHeight = static_cast<int32_t>(m_fieldHeight);
		// (A luma pixel is lumaScale output pixels: the flow's answer is that close.)
		refine.radius = static_cast<int32_t>(std::ceil((lumaScaleX > lumaScaleY ? lumaScaleX : lumaScaleY) - 0.01f));
		if (refine.radius < 1)
			refine.radius = 1;
		for (uint32_t direction = 0; direction < 2; direction++)
		{
			refine.direction = static_cast<int32_t>(direction);
			Dispatch(cmd, kRefine, refineSet[direction], &refine, sizeof(refine), m_fieldWidth, m_fieldHeight, 1);
		}
		ComputeBarrier(cmd);
		// A cut or not.
		Dispatch(cmd, kVerdict, m_verdictSet, &sizes, sizeof(sizes), 1, 1, 1);
		ComputeBarrier(cmd);
		stamp(4);

		// The picture at t.
		CompositePush comp = {};
		comp.pictureWidth = static_cast<float>(m_width);
		comp.pictureHeight = static_cast<float>(m_height);
		comp.blockPxX = refine.blockPxX;
		comp.blockPxY = refine.blockPxY;
		comp.fieldWidth = refine.fieldWidth;
		comp.fieldHeight = refine.fieldHeight;
		comp.t = (in.t < 0.0f) ? 0.0f : (in.t > 1.0f) ? 1.0f : in.t;
		comp.oneSidedCost = m_tuning.oneSidedCost;
		comp.flags = (in.showCurrentOnCut ? 1u : 0u) | (m_tuning.debugView ? 2u : 0u);
		comp.neighbours = static_cast<uint32_t>(m_quality);
		comp.tolerance = m_tuning.tolerance;
		comp.giveUp = m_tuning.giveUp;
		comp.otherCost = m_tuning.otherCost;
		Dispatch(cmd, kComposite, compositeSet, &comp, sizeof(comp), DivideUp(m_width, 8), DivideUp(m_height, 8), 1);
		ExitBarrier(cmd);
		stamp(5);

		// The scene change detection's answer and the verdict to the host.
		const VkBufferImageCopy scd = {32ull * slot, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {3, 1, 1}};
		const VkBufferImageCopy verdict = {32ull * slot + 16, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 1, 1}};
		vkCmdCopyImageToBuffer(cmd, m_scdOutput.image, G, m_readback, 1, &scd);
		vkCmdCopyImageToBuffer(cmd, m_verdict.image, G, m_readback, 1, &verdict);
		VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);

		m_slotRecord[slot] = m_records;
		m_records++;
		return true;
	}

	bool OpticalFlowFG::SceneChange(uint64_t record, SceneChangeInfo* info) const
	{
		if (!m_readbackData || m_slotRecord.empty() || !info)
			return false;
		const uint32_t slot = static_cast<uint32_t>(record % m_device.framesInFlight);
		if (m_slotRecord[slot] != record)
			return false;
		const uint32_t* words = m_readbackData + slot * 8;
		std::memcpy(&info->divergence, &words[0], sizeof(float));
		info->detector = (words[1] & 1u) != 0;
		info->cut = words[4] != 0;
		info->matchedShare = static_cast<float>(words[6]) / 1000.0f;
		return true;
	}

	VkImageView OpticalFlowFG::DebugView(uint32_t which) const
	{
		switch (which)
		{
			case 0: return m_flowFinal[0].view;
			case 1: return m_flowFinal[1].view;
			case 2: return m_field[0].view;
			case 3: return m_field[1].view;
			case 4: return m_scdOutput.view;
			case 5: return m_verdict.view;
			case 6: return m_camera.view;
			default: return VK_NULL_HANDLE;
		}
	}

	VkImage OpticalFlowFG::DebugImage(uint32_t which) const
	{
		switch (which)
		{
			case 0: return m_flowFinal[0].image;
			case 1: return m_flowFinal[1].image;
			case 2: return m_field[0].image;
			case 3: return m_field[1].image;
			case 4: return m_scdOutput.image;
			case 5: return m_verdict.image;
			case 6: return m_camera.image;
			default: return VK_NULL_HANDLE;
		}
	}

	std::vector<OpticalFlowFG::PassCount> OpticalFlowFG::Workload() const
	{
		std::vector<PassCount> out;
		if (!m_device.device)
			return out;
		const uint64_t prepare = 2ull * DivideUp(DivideUp(m_lumaPadWidth, 2), 16) * DivideUp(DivideUp(m_lumaPadHeight, 2), 16);
		out.push_back({"OF prepare luma", 2, prepare, 256});
		out.push_back({"OF luma pyramid", m_levels > 1 ? 2u : 0u, m_levels > 1 ? 2ull * (m_lumaPadWidth / 64) * (m_lumaPadHeight / 64) : 0, 256});
		out.push_back({"OF SCD histogram", 2, 2ull * DivideUp((m_lumaWidth / 4) / kHistogramsPerDim, 32) * 16 * 9, 256});
		out.push_back({"OF SCD divergence", 2, 2ull * 9 * kHistogramShifts, 256});
		out.push_back({"OF luma gain", 1, 1, 256});
		uint64_t search = 0, filter = 0, scale = 0;
		for (uint32_t level = 0; level < m_levels; level++)
		{
			search += 2ull * DivideUp(DivideUp(m_lumaWidth >> level, 4) * 16, 64) * DivideUp(m_lumaHeight >> level, 16);
			filter += 2ull * DivideUp(m_flowWidth[level], 16) * DivideUp(m_flowHeight[level], 4);
			if (level > 0)
				scale += 2ull * DivideUp(m_flowWidth[level - 1], 4) * DivideUp(m_flowHeight[level - 1], 4);
		}
		out.push_back({"OF search", 2 * m_levels, search, 64});
		out.push_back({"OF filter", 2 * m_levels, filter, 64});
		out.push_back({"OF scale", 2 * (m_levels - 1), scale, 64});
		out.push_back({"FG camera", 1, 2, 256});
		out.push_back({"FG refine", 2, 2ull * m_fieldWidth * m_fieldHeight, 64});
		out.push_back({"FG verdict", 1, 1, 256});
		out.push_back({"FG composite", 1, 1ull * DivideUp(m_width, 8) * DivideUp(m_height, 8), 64});
		return out;
	}

	// ======================================================================================
	// NIS
	// ======================================================================================

	namespace
	{
		// NVIDIA's NISConfig up to its reserved values: what nis_main.comp's push constants hold.
		constexpr uint32_t kNisPushBytes = 28 * 4;
		static_assert(offsetof(NISConfig, reserved1) + sizeof(float) == kNisPushBytes);
		constexpr uint32_t kNisTableBytes = kPhaseCount * kFilterSize * sizeof(float); // 64 phases of 8 taps
	} // namespace

	NIS::NIS() = default;

	NIS::~NIS()
	{
		Destroy();
	}

	bool NIS::Fail(const char* error)
	{
		m_error = error;
		Destroy();
		return false;
	}

	bool NIS::Create(const Device& device, VkFormat outputFormat)
	{
		Destroy();
		m_error = "";
		if (!device.device || !device.physicalDevice)
		{
			m_error = "no device";
			return false;
		}
		m_device = device;
		if (m_device.framesInFlight == 0)
			m_device.framesInFlight = 1;
		m_outputFormat = outputFormat;
		if (!HasFormat(m_device, outputFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
			return Fail("the output format is not a storage format on this device");
		const bool rgba8 = (outputFormat == VK_FORMAT_R8G8B8A8_UNORM), rgba16f = (outputFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
		if (!rgba8 && !rgba16f && !m_device.shaderStorageImageWriteWithoutFormat)
			return Fail("an output format other than RGBA8_UNORM and RGBA16_SFLOAT needs shaderStorageImageWriteWithoutFormat");
		if (!HasFormat(m_device, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
			return Fail("the device cannot sample RGBA32F");
		m_half = m_device.shaderFloat16;
		const Spirv scaler = m_half ? (rgba8 ? CRUX2_SPIRV(NisScalerHalfRgba8) : rgba16f ? CRUX2_SPIRV(NisScalerHalfRgba16f) : CRUX2_SPIRV(NisScalerHalfUnformatted)) :
									  (rgba8 ? CRUX2_SPIRV(NisScalerRgba8) : rgba16f ? CRUX2_SPIRV(NisScalerRgba16f) : CRUX2_SPIRV(NisScalerUnformatted));
		const Spirv sharpener = rgba8 ? CRUX2_SPIRV(NisSharpenRgba8) : rgba16f ? CRUX2_SPIRV(NisSharpenRgba16f) : CRUX2_SPIRV(NisSharpenUnformatted);

		// Bindings 1..5 as NVIDIA's NIS_Main.glsl has them: sampler, input, output, the two tables.
		VkPipelineLayout sharpenLayout = VK_NULL_HANDLE;
		if (!MakeLinearSampler(m_device, &m_linear) || !MakeSetLayout(m_device, "XSISS", 1, &m_setLayout) ||
			!MakePipeline(m_device, m_setLayout, kNisPushBytes, scaler, &m_pipelineLayout, &m_scaler) ||
			!MakePipeline(m_device, m_setLayout, kNisPushBytes, sharpener, &sharpenLayout, &m_sharpener))
		{
			if (sharpenLayout)
				vkDestroyPipelineLayout(m_device.device, sharpenLayout, m_device.allocator);
			return Fail("could not make the pipelines");
		}
		// (Both pipelines have the same layout; one is kept.)
		vkDestroyPipelineLayout(m_device.device, sharpenLayout, m_device.allocator);

		// The two tables (2x64 RGBA32F) and the buffer they are filled through, in one allocation.
		VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		imageInfo.extent = {kFilterSize / 4, kPhaseCount, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bufferInfo.size = 2 * kNisTableBytes;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (vkCreateImage(m_device.device, &imageInfo, m_device.allocator, &m_coefficients[0]) != VK_SUCCESS ||
			vkCreateImage(m_device.device, &imageInfo, m_device.allocator, &m_coefficients[1]) != VK_SUCCESS ||
			vkCreateBuffer(m_device.device, &bufferInfo, m_device.allocator, &m_upload) != VK_SUCCESS)
			return Fail("could not make the coefficient tables");
		VkMemoryRequirements reqs[3];
		vkGetImageMemoryRequirements(m_device.device, m_coefficients[0], &reqs[0]);
		vkGetImageMemoryRequirements(m_device.device, m_coefficients[1], &reqs[1]);
		vkGetBufferMemoryRequirements(m_device.device, m_upload, &reqs[2]);
		VkDeviceSize offsets[3], size = 0;
		uint32_t bits = ~0u;
		for (uint32_t i = 0; i < 3; i++)
		{
			size = (size + reqs[i].alignment - 1) / reqs[i].alignment * reqs[i].alignment;
			offsets[i] = size;
			size += reqs[i].size;
			bits &= reqs[i].memoryTypeBits;
		}
		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize = size;
		if ((!MemoryType(m_device, bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex) &&
				!MemoryType(m_device, bits, 0, &alloc.memoryTypeIndex)) ||
			vkAllocateMemory(m_device.device, &alloc, m_device.allocator, &m_memory) != VK_SUCCESS ||
			vkBindImageMemory(m_device.device, m_coefficients[0], m_memory, offsets[0]) != VK_SUCCESS ||
			vkBindImageMemory(m_device.device, m_coefficients[1], m_memory, offsets[1]) != VK_SUCCESS ||
			vkBindBufferMemory(m_device.device, m_upload, m_memory, offsets[2]) != VK_SUCCESS)
			return Fail("could not allocate the coefficient tables");
		for (uint32_t i = 0; i < 2; i++)
		{
			VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = m_coefficients[i];
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = VK_FORMAT_R32G32B32A32_SFLOAT;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (vkCreateImageView(m_device.device, &view, m_device.allocator, &m_coefficientViews[i]) != VK_SUCCESS)
				return Fail("could not make the coefficient tables");
		}
		m_framePools.resize(m_device.framesInFlight, VK_NULL_HANDLE);
		for (VkDescriptorPool& pool : m_framePools)
		{
			if (!MakePool(m_device, 1, 4, 1, 1, &pool))
				return Fail("could not make the descriptor pools");
		}
		m_uploaded = false;
		m_records = 0;
		return true;
	}

	void NIS::Destroy()
	{
		if (!m_device.device)
			return;
		const VkDevice d = m_device.device;
		const VkAllocationCallbacks* a = m_device.allocator;
		for (VkDescriptorPool pool : m_framePools)
			vkDestroyDescriptorPool(d, pool, a);
		m_framePools.clear();
		vkDestroyPipeline(d, m_scaler, a);
		vkDestroyPipeline(d, m_sharpener, a);
		vkDestroyPipelineLayout(d, m_pipelineLayout, a);
		vkDestroyDescriptorSetLayout(d, m_setLayout, a);
		vkDestroySampler(d, m_linear, a);
		for (uint32_t i = 0; i < 2; i++)
		{
			vkDestroyImageView(d, m_coefficientViews[i], a);
			vkDestroyImage(d, m_coefficients[i], a);
			m_coefficientViews[i] = VK_NULL_HANDLE;
			m_coefficients[i] = VK_NULL_HANDLE;
		}
		vkDestroyBuffer(d, m_upload, a);
		vkFreeMemory(d, m_memory, a);
		m_scaler = m_sharpener = VK_NULL_HANDLE;
		m_pipelineLayout = VK_NULL_HANDLE;
		m_setLayout = VK_NULL_HANDLE;
		m_linear = VK_NULL_HANDLE;
		m_upload = VK_NULL_HANDLE;
		m_memory = VK_NULL_HANDLE;
		m_device = Device{};
	}

	bool NIS::Record(VkCommandBuffer cmd, const NISInput& in, const Timestamps* timestamps)
	{
		if (!m_device.device || !cmd || !in.input || !in.output)
			return false;
		const bool sharpenOnly = (in.inputWidth == in.outputWidth && in.inputHeight == in.outputHeight);
		NISConfig config = {};
		const bool ok = sharpenOnly ?
							NVSharpenUpdateConfig(config, in.sharpness, 0, 0, in.inputWidth, in.inputHeight, in.inputWidth, in.inputHeight, 0, 0) :
							NVScalerUpdateConfig(config, in.sharpness, 0, 0, in.inputWidth, in.inputHeight, in.inputWidth, in.inputHeight, 0, 0,
								in.outputWidth, in.outputHeight, in.outputWidth, in.outputHeight);
		if (!ok)
		{
			m_error = "NIS scales by 1x to 2x on each axis";
			return false;
		}
		const uint32_t slot = static_cast<uint32_t>(m_records % m_device.framesInFlight);
		if (vkResetDescriptorPool(m_device.device, m_framePools[slot], 0) != VK_SUCCESS)
			return false;
		const VkDescriptorSet set = AllocateSet(m_device, m_framePools[slot], m_setLayout);
		if (!set)
			return false;
		const Bind binds[5] = {{VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL}, {in.input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{in.output, VK_IMAGE_LAYOUT_GENERAL}, {m_coefficientViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{m_coefficientViews[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
		WriteSet(m_device, set, "XSISS", 1, binds, m_linear);

		if (timestamps && timestamps->pool)
		{
			vkCmdResetQueryPool(cmd, timestamps->pool, timestamps->first, 2);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps->pool, timestamps->first);
		}
		if (!m_uploaded)
		{
			// NVIDIA's tables into the two textures, once: through the buffer, so nothing on the host
			// has to be mapped.
			vkCmdUpdateBuffer(cmd, m_upload, 0, kNisTableBytes, coef_scale);
			vkCmdUpdateBuffer(cmd, m_upload, kNisTableBytes, kNisTableBytes, coef_usm);
			VkBufferMemoryBarrier filled = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
			filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			filled.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			filled.srcQueueFamilyIndex = filled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			filled.buffer = m_upload;
			filled.size = VK_WHOLE_SIZE;
			VkImageMemoryBarrier toCopy[2];
			for (uint32_t i = 0; i < 2; i++)
			{
				toCopy[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				toCopy[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				toCopy[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				toCopy[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toCopy[i].srcQueueFamilyIndex = toCopy[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				toCopy[i].image = m_coefficients[i];
				toCopy[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			}
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &filled, 2, toCopy);
			for (uint32_t i = 0; i < 2; i++)
			{
				const VkBufferImageCopy copy = {i * kNisTableBytes, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
					{kFilterSize / 4, kPhaseCount, 1}};
				vkCmdCopyBufferToImage(cmd, m_upload, m_coefficients[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			}
			VkImageMemoryBarrier toRead[2];
			for (uint32_t i = 0; i < 2; i++)
			{
				toRead[i] = toCopy[i];
				toRead[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				toRead[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			}
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);
			m_uploaded = true;
		}
		EntryBarrier(cmd, nullptr, 0);
		const VkPipeline pipeline = sharpenOnly ? m_sharpener : m_scaler;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, kNisPushBytes, &config);
		// A workgroup a block of the output: 32x24 when scaling, 32x32 when sharpening (nis_main.comp).
		vkCmdDispatch(cmd, DivideUp(in.outputWidth, 32), DivideUp(in.outputHeight, sharpenOnly ? 32 : 24), 1);
		ExitBarrier(cmd);
		if (timestamps && timestamps->pool)
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps->pool, timestamps->first + 1);
		m_records++;
		return true;
	}
} // namespace PS5ComputeFX

// Crux2 - a libretro preset run on Vulkan from PS5Slang's ChainPlan: the test's runner, and the shape for GSDeviceVK's.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Test-only (ps5/tests/slang/slang_vk.cpp runs it on lavapipe), written to be copied: RunFrame is
// the whole per-frame logic and follows ChainPlan step by step. What the title's runner does
// differently is only where things come from: the game's picture is a GSTexture, the command
// buffer GSDeviceVK's, the shaders compiled by VKShaderCache::CompileShaderToSPV, the uniform
// ring and descriptor sets PCSX2's own (frames in flight instead of a wait each frame).
//
// Per frame (RunFrame):
//   1. plan.Build(preset, frame): sizes, the bytes of every uniform block and push constant block,
//      what each sampler reads and how.
//   2. Pictures: each pass's output at its planned size, format and mip levels (and, for a pass
//      whose picture of the frame before is read, a second one); made anew and cleared to
//      transparent black when a size changes. The history ring (OriginalHistory1..N) at the
//      game's picture's size. A copy of the game's picture with mipmaps when pass 0 asks.
//   3. For each pass: its uniform bytes into this frame's slot of the uniform buffer, a
//      descriptor set (the uniform block, then one combined image sampler per TextureBind:
//      the picture its TextureRef names, a sampler of its filter / mipmap / wrap), the render
//      pass on its output, the push constant bytes, a 4-vertex strip; then mipmaps when the
//      next pass asks.
//   4. After the passes: the game's picture into the history ring (it becomes OriginalHistory1),
//      and each feedback pass's two pictures swapped (this frame's output is the next frame's
//      PassFeedback).
// Every picture is left in SHADER_READ_ONLY_OPTIMAL between uses. Each render pass's two
// external dependencies order it after whatever wrote or read its pictures before, and before
// whatever reads its output after, so passes need no barriers between them.
//
// The Vulkan functions are libvulkan's; the shader compiler is the caller's.

#pragma once

#include "ps5_slang.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ChainRunner
{
struct Device
{
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory{};
	VkDeviceSize uniformAlign = 256;
};

inline uint32_t MemoryType(const Device& d, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < d.memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

struct Image
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;   // every level: what a sampler reads
	VkImageView level0 = VK_NULL_HANDLE; // the first level: what a render pass draws
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0, levels = 1;
	VkFormat format = VK_FORMAT_UNDEFINED;
};

inline void Destroy(const Device& d, Image& i)
{
	if (i.framebuffer)
		vkDestroyFramebuffer(d.device, i.framebuffer, nullptr);
	if (i.level0 && i.level0 != i.view)
		vkDestroyImageView(d.device, i.level0, nullptr);
	if (i.view)
		vkDestroyImageView(d.device, i.view, nullptr);
	if (i.image)
		vkDestroyImage(d.device, i.image, nullptr);
	if (i.memory)
		vkFreeMemory(d.device, i.memory, nullptr);
	i = Image();
}

inline void Barrier(VkCommandBuffer cmd, VkImage image, uint32_t level, uint32_t levels, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = from;
	b.newLayout = to;
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1};
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// The stage of a pass's shader, compiled: SPIR-V words, empty on failure.
using Compile = std::function<std::vector<uint32_t>(const std::string& glsl, bool vertex, size_t pass)>;

class Runner
{
public:
	Runner(const Device& device, PS5Slang::Preset& preset) : d(device), preset(preset) {}
	~Runner() { Release(); }

	// Pipelines, layouts, samplers, the lookup pictures (decoded RGBA8 in `lutPixels`, the sizes
	// already in preset.luts[i].width / height). `fetchCounter`: a storage buffer every
	// fragment stage counts its fetches into (set 1, binding 0; the test's cost measurement).
	bool Load(VkCommandBuffer cmd, const Compile& compile, const std::vector<std::vector<uint8_t>>& lutPixels,
		VkBuffer fetchCounter = VK_NULL_HANDLE);

	// One frame: `input` is the game's picture (its view, its size, in SHADER_READ_ONLY_OPTIMAL,
	// with TRANSFER_SRC usage when the preset keeps history or wants it mipmapped).
	bool RunFrame(VkCommandBuffer cmd, VkImage inputImage, VkImageView inputView, VkFormat inputFormat, uint32_t width,
		uint32_t height, uint32_t viewportWidth, uint32_t viewportHeight, uint64_t frameCount, VkQueryPool timestamps = VK_NULL_HANDLE);

	// The last pass's picture of the frame just run, and every pass's (for the test's checks).
	const Image& Output() const { return finalImage; }
	const Image& PassOutput(size_t p) const { return passes[p].feedbackSwapped ? passes[p].previous : passes[p].output; }
	const PS5Slang::ChainPlan& Plan() const { return plan; }
	std::string error;

private:
	struct PassObjects
	{
		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		Image output, previous; // previous: the picture of the frame before, for PassFeedback
		bool feedbackSwapped = false;
	};

	bool Fail(const std::string& why)
	{
		error = why;
		return false;
	}
	VkRenderPass RenderPassFor(VkFormat format);
	VkSampler SamplerFor(bool linear, bool mipmap, PS5Slang::Wrap wrap);
	bool Ensure(VkCommandBuffer cmd, Image& image, uint32_t w, uint32_t h, uint32_t levels, VkFormat format, bool attachment);
	void MakeMipmaps(VkCommandBuffer cmd, Image& image);
	VkImageView ViewOf(const PS5Slang::TextureRef& ref, VkImageView original) const;
	void Release();

	const Device& d;
	PS5Slang::Preset& preset;
	PS5Slang::ChainPlan plan;
	std::vector<PassObjects> passes;
	std::vector<Image> luts, history;
	Image original; // the game's picture with mipmaps, when pass 0 asks
	Image finalImage;
	std::map<VkFormat, VkRenderPass> renderPasses;
	std::map<uint32_t, VkSampler> samplers;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkBuffer uniforms = VK_NULL_HANDLE, vertices = VK_NULL_HANDLE, staging = VK_NULL_HANDLE;
	VkDeviceMemory uniformMemory = VK_NULL_HANDLE, vertexMemory = VK_NULL_HANDLE, stagingMemory = VK_NULL_HANDLE;
	uint8_t* uniformMap = nullptr;
	VkDeviceSize uniformStride = 0;
	VkBuffer counter = VK_NULL_HANDLE;
	VkDescriptorSetLayout countLayout = VK_NULL_HANDLE;
};

// ------------------------------------------------------------------------------------------------

inline bool MakeBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, void** map)
{
	VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = size;
	info.usage = usage;
	if (vkCreateBuffer(d.device, &info, nullptr, &buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(d.device, buffer, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = MemoryType(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d.device, &alloc, nullptr, &memory) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(d.device, buffer, memory, 0);
	return !map || vkMapMemory(d.device, memory, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
}

inline VkRenderPass Runner::RenderPassFor(VkFormat format)
{
	const auto it = renderPasses.find(format);
	if (it != renderPasses.end())
		return it->second;
	VkAttachmentDescription attachment{};
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the quad covers it all
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	VkSubpassDependency around[2] = {};
	// Before: earlier passes' writes (and copies, mipmaps) visible to this pass's reads; earlier
	// reads of this picture (last frame's) done before it is drawn over.
	around[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	around[0].dstSubpass = 0;
	around[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
							 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	around[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	around[0].dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
							 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	// After: this pass's picture visible to later passes' reads, mipmap blits and copies.
	around[1].srcSubpass = 0;
	around[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	around[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	around[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	around[1].dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	around[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	info.attachmentCount = 1;
	info.pAttachments = &attachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = around;
	VkRenderPass rp = VK_NULL_HANDLE;
	if (vkCreateRenderPass(d.device, &info, nullptr, &rp) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	renderPasses[format] = rp;
	return rp;
}

// RetroArch's samplers: the filter both ways, mipmaps blended only when filtered and present,
// every level allowed, transparent black beyond the border.
inline VkSampler Runner::SamplerFor(bool linear, bool mipmap, PS5Slang::Wrap wrap)
{
	const uint32_t key = (linear ? 1u : 0u) | (mipmap ? 2u : 0u) | (static_cast<uint32_t>(wrap) << 2);
	const auto it = samplers.find(key);
	if (it != samplers.end())
		return it->second;
	static const VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
	VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	info.magFilter = info.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.mipmapMode = linear && mipmap ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = info.addressModeV = info.addressModeW = modes[static_cast<int>(wrap)];
	info.maxLod = VK_LOD_CLAMP_NONE;
	info.maxAnisotropy = 1.f;
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	VkSampler s = VK_NULL_HANDLE;
	vkCreateSampler(d.device, &info, nullptr, &s);
	samplers[key] = s;
	return s;
}

// The picture at that size, format and levels; made anew (and cleared to transparent black, left
// readable) when anything differs.
inline bool Runner::Ensure(VkCommandBuffer cmd, Image& image, uint32_t w, uint32_t h, uint32_t levels, VkFormat format, bool attachment)
{
	if (image.image && image.width == w && image.height == h && image.levels == levels && image.format == format)
		return true;
	Destroy(d, image);
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = {w, h, 1};
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				 (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
	if (vkCreateImage(d.device, &info, nullptr, &image.image) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(d.device, image.image, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = MemoryType(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(d.device, &alloc, nullptr, &image.memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(d.device, image.image, image.memory, 0);
	VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view.image = image.image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
	if (vkCreateImageView(d.device, &view, nullptr, &image.view) != VK_SUCCESS)
		return false;
	image.level0 = image.view;
	if (levels > 1)
	{
		view.subresourceRange.levelCount = 1;
		if (vkCreateImageView(d.device, &view, nullptr, &image.level0) != VK_SUCCESS)
			return false;
	}
	if (attachment)
	{
		VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fb.renderPass = RenderPassFor(format);
		fb.attachmentCount = 1;
		fb.pAttachments = &image.level0;
		fb.width = w;
		fb.height = h;
		fb.layers = 1;
		if (!fb.renderPass || vkCreateFramebuffer(d.device, &fb, nullptr, &image.framebuffer) != VK_SUCCESS)
			return false;
	}
	image.width = w;
	image.height = h;
	image.levels = levels;
	image.format = format;
	Barrier(cmd, image.image, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	const VkClearColorValue black{};
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
	vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	Barrier(cmd, image.image, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
	return true;
}

// Levels 1.. from level 0, each half the one above (linear blits); all left readable.
inline void Runner::MakeMipmaps(VkCommandBuffer cmd, Image& image)
{
	if (image.levels <= 1)
		return;
	Barrier(cmd, image.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	Barrier(cmd, image.image, 1, image.levels - 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	for (uint32_t level = 1; level < image.levels; level++)
	{
		VkImageBlit blit{};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
		blit.srcOffsets[1] = {static_cast<int32_t>(std::max(1u, image.width >> (level - 1))),
			static_cast<int32_t>(std::max(1u, image.height >> (level - 1))), 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		blit.dstOffsets[1] = {static_cast<int32_t>(std::max(1u, image.width >> level)), static_cast<int32_t>(std::max(1u, image.height >> level)), 1};
		vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		Barrier(cmd, image.image, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	}
	Barrier(cmd, image.image, 0, image.levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT);
}

inline VkImageView Runner::ViewOf(const PS5Slang::TextureRef& ref, VkImageView originalView) const
{
	switch (ref.kind)
	{
		case PS5Slang::TextureRef::History: return ref.index == 0 ? originalView : history[ref.index - 1].view;
		case PS5Slang::TextureRef::PassOutput: return passes[ref.index].output.view;
		case PS5Slang::TextureRef::PassFeedback: return passes[ref.index].previous.view;
		case PS5Slang::TextureRef::Lut: return luts[ref.index].view;
	}
	return VK_NULL_HANDLE;
}

inline bool Runner::Load(VkCommandBuffer cmd, const Compile& compile, const std::vector<std::vector<uint8_t>>& lutPixels, VkBuffer fetchCounter)
{
	counter = fetchCounter;
	const size_t count = preset.passes.size();
	passes.resize(count);
	// One uniform slot a pass; the test waits for each frame, the title rings these per frame in flight.
	uniformStride = 0;
	for (const PS5Slang::Pass& p : preset.passes)
		uniformStride = std::max<VkDeviceSize>(uniformStride, p.uboSize);
	uniformStride = std::max<VkDeviceSize>((uniformStride + d.uniformAlign - 1) / d.uniformAlign * d.uniformAlign, d.uniformAlign);
	void* map = nullptr;
	if (!MakeBuffer(d, uniformStride * count, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniforms, uniformMemory, &map))
		return Fail("no uniform buffer");
	uniformMap = static_cast<uint8_t*>(map);
	if (!MakeBuffer(d, sizeof(PS5Slang::QUAD_VERTICES), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, vertexMemory, &map))
		return Fail("no vertex buffer");
	std::memcpy(map, PS5Slang::QUAD_VERTICES, sizeof(PS5Slang::QUAD_VERTICES));

	// The lookup pictures, uploaded once (with mipmaps when the preset asks).
	size_t total = 0;
	for (const auto& px : lutPixels)
		total += px.size();
	if (total > 0)
	{
		if (!MakeBuffer(d, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingMemory, &map))
			return Fail("no staging buffer");
		luts.resize(preset.luts.size());
		size_t offset = 0;
		for (size_t i = 0; i < preset.luts.size(); i++)
		{
			const PS5Slang::Lut& lut = preset.luts[i];
			uint32_t levels = 1;
			if (lut.mipmap)
				for (uint32_t s = std::max(lut.width, lut.height); s > 1; s >>= 1)
					levels++;
			if (!Ensure(cmd, luts[i], lut.width, lut.height, levels, VK_FORMAT_R8G8B8A8_UNORM, false))
				return Fail("no lookup picture");
			std::memcpy(static_cast<uint8_t*>(map) + offset, lutPixels[i].data(), lutPixels[i].size());
			Barrier(cmd, luts[i].image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			VkBufferImageCopy copy{};
			copy.bufferOffset = offset;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageExtent = {lut.width, lut.height, 1};
			vkCmdCopyBufferToImage(cmd, staging, luts[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			Barrier(cmd, luts[i].image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			MakeMipmaps(cmd, luts[i]);
			offset += lutPixels[i].size();
		}
	}

	uint32_t uboCount = 0, imageCount = 0;
	for (size_t p = 0; p < count; p++)
	{
		const PS5Slang::Pass& pass = preset.passes[p];
		PassObjects& po = passes[p];
		uboCount += pass.uboBinding >= 0;
		imageCount += static_cast<uint32_t>(pass.samplers.size());
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		if (pass.uboBinding >= 0)
			bindings.push_back({static_cast<uint32_t>(pass.uboBinding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr});
		for (const PS5Slang::Sampler& s : pass.samplers)
			bindings.push_back({s.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, both, nullptr});
		VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dsl.bindingCount = static_cast<uint32_t>(bindings.size());
		dsl.pBindings = bindings.data();
		if (vkCreateDescriptorSetLayout(d.device, &dsl, nullptr, &po.setLayout) != VK_SUCCESS)
			return Fail("no descriptor set layout");
		if (counter && !countLayout)
		{
			const VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo c{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			c.bindingCount = 1;
			c.pBindings = &b;
			vkCreateDescriptorSetLayout(d.device, &c, nullptr, &countLayout);
		}
		VkDescriptorSetLayout layouts[2] = {po.setLayout, countLayout};
		const VkPushConstantRange range{both, 0, pass.pushSize};
		VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pl.setLayoutCount = counter ? 2 : 1;
		pl.pSetLayouts = layouts;
		pl.pushConstantRangeCount = pass.pushSize > 0;
		pl.pPushConstantRanges = &range;
		if (vkCreatePipelineLayout(d.device, &pl, nullptr, &po.layout) != VK_SUCCESS)
			return Fail("no pipeline layout");

		VkShaderModule modules[2] = {};
		for (int s = 0; s < 2; s++)
		{
			const std::vector<uint32_t> spirv = compile(s == 0 ? pass.vertex : pass.fragment, s == 0, p);
			if (spirv.empty())
				return Fail("pass " + std::to_string(p) + " does not compile");
			VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			mi.codeSize = spirv.size() * 4;
			mi.pCode = spirv.data();
			vkCreateShaderModule(d.device, &mi, nullptr, &modules[s]);
		}
		po.renderPass = RenderPassFor(static_cast<VkFormat>(pass.vkFormat));
		VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = modules[0];
		stages[0].pName = "main";
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = modules[1];
		stages[1].pName = "main";
		const VkVertexInputBindingDescription binding{0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
		const VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)}};
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vi.vertexBindingDescriptionCount = 1;
		vi.pVertexBindingDescriptions = &binding;
		vi.vertexAttributeDescriptionCount = 2;
		vi.pVertexAttributeDescriptions = attributes;
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1.f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &blendAttachment;
		const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dy.dynamicStateCount = 2;
		dy.pDynamicStates = dynamic;
		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.stageCount = 2;
		gp.pStages = stages;
		gp.pVertexInputState = &vi;
		gp.pInputAssemblyState = &ia;
		gp.pViewportState = &vp;
		gp.pRasterizationState = &rs;
		gp.pMultisampleState = &ms;
		gp.pColorBlendState = &cb;
		gp.pDynamicState = &dy;
		gp.layout = po.layout;
		gp.renderPass = po.renderPass;
		const VkResult r = vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &gp, nullptr, &po.pipeline);
		vkDestroyShaderModule(d.device, modules[0], nullptr);
		vkDestroyShaderModule(d.device, modules[1], nullptr);
		if (r != VK_SUCCESS)
			return Fail("pass " + std::to_string(p) + ": no pipeline");
	}
	std::vector<VkDescriptorPoolSize> sizes = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, std::max(uboCount, 1u)},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(imageCount, 1u)}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(count)}};
	VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.maxSets = static_cast<uint32_t>(count) * 2;
	pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
	pi.pPoolSizes = sizes.data();
	if (vkCreateDescriptorPool(d.device, &pi, nullptr, &pool) != VK_SUCCESS)
		return Fail("no descriptor pool");
	return true;
}

inline bool Runner::RunFrame(VkCommandBuffer cmd, VkImage inputImage, VkImageView inputView, VkFormat inputFormat, uint32_t width,
	uint32_t height, uint32_t viewportWidth, uint32_t viewportHeight, uint64_t frameCount, VkQueryPool timestamps)
{
	// 1. The plan.
	PS5Slang::FrameInfo frame;
	frame.sourceWidth = width;
	frame.sourceHeight = height;
	frame.viewportWidth = viewportWidth;
	frame.viewportHeight = viewportHeight;
	frame.frameCount = frameCount;
	for (const Image& h : history)
	{
		frame.historyWidths.push_back(h.width);
		frame.historyHeights.push_back(h.height);
	}
	if (!plan.Build(preset, frame, &error))
		return false;
	const size_t count = plan.passes.size();

	// 2. The pictures.
	for (size_t p = 0; p < count; p++)
	{
		const PS5Slang::PassPlan& pp = plan.passes[p];
		const VkFormat format = static_cast<VkFormat>(pp.vkFormat);
		if (!Ensure(cmd, passes[p].output, pp.width, pp.height, pp.mipLevels, format, true) ||
			(preset.passes[p].feedback && !Ensure(cmd, passes[p].previous, pp.width, pp.height, pp.mipLevels, format, true)))
			return Fail("no picture for pass " + std::to_string(p));
		passes[p].feedbackSwapped = false;
	}
	history.resize(plan.historyFrames);
	for (Image& h : history)
		if (!Ensure(cmd, h, width, height, 1, inputFormat, false))
			return Fail("no history picture");
	VkImageView originalView = inputView;
	if (plan.originalMipLevels > 1)
	{
		if (!Ensure(cmd, original, width, height, plan.originalMipLevels, inputFormat, false))
			return Fail("no mipmapped copy of the input");
		Barrier(cmd, inputImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_READ_BIT);
		Barrier(cmd, original.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkImageCopy copy{};
		copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.extent = {width, height, 1};
		vkCmdCopyImage(cmd, inputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		Barrier(cmd, inputImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		Barrier(cmd, original.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		MakeMipmaps(cmd, original);
		originalView = original.view;
	}

	// 3. The passes.
	vkResetDescriptorPool(d.device, pool, 0);
	for (size_t p = 0; p < count; p++)
	{
		const PS5Slang::Pass& pass = preset.passes[p];
		const PS5Slang::PassPlan& pp = plan.passes[p];
		PassObjects& po = passes[p];
		std::memcpy(uniformMap + p * uniformStride, pp.ubo.data(), pp.ubo.size());

		VkDescriptorSet set = VK_NULL_HANDLE;
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = pool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &po.setLayout;
		if (vkAllocateDescriptorSets(d.device, &ai, &set) != VK_SUCCESS)
			return Fail("no descriptor set");
		std::vector<VkWriteDescriptorSet> writes;
		std::vector<VkDescriptorImageInfo> images(pp.textures.size());
		const VkDescriptorBufferInfo ubo{uniforms, p * uniformStride, std::max<VkDeviceSize>(pass.uboSize, 16)};
		if (pass.uboBinding >= 0)
		{
			VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w.dstSet = set;
			w.dstBinding = static_cast<uint32_t>(pass.uboBinding);
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			w.pBufferInfo = &ubo;
			writes.push_back(w);
		}
		for (size_t t = 0; t < pp.textures.size(); t++)
		{
			const PS5Slang::TextureBind& b = pp.textures[t];
			images[t] = {SamplerFor(b.linear, b.mipmap, b.wrap), ViewOf(b.texture, originalView), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w.dstSet = set;
			w.dstBinding = b.binding;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			w.pImageInfo = &images[t];
			writes.push_back(w);
		}
		vkUpdateDescriptorSets(d.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		VkDescriptorSet countSet = VK_NULL_HANDLE;
		if (counter)
		{
			// The test's fetch counter: set 1.
			ai.pSetLayouts = &countLayout;
			vkAllocateDescriptorSets(d.device, &ai, &countSet);
			const VkDescriptorBufferInfo bi{counter, 0, VK_WHOLE_SIZE};
			VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w.dstSet = countSet;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			w.pBufferInfo = &bi;
			vkUpdateDescriptorSets(d.device, 1, &w, 0, nullptr);
		}

		if (timestamps)
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, static_cast<uint32_t>(2 * p));
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = po.renderPass;
		begin.framebuffer = po.output.framebuffer;
		begin.renderArea = {{0, 0}, {pp.width, pp.height}};
		vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, po.pipeline);
		const VkViewport viewport{0.f, 0.f, static_cast<float>(pp.width), static_cast<float>(pp.height), 0.f, 1.f};
		const VkRect2D scissor{{0, 0}, {pp.width, pp.height}};
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		const VkDescriptorSet sets[2] = {set, countSet};
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, po.layout, 0, counter ? 2 : 1, sets, 0, nullptr);
		if (pass.pushSize > 0)
			vkCmdPushConstants(cmd, po.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pass.pushSize, pp.push.data());
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &zero);
		vkCmdDraw(cmd, 4, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		MakeMipmaps(cmd, po.output);
		if (timestamps)
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, static_cast<uint32_t>(2 * p + 1));
	}
	finalImage = passes.back().output;

	// 4. What the next frame reads: the game's picture into the history ring, the feedback swapped.
	if (!history.empty())
	{
		std::rotate(history.rbegin(), history.rbegin() + 1, history.rend()); // the oldest becomes the newest's place
		Image& newest = history[0];
		Barrier(cmd, inputImage, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		Barrier(cmd, newest.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkImageCopy copy{};
		copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.extent = {width, height, 1};
		vkCmdCopyImage(cmd, inputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, newest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		Barrier(cmd, inputImage, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		Barrier(cmd, newest.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	for (size_t p = 0; p < count; p++)
		if (preset.passes[p].feedback)
		{
			std::swap(passes[p].output, passes[p].previous);
			passes[p].feedbackSwapped = true;
		}
	return true;
}

inline void Runner::Release()
{
	if (!d.device)
		return;
	vkDeviceWaitIdle(d.device);
	for (PassObjects& po : passes)
	{
		Destroy(d, po.output);
		Destroy(d, po.previous);
		if (po.pipeline)
			vkDestroyPipeline(d.device, po.pipeline, nullptr);
		if (po.layout)
			vkDestroyPipelineLayout(d.device, po.layout, nullptr);
		if (po.setLayout)
			vkDestroyDescriptorSetLayout(d.device, po.setLayout, nullptr);
	}
	passes.clear();
	for (Image& i : luts)
		Destroy(d, i);
	for (Image& i : history)
		Destroy(d, i);
	Destroy(d, original);
	for (auto& [f, rp] : renderPasses)
		vkDestroyRenderPass(d.device, rp, nullptr);
	for (auto& [k, s] : samplers)
		vkDestroySampler(d.device, s, nullptr);
	if (pool)
		vkDestroyDescriptorPool(d.device, pool, nullptr);
	if (countLayout)
		vkDestroyDescriptorSetLayout(d.device, countLayout, nullptr);
	countLayout = VK_NULL_HANDLE;
	for (auto [b, m] : {std::pair{uniforms, uniformMemory}, std::pair{vertices, vertexMemory}, std::pair{staging, stagingMemory}})
	{
		if (b)
			vkDestroyBuffer(d.device, b, nullptr);
		if (m)
			vkFreeMemory(d.device, m, nullptr);
	}
	uniforms = vertices = staging = VK_NULL_HANDLE;
	renderPasses.clear();
	samplers.clear();
	pool = VK_NULL_HANDLE;
}
} // namespace ChainRunner

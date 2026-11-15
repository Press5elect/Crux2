// Crux2 - downsampling and SMAA (ps5_aa.glsl) on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// On a software Vulkan driver (Mesa's lavapipe), the passes recorded as GSDeviceVK_PS5.cpp's DoDownsample
// and DoSMAA record them: the same push constants (ps5_aa.h's Pass), the same formats (the first downsample
// pass into 16-bit float, the rest RGBA8), the same samplers (linear; SMAA's search texture point), SMAA's
// lookup textures expanded to RGBA8 as the device does, its edges' picture cleared first.
//  - Downsampling, Soft and Sharp, at ratios 1.33, 2, 2.67 and 5, from a picture in part of its texture
//    (a border of another colour round it, which must not bleed in):
//      a flat colour comes out exactly; the mean of a random picture is kept (DC gain 1);
//      every pixel is the double-precision reference's within a level of 255;
//      a step's overshoot and undershoot stay within 5% (Soft: none);
//      on a zone plate, the aliasing (what is left where the rings are finer than a screen pixel can
//      hold) is less than one bilinear tap's (the screen's filter, what Bilinear does).
//  - SMAA: a flat picture and one with only faint gradients come out unchanged; on staircase edges
//    (a nearly horizontal and a nearly vertical line, aliased) the flat parts are untouched, the
//    steps blended, and the picture nearer the line's true coverage than the aliased one.
//   aa_test <folder of the compiled shaders>
#include <vulkan/vulkan.h>

#include "ps5_aa.h"
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define VKC(x) \
	do \
	{ \
		VkResult r_ = (x); \
		if (r_ != VK_SUCCESS) \
		{ \
			std::fprintf(stderr, "%s: %d\n", #x, (int)r_); \
			std::exit(2); \
		} \
	} while (0)

static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkDescriptorSetLayout setLayout;
static VkPipelineLayout layout;
static VkDescriptorPool pool;
static VkSampler linear, point;
static int failures = 0;

static void check(bool ok, const std::string& what)
{
	if (!ok)
	{
		std::printf("  FAIL: %s\n", what.c_str());
		failures++;
	}
}

static uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties p;
	vkGetPhysicalDeviceMemoryProperties(gpu, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; i++)
	{
		if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
			return i;
	}
	std::fprintf(stderr, "no memory type\n");
	std::exit(2);
}

struct Image
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t w = 0, h = 0;
};

static Image makeImage(uint32_t w, uint32_t h, VkFormat format)
{
	Image im;
	im.format = format;
	im.w = w;
	im.h = h;
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = format;
	ci.extent = {w, h, 1};
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VKC(vkCreateImage(device, &ci, nullptr, &im.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, im.image, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKC(vkAllocateMemory(device, &ai, nullptr, &im.memory));
	VKC(vkBindImageMemory(device, im.image, im.memory, 0));
	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = im.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VKC(vkCreateImageView(device, &vi, nullptr, &im.view));
	return im;
}

static void freeImage(Image& im)
{
	vkDestroyImageView(device, im.view, nullptr);
	vkDestroyImage(device, im.image, nullptr);
	vkFreeMemory(device, im.memory, nullptr);
	im = {};
}

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
};

static Buffer makeBuffer(VkDeviceSize size)
{
	Buffer b;
	VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = size;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VKC(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, b.buffer, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKC(vkAllocateMemory(device, &ai, nullptr, &b.memory));
	VKC(vkBindBufferMemory(device, b.buffer, b.memory, 0));
	VKC(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
	return b;
}

static void freeBuffer(Buffer& b)
{
	vkUnmapMemory(device, b.memory);
	vkDestroyBuffer(device, b.buffer, nullptr);
	vkFreeMemory(device, b.memory, nullptr);
	b = {};
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void begin()
{
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VKC(vkResetCommandBuffer(cmd, 0));
	VKC(vkBeginCommandBuffer(cmd, &bi));
}

static void submit()
{
	VKC(vkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	VKC(vkQueueWaitIdle(queue));
}

// RGBA8 pixels into an RGBA8 image, left in the shader read layout.
static Image upload(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
	Image im = makeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM);
	Buffer b = makeBuffer(rgba.size());
	std::memcpy(b.mapped, rgba.data(), rgba.size());
	begin();
	barrier(im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {w, h, 1};
	vkCmdCopyBufferToImage(cmd, b.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	barrier(im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit();
	freeBuffer(b);
	return im;
}

// An RGBA8 image (in the shader read layout) read back.
static std::vector<uint8_t> download(const Image& im)
{
	Buffer b = makeBuffer(static_cast<VkDeviceSize>(im.w) * im.h * 4);
	begin();
	barrier(im.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {im.w, im.h, 1};
	vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &region);
	barrier(im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit();
	std::vector<uint8_t> out(static_cast<const uint8_t*>(b.mapped), static_cast<const uint8_t*>(b.mapped) + im.w * im.h * 4);
	freeBuffer(b);
	return out;
}

static VkShaderModule shader(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (code.empty())
	{
		std::fprintf(stderr, "no %s\n", path.c_str());
		std::exit(2);
	}
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = code.size();
	ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule m;
	VKC(vkCreateShaderModule(device, &ci, nullptr, &m));
	return m;
}

static VkPipeline makePipeline(VkShaderModule vs, VkShaderModule fs, VkFormat format)
{
	VkPipelineShaderStageCreateInfo stages[2] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
	};
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &format;
	VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gci.pNext = &rci;
	gci.stageCount = 2;
	gci.pStages = stages;
	gci.pVertexInputState = &vin;
	gci.pInputAssemblyState = &ia;
	gci.pViewportState = &vp;
	gci.pRasterizationState = &rs;
	gci.pMultisampleState = &ms;
	gci.pColorBlendState = &cb;
	gci.pDynamicState = &ds;
	gci.layout = layout;
	VkPipeline p;
	VKC(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gci, nullptr, &p));
	return p;
}

struct Input
{
	const Image* image;
	VkSampler sampler;
};

// One pass into `target` (cleared to 0 first, as the edges' picture is), from up to four inputs at bindings 0-3.
static void run(VkPipeline pipeline, Image& target, std::initializer_list<Input> inputs, const void* push, uint32_t push_size)
{
	VkDescriptorSet set;
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &setLayout;
	VKC(vkAllocateDescriptorSets(device, &dai, &set));
	std::vector<VkDescriptorImageInfo> infos;
	for (const Input& in : inputs)
		infos.push_back({in.sampler, in.image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
	while (infos.size() < 4) // (the bindings a pass does not read get its first input: a set must be whole)
		infos.push_back(infos.front());
	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = set;
	write.descriptorCount = 4;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = infos.data();
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

	begin();
	barrier(target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	att.imageView = target.view;
	att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {target.w, target.h}};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &att;
	vkCmdBeginRendering(cmd, &ri);
	VkViewport viewport{0, 0, float(target.w), float(target.h), 0, 1};
	VkRect2D scissor{{0, 0}, {target.w, target.h}};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	barrier(target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit();
	VKC(vkFreeDescriptorSets(device, pool, 1, &set));
}

static VkPipeline aa[5];

// ---- downsampling ---------------------------------------------------------------------------------------------------

struct Picture
{
	uint32_t w = 0, h = 0;      // the texture
	int x = 0, y = 0, pw = 0, ph = 0; // the part the picture is in
	std::vector<uint8_t> rgba;
};

// A texture with a magenta border round the picture, which no pass may read.
static Picture framed(int pw, int ph, int x, int y, auto&& pixel)
{
	Picture p;
	p.w = static_cast<uint32_t>(pw + x + 6);
	p.h = static_cast<uint32_t>(ph + y + 5);
	p.x = x;
	p.y = y;
	p.pw = pw;
	p.ph = ph;
	p.rgba.assign(p.w * p.h * 4, 0);
	for (uint32_t j = 0; j < p.h; j++)
	{
		for (uint32_t i = 0; i < p.w; i++)
		{
			uint8_t* o = &p.rgba[(j * p.w + i) * 4];
			const int px = static_cast<int>(i) - x, py = static_cast<int>(j) - y;
			if (px < 0 || py < 0 || px >= pw || py >= ph)
			{
				o[0] = 255, o[1] = 0, o[2] = 255, o[3] = 255;
				continue;
			}
			uint8_t c[4];
			pixel(px, py, c);
			std::memcpy(o, c, 4);
		}
	}
	return p;
}

static std::vector<uint8_t> downsample(const Picture& p, PS5AA::Downsample mode, int dw, int dh)
{
	Image src = upload(p.rgba, p.w, p.h);
	Image mid = makeImage(static_cast<uint32_t>(dw), static_cast<uint32_t>(p.ph), VK_FORMAT_R16G16B16A16_SFLOAT);
	Image out = makeImage(static_cast<uint32_t>(dw), static_cast<uint32_t>(dh), VK_FORMAT_R8G8B8A8_UNORM);
	const PS5AA::PassConstants across = PS5AA::Pass(mode, p.x, p.pw, dw, p.y, p.ph);
	run(aa[0], mid, {{&src, linear}}, &across, sizeof(across));
	const PS5AA::PassConstants down = PS5AA::Pass(mode, 0, p.ph, dh, 0, dw);
	run(aa[1], out, {{&mid, linear}}, &down, sizeof(down));
	std::vector<uint8_t> pixels = download(out);
	freeImage(src);
	freeImage(mid);
	freeImage(out);
	return pixels;
}

// ps5_aa.glsl's passes in double precision, from the picture's 8-bit values: c R, G, B, A in 0..255.
static std::vector<double> reference(const Picture& p, PS5AA::Downsample mode, int dw, int dh)
{
	const auto filter = [&](int n, int out, auto&& at, auto&& put) {
		const double ratio = static_cast<double>(n) / out;
		const double k = PS5AA::KernelRatio(static_cast<float>(ratio));
		const double reach = PS5AA::Reach(mode, static_cast<float>(ratio));
		for (int j = 0; j < out; j++)
		{
			const double centre = (j + 0.5) * ratio - 0.5;
			double sum[4] = {}, total = 0;
			for (int i = static_cast<int>(std::ceil(centre - reach)); i - centre <= reach; i++)
			{
				const double d = i - centre;
				const double w = mode == PS5AA::DOWNSAMPLE_SHARP ? PS5AA::LanczosWeight(d, k) : PS5AA::TentWeight(d, k);
				const double* v = at(std::clamp(i, 0, n - 1));
				for (int c = 0; c < 4; c++)
					sum[c] += w * v[c];
				total += w;
			}
			for (int c = 0; c < 4; c++)
				sum[c] /= total;
			put(j, sum);
		}
	};
	std::vector<double> in(static_cast<size_t>(p.pw) * p.ph * 4), mid(static_cast<size_t>(dw) * p.ph * 4),
		out(static_cast<size_t>(dw) * dh * 4);
	for (int y = 0; y < p.ph; y++)
		for (int x = 0; x < p.pw; x++)
			for (int c = 0; c < 4; c++)
				in[(y * p.pw + x) * 4 + c] = p.rgba[((p.y + y) * p.w + p.x + x) * 4 + c] / 255.0;
	for (int y = 0; y < p.ph; y++)
		filter(p.pw, dw, [&](int i) { return &in[(y * p.pw + i) * 4]; },
			[&](int j, const double* v) { std::copy(v, v + 4, &mid[(y * dw + j) * 4]); });
	for (int x = 0; x < dw; x++)
		filter(p.ph, dh, [&](int i) { return &mid[(i * dw + x) * 4]; },
			[&](int j, const double* v) {
				for (int c = 0; c < 4; c++)
					out[(j * dw + x) * 4 + c] = std::clamp(v[c], 0.0, 1.0) * 255.0;
			});
	return out;
}

// One bilinear tap at each screen pixel's centre (the screen's filter, Bilinear), the red channel, 0..255.
static std::vector<double> bilinear(const Picture& p, int dw, int dh)
{
	std::vector<double> out(static_cast<size_t>(dw) * dh);
	const auto at = [&](int x, int y) {
		x = std::clamp(x, 0, p.pw - 1);
		y = std::clamp(y, 0, p.ph - 1);
		return static_cast<double>(p.rgba[((p.y + y) * p.w + p.x + x) * 4]);
	};
	for (int j = 0; j < dh; j++)
	{
		for (int i = 0; i < dw; i++)
		{
			const double u = (i + 0.5) * p.pw / dw - 0.5, v = (j + 0.5) * p.ph / dh - 0.5;
			const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
			const double fx = u - x0, fy = v - y0;
			out[j * dw + i] = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
		}
	}
	return out;
}

static void testDownsample()
{
	const int dw = 96, dh = 72;
	const struct
	{
		const char* name;
		int pw, ph;
	} ratios[] = {{"1.33", 128, 96}, {"2", 192, 144}, {"2.67", 256, 192}, {"5", 480, 360}};
	for (int mode = PS5AA::DOWNSAMPLE_SOFT; mode <= PS5AA::DOWNSAMPLE_SHARP; mode++)
	{
		const PS5AA::Downsample m = static_cast<PS5AA::Downsample>(mode);
		const std::string kind = PS5AA::DOWNSAMPLE_CHOICES[mode];
		for (const auto& r : ratios)
		{
			const std::string at = kind + " at " + r.name + " (" + std::to_string(r.pw) + "x" + std::to_string(r.ph) + " to 96x72)";
			// A flat colour: exactly, and nothing of the border.
			{
				const Picture p = framed(r.pw, r.ph, 5, 3, [](int, int, uint8_t* c) { c[0] = 37, c[1] = 200, c[2] = 129, c[3] = 255; });
				const std::vector<uint8_t> out = downsample(p, m, dw, dh);
				bool exact = true;
				for (size_t i = 0; i < out.size(); i += 4)
					exact = exact && out[i] == 37 && out[i + 1] == 200 && out[i + 2] == 129 && out[i + 3] == 255;
				check(exact, at + ": a flat colour comes out exactly, the border not read");
			}
			// A random picture: the reference within a level, its mean kept.
			double worst = 0, mean_in = 0, mean_out = 0;
			{
				uint32_t seed = 12345u + static_cast<uint32_t>(r.pw);
				const Picture p = framed(r.pw, r.ph, 5, 3, [&](int, int, uint8_t* c) {
					for (int k = 0; k < 3; k++)
					{
						seed = seed * 1664525u + 1013904223u;
						c[k] = static_cast<uint8_t>(seed >> 24);
					}
					c[3] = 255;
				});
				const std::vector<uint8_t> out = downsample(p, m, dw, dh);
				const std::vector<double> ref = reference(p, m, dw, dh);
				for (size_t i = 0; i < out.size(); i++)
				{
					worst = std::max(worst, std::fabs(out[i] - ref[i]));
					if (i % 4 == 0)
						mean_out += out[i];
				}
				for (int y = 0; y < p.ph; y++)
					for (int x = 0; x < p.pw; x++)
						mean_in += p.rgba[((p.y + y) * p.w + p.x + x) * 4];
				mean_in /= static_cast<double>(p.pw) * p.ph;
				mean_out /= static_cast<double>(dw) * dh;
				check(worst <= 1.0, at + ": the double-precision reference within a level (" + std::to_string(worst) + ")");
				check(std::fabs(mean_in - mean_out) <= 1.0, at + ": the mean kept, DC gain 1 (" + std::to_string(mean_in) + " to " +
														   std::to_string(mean_out) + ")");
			}
			// Steps, across and down, between 64 and 191 (inside 0..1: the clamp would hide an overshoot).
			double over = 0, under = 0;
			for (int axis = 0; axis < 2; axis++)
			{
				const Picture p = framed(r.pw, r.ph, 5, 3, [&](int x, int y, uint8_t* c) {
					const bool hi = axis == 0 ? (x >= r.pw * 37 / 100) : (y >= r.ph * 41 / 100);
					c[0] = c[1] = c[2] = hi ? 191 : 64;
					c[3] = 255;
				});
				const std::vector<uint8_t> out = downsample(p, m, dw, dh);
				for (size_t i = 0; i < out.size(); i += 4)
				{
					over = std::max(over, (out[i] - 191.0) / 127.0);
					under = std::max(under, (64.0 - out[i]) / 127.0);
				}
			}
			const double limit = m == PS5AA::DOWNSAMPLE_SHARP ? 0.05 : 0.0;
			check(over <= limit && under <= limit,
				at + ": a step's overshoot " + std::to_string(over * 100) + "% and undershoot " + std::to_string(under * 100) + "%");
			// A zone plate: rings from the top left, as fine as the picture can hold at its far corner.
			double alias = 0, alias_bilinear = 0;
			{
				const double rmax = std::hypot(r.pw, r.ph);
				const Picture p = framed(r.pw, r.ph, 5, 3, [&](int x, int y, uint8_t* c) {
					const double rr = (x + 0.5) * (x + 0.5) + (y + 0.5) * (y + 0.5);
					const double v = 0.5 + 0.5 * std::cos(3.14159265358979 * 0.5 * rr / rmax);
					c[0] = c[1] = c[2] = static_cast<uint8_t>(std::lround(v * 255.0));
					c[3] = 255;
				});
				const std::vector<uint8_t> out = downsample(p, m, dw, dh);
				const std::vector<double> bil = bilinear(p, dw, dh);
				const double ratio = static_cast<double>(r.pw) / dw;
				const double nyquist = 0.5 / ratio; // a screen pixel's, in cycles a texel
				int n = 0;
				for (int j = 0; j < dh; j++)
				{
					for (int i = 0; i < dw; i++)
					{
						const double freq = 0.5 * std::hypot((i + 0.5) * ratio, (j + 0.5) * ratio) / rmax; // the rings', there
						if (freq <= 1.1 * nyquist || freq >= 0.45)
							continue;
						alias += (out[(j * dw + i) * 4] - 127.5) * (out[(j * dw + i) * 4] - 127.5);
						alias_bilinear += (bil[j * dw + i] - 127.5) * (bil[j * dw + i] - 127.5);
						n++;
					}
				}
				alias = std::sqrt(alias / std::max(n, 1));
				alias_bilinear = std::sqrt(alias_bilinear / std::max(n, 1));
				// (Strictly less; from 2 up, under 0.6 of a bilinear tap's (measured: Sharp 0.53 at 2, both kernels far less
				// above). Sharp at 1.33 is only a little under it: the rings finer than a screen pixel are just past its
				// Nyquist there, in Lanczos-2's transition band.)
				check(n > 0 && alias < alias_bilinear && (ratio < 1.9 || alias < 0.6 * alias_bilinear),
					at + ": less aliasing than one bilinear tap (" + std::to_string(alias) + " to its " + std::to_string(alias_bilinear) + ")");
			}
			std::printf("  %-34s worst %.2f levels, mean %.2f to %.2f, overshoot %.1f%%, undershoot %.1f%%, aliasing %.1f (bilinear %.1f)\n",
				at.c_str(), worst, mean_in, mean_out, over * 100, under * 100, alias, alias_bilinear);
		}
	}
}

// ---- SMAA -------------------------------------------------------------------------------------------------------------

static Image area, search;

static std::vector<uint8_t> smaa(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, std::vector<uint8_t>* edges_out)
{
	Image src = upload(rgba, w, h);
	Image edges = makeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM);
	Image weights = makeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM);
	Image out = makeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM);
	const float metrics[4] = {1.0f / w, 1.0f / h, float(w), float(h)};
	run(aa[2], edges, {{&src, linear}}, metrics, sizeof(metrics));
	run(aa[3], weights, {{&edges, linear}, {&area, linear}, {&search, point}}, metrics, sizeof(metrics));
	run(aa[4], out, {{&src, linear}, {&weights, linear}}, metrics, sizeof(metrics));
	if (edges_out)
		*edges_out = download(edges);
	std::vector<uint8_t> pixels = download(out);
	for (Image* im : {&src, &edges, &weights, &out})
		freeImage(*im);
	return pixels;
}

static void testSmaa()
{
	const uint32_t w = 128, h = 96;
	// A flat picture, and one of gradients too faint to be edges: unchanged.
	for (int kind = 0; kind < 2; kind++)
	{
		std::vector<uint8_t> in(w * h * 4);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
			{
				uint8_t* p = &in[(y * w + x) * 4];
				p[0] = static_cast<uint8_t>(kind ? 60 + x / 2 : 128);
				p[1] = static_cast<uint8_t>(kind ? 90 + y / 3 : 128);
				p[2] = 128;
				p[3] = 255;
			}
		std::vector<uint8_t> edges;
		const std::vector<uint8_t> out = smaa(in, w, h, &edges);
		check(out == in, kind ? "SMAA: faint gradients come out unchanged" : "SMAA: a flat picture comes out unchanged");
		check(std::all_of(edges.begin(), edges.end(), [](uint8_t v) { return v == 0; }) || kind == 1,
			"SMAA: no edges found in a flat picture");
	}

	// Staircases: a line nearly across (rising 1 in 8) and one nearly down (1 in 6), white past it, black before; aliased
	// (a pixel is white when its centre is past the line). Compared with the line's true coverage of each pixel.
	for (int vertical = 0; vertical < 2; vertical++)
	{
		const double slope = vertical ? 1.0 / 6.0 : 1.0 / 8.0;
		const double at0 = vertical ? 40.0 : 30.0;
		// (u along the line's run, v across it: x, y for the line across; y, x for the one down)
		const auto line = [&](double u) { return at0 + u * slope; };
		const auto coverage = [&](int u, int v) {
			double c = 0;
			for (int s = 0; s < 64; s++)
				c += std::clamp(v + 1 - line(u + (s + 0.5) / 64.0), 0.0, 1.0);
			return c / 64.0;
		};
		std::vector<uint8_t> in(w * h * 4);
		std::vector<double> truth(w * h);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
			{
				const int u = vertical ? static_cast<int>(y) : static_cast<int>(x), v = vertical ? static_cast<int>(x) : static_cast<int>(y);
				const bool white = v + 0.5 >= line(u + 0.5);
				uint8_t* p = &in[(y * w + x) * 4];
				p[0] = p[1] = p[2] = white ? 255 : 0;
				p[3] = 255;
				truth[y * w + x] = coverage(u, v) * 255.0;
			}
		const std::vector<uint8_t> out = smaa(in, w, h, nullptr);
		int changed = 0, far_changed = 0, blended = 0;
		double error_in = 0, error_out = 0;
		for (uint32_t y = 4; y < h - 4; y++) // (away from the picture's edges, where the line's ends are)
			for (uint32_t x = 4; x < w - 4; x++)
			{
				const int u = vertical ? static_cast<int>(y) : static_cast<int>(x), v = vertical ? static_cast<int>(x) : static_cast<int>(y);
				const size_t i = (y * w + x) * 4;
				const bool near = std::fabs(v + 0.5 - line(u + 0.5)) < 2.5;
				if (out[i] != in[i])
				{
					changed++;
					far_changed += near ? 0 : 1;
					blended += (out[i] > 0 && out[i] < 255) ? 1 : 0;
				}
				error_in += std::fabs(in[i] - truth[y * w + x]);
				error_out += std::fabs(out[i] - truth[y * w + x]);
			}
		const std::string what = vertical ? "SMAA on a line nearly down" : "SMAA on a line nearly across";
		check(far_changed == 0, what + ": the flat parts untouched (" + std::to_string(far_changed) + " pixels changed away from the line)");
		check(changed >= 40 && blended == changed, what + ": the steps blended (" + std::to_string(changed) + " pixels)");
		check(error_out < 0.7 * error_in, what + ": nearer the line's true coverage (" + std::to_string(error_out) + " from " +
											  std::to_string(error_in) + ")");
		std::printf("  %-34s %d pixels blended, none away from it; off the true coverage %.0f from %.0f\n", what.c_str(), changed,
			error_out, error_in);
	}
}

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : ".";
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	const char* validation = "VK_LAYER_KHRONOS_validation";
	uint32_t layerCount = 0;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
	std::vector<VkLayerProperties> layers(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
	bool validate = false;
	for (const auto& l : layers)
		validate = validate || std::strcmp(l.layerName, validation) == 0;
	if (validate)
	{
		ici.enabledLayerCount = 1;
		ici.ppEnabledLayerNames = &validation;
	}
	std::printf("downsampling and SMAA on the PC: the validation layer %s\n", validate ? "on (its messages, if any, above the results)" : "not installed");
	VkInstance instance;
	VKC(vkCreateInstance(&ici, nullptr, &instance));
	uint32_t count = 1;
	VkResult res = vkEnumeratePhysicalDevices(instance, &count, &gpu);
	if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || count == 0)
	{
		std::printf("downsampling and SMAA: NOT RUN (no Vulkan driver)\n");
		return 0;
	}
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = 0;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.dynamicRendering = VK_TRUE;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &f13;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VKC(vkCreateDevice(gpu, &dci, nullptr, &device));
	vkGetDeviceQueue(device, 0, 0, &queue);
	VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool cmdPool;
	VKC(vkCreateCommandPool(device, &cpi, nullptr, &cmdPool));
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = cmdPool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VKC(vkAllocateCommandBuffers(device, &cai, &cmd));

	// As PCSX2's utility layout: four pictures, and push constants for both stages (96 bytes).
	VkDescriptorSetLayoutBinding bindings[4];
	for (uint32_t i = 0; i < 4; i++)
		bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dl.bindingCount = 4;
	dl.pBindings = bindings;
	VKC(vkCreateDescriptorSetLayout(device, &dl, nullptr, &setLayout));
	VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
	VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &setLayout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &range;
	VKC(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
	VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
	VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	dp.maxSets = 16;
	dp.poolSizeCount = 1;
	dp.pPoolSizes = &ps;
	VKC(vkCreateDescriptorPool(device, &dp, nullptr, &pool));
	for (VkSampler* s : {&linear, &point})
	{
		VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sci.magFilter = sci.minFilter = (s == &linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.maxLod = 0.0f;
		VKC(vkCreateSampler(device, &sci, nullptr, s));
	}

	const VkShaderModule vs = shader(dir + "/screen.vert.spv");
	for (int pass = 0; pass < 5; pass++)
		aa[pass] = makePipeline(vs, shader(dir + "/aa" + std::to_string(pass) + ".frag.spv"),
			pass == 0 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);

	// SMAA's lookup textures, as GSDeviceVK_PS5.cpp's PS5PrepareAA makes them.
	{
		std::vector<uint8_t> rgba(AREATEX_WIDTH * AREATEX_HEIGHT * 4);
		for (size_t i = 0; i < AREATEX_WIDTH * AREATEX_HEIGHT; i++)
		{
			rgba[i * 4] = areaTexBytes[i * 2];
			rgba[i * 4 + 1] = areaTexBytes[i * 2 + 1];
			rgba[i * 4 + 2] = 0;
			rgba[i * 4 + 3] = 255;
		}
		area = upload(rgba, AREATEX_WIDTH, AREATEX_HEIGHT);
		rgba.assign(SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT * 4, 0);
		for (size_t i = 0; i < SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT; i++)
		{
			rgba[i * 4] = searchTexBytes[i];
			rgba[i * 4 + 3] = 255;
		}
		search = upload(rgba, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
	}

	testDownsample();
	testSmaa();
	std::printf("downsampling and SMAA (ps5_aa.glsl): %s\n", failures ? "FAILED" : "PASS");
	return failures ? 1 : 0;
}

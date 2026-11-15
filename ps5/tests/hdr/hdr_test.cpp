// Crux2 - the last passes before the screen (ps5_final.glsl: HDR10 output and debanding) on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// After OGBox's ps5/tests/hdr-ogbox (Press5elect). On a software Vulkan driver (Mesa's lavapipe), as
// GSDeviceVK_PS5.cpp's PS5FinalEnd records the passes:
//  - HDR: two layers of the swapchain's format (B8G8R8A8: the picture, and the menus over clear 0),
//    FINAL_PASS 1 into an A2R10G10B10 image, copied into an image of the framebuffers' format and read
//    back. Each pixel's three 10-bit codes, taken where VideoOut's HDR10 type reads them (B in bits 0-9,
//    G 10-19, R 20-29), must be the shader's maths done here in double precision, within a code: the
//    gamma 2.2 decoding, BT.709 to BT.2020, the white level, the highlights' curve, the menus' blend, PQ.
//    The test screen's patterns are checked code by code at their landmarks.
//  - Debanding: FINAL_PASS 0 over gradients of 8-bit and of 16-bit colour's steps, and over hard edges:
//    the steps must come out smaller, the edges kept.
//  - ps5_hdr.h: the settings' choices and the test screen's steps.
//   hdr_test <folder of the compiled shaders>
#include <vulkan/vulkan.h>

#include "ps5_hdr.h"

#include <algorithm>
#include <cmath>
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

static const uint32_t W = 400, H = 300; // rows 0-5 the test pixels; the rest black (the test screen's patterns are in the middle)

static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;

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
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
};

static Image makeImage(VkFormat format, VkImageUsageFlags usage)
{
	Image im{};
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = format;
	ci.extent = {W, H, 1};
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	VKC(vkCreateImage(device, &ci, nullptr, &im.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, im.image, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKC(vkAllocateMemory(device, &ai, nullptr, &im.memory));
	VKC(vkBindImageMemory(device, im.image, im.memory, 0));
	if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
	{
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = im.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VKC(vkCreateImageView(device, &vi, nullptr, &im.view));
	}
	return im;
}

struct Buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	void* mapped;
};

static Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
	Buffer b{};
	VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = size;
	ci.usage = usage;
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

static void barrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkImageLayout from,
	VkImageLayout to, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
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

// ---- ps5_final.glsl's HDR maths, in double precision ----------------------------------------------------------
static double pqCode(double nits)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
	return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2) * 1023.0;
}

static double nitsOfCode(double code)
{
	const double e = std::pow(code / 1023.0, 1.0 / 78.84375);
	return 10000.0 * std::pow(std::max(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e), 1.0 / 0.1593017578125);
}

struct Push // cb_final
{
	float white, peak, linear, highlights;
	float cal, level, unused0, unused1;
	float step, dither, reach, deband;
};

// The codes R, G, B for a picture pixel p and a menu pixel u (B, G, R, A bytes as stored; u premultiplied).
static void expect(const uint8_t* p, const uint8_t* u, const Push& k, double out[3])
{
	const double pic[3] = {p[2] / 255.0, p[1] / 255.0, p[0] / 255.0};
	const double ui[3] = {u[2] / 255.0, u[1] / 255.0, u[0] / 255.0}, ua = u[3] / 255.0;
	const double white = std::max<double>(k.white, 1.0);
	double gain = 1.0;
	if (k.highlights > 0.5 && k.peak > white)
	{
		const double l = 0.2126 * std::pow(pic[0], 2.2) + 0.7152 * std::pow(pic[1], 2.2) + 0.0722 * std::pow(pic[2], 2.2);
		if (l > 0.5)
		{
			const double t = (std::min(l, 1.0) - 0.5) / 0.5;
			gain = (l + (k.peak / white - 1.0) * t * t) / l;
		}
	}
	double light[3];
	for (int i = 0; i < 3; i++)
		light[i] = std::pow(ui[i] + pic[i] * (1.0 - ua), 2.2) * (gain + (1.0 - gain) * ua);
	const double m[3][3] = {{0.6274, 0.3293, 0.0433}, {0.0691, 0.9195, 0.0114}, {0.0164, 0.0880, 0.8956}};
	for (int i = 0; i < 3; i++)
	{
		const double v = m[i][0] * light[0] + m[i][1] * light[1] + m[i][2] * light[2];
		out[i] = pqCode(std::max(v, 0.0) * white);
	}
}

// One pass of ps5_final.glsl, recorded as PS5FinalEnd does, its target read back as 32-bit words.
struct Pass
{
	VkPipeline pipeline;
	VkFormat format;
	Image target;
};

static VkPipelineLayout layout;
static VkDescriptorSet set;
static VkCommandBuffer cmd;
static Image picture, menus, frame;
static Buffer upPic, upUi, down;

static Pass makePass(VkShaderModule vs, VkShaderModule fs, VkFormat format)
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
	Pass p{};
	p.format = format;
	VKC(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gci, nullptr, &p.pipeline));
	p.target = makeImage(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	return p;
}

// Uploads the layers, runs the pass, copies its target into `frame` (B8G8R8A8, as the framebuffers) and reads it.
static const uint32_t* run(const Pass& pass, const Push& k, const std::vector<uint8_t>& pic, const std::vector<uint8_t>& ui)
{
	std::memcpy(upPic.mapped, pic.data(), pic.size());
	std::memcpy(upUi.mapped, ui.data(), ui.size());
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VKC(vkResetCommandBuffer(cmd, 0));
	VKC(vkBeginCommandBuffer(cmd, &bi));
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {W, H, 1};
	for (const auto& up : {std::make_pair(upPic.buffer, picture.image), std::make_pair(upUi.buffer, menus.image)})
	{
		barrier(cmd, up.second, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyBufferToImage(cmd, up.first, up.second, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		barrier(cmd, up.second, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	barrier(cmd, pass.target.image, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	att.imageView = pass.target.view;
	att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {W, H}};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &att;
	vkCmdBeginRendering(cmd, &ri);
	VkViewport viewport{0, 0, float(W), float(H), 0, 1};
	VkRect2D scissor{{0, 0}, {W, H}};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &k);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	barrier(cmd, pass.target.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	barrier(cmd, frame.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkImageCopy copy{};
	copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.extent = {W, H, 1};
	vkCmdCopyImage(cmd, pass.target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barrier(cmd, frame.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buffer, 1, &region);
	VKC(vkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	VKC(vkQueueWaitIdle(queue));
	return static_cast<const uint32_t*>(down.mapped);
}

static int checkChoices()
{
	int bad = 0;
	const auto check = [&](bool ok, const char* what) {
		if (!ok)
		{
			std::printf("  FAIL: %s\n", what);
			bad++;
		}
	};
	const std::vector<int> peak = PS5Hdr::PeakChoices(), white = PS5Hdr::WhiteChoices();
	check(peak.front() == 0 && peak[1] == 400 && peak.back() == 4000, "the highlights' choices: Off, 400 .. 4000");
	check(std::find(peak.begin(), peak.end(), PS5Hdr::DEFAULT_PEAK) != peak.end(), "1000 nits among the highlights' choices");
	check(std::is_sorted(peak.begin(), peak.end()) && std::is_sorted(white.begin(), white.end()), "the choices in order");
	check(white.front() == 80 && white.back() == 400 && std::count(white.begin(), white.end(), 203) == 1 &&
			  std::count(white.begin(), white.end(), 200) == 1 && std::count(white.begin(), white.end(), 210) == 1,
		"the white level's choices: 80 .. 400 in 10s, with 203 once");
	check(PS5Hdr::Step(peak, 1000, 1) == 1250 && PS5Hdr::Step(peak, 1000, -1) == 900, "the highlights stepped from 1000");
	check(PS5Hdr::Step(peak, 400, -1) == 400, "the test screen does not step the highlights to Off");
	check(PS5Hdr::Step(peak, 4000, 1) == 4000, "nor past 4000");
	check(PS5Hdr::Step(peak, 1100, 1) == 1250 && PS5Hdr::Step(peak, 0, 0) == 400, "a value not among them from the nearest");
	check(PS5Hdr::Step(white, 200, 1) == 203 && PS5Hdr::Step(white, 203, 1) == 210, "the white level stepped through 203");
	check(PS5Hdr::WhiteOf(10) == 80 && PS5Hdr::WhiteOf(9000) == 400 && PS5Hdr::PeakOf(-5) == 0, "the settings' values clamped");
	const PS5Hdr::Deband low = PS5Hdr::DebandOf(1, 2160), high = PS5Hdr::DebandOf(2, 2160);
	check(std::fabs(low.step * 255.0f - 2.0f) < 1e-4f && std::fabs(high.step * 255.0f - 5.0f) < 1e-4f && std::fabs(low.reach - 144.0f) < 1e-3f,
		"debanding's numbers (2 and 5 levels, 32 lines of 480)");
	std::printf("ps5_hdr.h: %s\n", bad ? "FAILED" : "PASS (the choices, the test screen's steps, debanding's numbers)");
	return bad;
}

int main(int argc, char** argv)
{
	int failed = checkChoices();
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
	std::printf("final passes on the PC: the validation layer %s\n", validate ? "on (its messages, if any, above the results)" : "not installed");
	VkInstance instance;
	VKC(vkCreateInstance(&ici, nullptr, &instance));
	uint32_t n = 1;
	const VkResult enumerated = vkEnumeratePhysicalDevices(instance, &n, &gpu);
	if (n == 0 || (enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE))
	{
		std::printf("final passes on the PC: NOT RUN (no Vulkan device)\n");
		return failed ? 1 : 0;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(gpu, &props);
	std::printf("final passes on the PC: %s\n", props.deviceName);
	float priority = 1.0f;
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

	const VkFormat layerFormat = VK_FORMAT_B8G8R8A8_UNORM, ten = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	picture = makeImage(layerFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	menus = makeImage(layerFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	frame = makeImage(layerFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	upPic = makeBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	upUi = makeBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	down = makeBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

	// The utility layout's two pictures as GSDeviceVK binds them (binding 0 the picture, 1 the first extra one,
	// both through the linear sampler) and its push constants (96 bytes, both stages).
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler sampler;
	VKC(vkCreateSampler(device, &sci, nullptr, &sampler));
	VkDescriptorSetLayoutBinding bindings[2] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dlci.bindingCount = 2;
	dlci.pBindings = bindings;
	VkDescriptorSetLayout setLayout;
	VKC(vkCreateDescriptorSetLayout(device, &dlci, nullptr, &setLayout));
	VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &push;
	VKC(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
	VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	VkDescriptorPool pool;
	VKC(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout;
	VKC(vkAllocateDescriptorSets(device, &dsai, &set));
	VkDescriptorImageInfo infos[2] = {{sampler, picture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{sampler, menus.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
	VkWriteDescriptorSet writes[2] = {};
	for (int i = 0; i < 2; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = static_cast<uint32_t>(i);
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

	VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = 0;
	VkCommandPool cmdPool;
	VKC(vkCreateCommandPool(device, &cpci, nullptr, &cmdPool));
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = cmdPool;
	cbai.commandBufferCount = 1;
	VKC(vkAllocateCommandBuffers(device, &cbai, &cmd));

	const VkShaderModule vs = shader(dir + "/screen.vert.spv");
	const Pass hdr = makePass(vs, shader(dir + "/final1.frag.spv"), ten);
	const Pass deband = makePass(vs, shader(dir + "/final0.frag.spv"), layerFormat);

	// ---- HDR: the rows of test pixels (B, G, R, A as the swapchain's B8G8R8A8 stores them) ----
	std::vector<uint8_t> pic(W * H * 4), ui(W * H * 4, 0);
	for (uint32_t y = 6; y < H; y++)
	{
		for (uint32_t x = 0; x < W; x++)
			pic[(y * W + x) * 4 + 3] = 255;
	}
	for (uint32_t x = 0; x < 256; x++)
	{
		const uint8_t v = static_cast<uint8_t>(x);
		auto put = [&](std::vector<uint8_t>& img, uint32_t y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
			uint8_t* p = &img[(y * W + x) * 4];
			p[0] = b;
			p[1] = g;
			p[2] = r;
			p[3] = a;
		};
		put(pic, 0, v, v, v, 255); // grey ramp
		const int c = x % 6;       // colours at ramp levels
		put(pic, 1, c == 2 || c == 4 || c == 5 ? v : 0, c == 1 || c == 3 || c == 4 ? v : 0, c == 0 || c == 3 || c == 5 ? v : 0, 255);
		put(pic, 2, v, static_cast<uint8_t>(255 - v), static_cast<uint8_t>((v * 7) & 255), 255); // mixed
		put(pic, 3, 255, 255, 255, 255);                                                        // full white under the menus
		put(ui, 3, static_cast<uint8_t>(v * 200 / 255), static_cast<uint8_t>(v * 200 / 255), static_cast<uint8_t>(v * 200 / 255), v);
		put(pic, 4, v, v, v, 255);
		put(ui, 4, 0, 0, 0, 128); // a half-dark panel over the ramp
		put(pic, 5, 0, 0, 0, 255);
		put(ui, 5, v, v, v, 255); // opaque menus: the white level, no gain
	}
	const struct
	{
		const char* name;
		Push k;
	} cases[] = {
		{"white 203, highlights 1000", {203, 1000, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"white 203, highlights off", {203, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"white 300, highlights 2000", {300, 2000, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"white 80, highlights 4000", {80, 4000, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"test screen: highlights at 600 nits", {203, 1000, 0, 1, 1, 600, 0, 0, 0, 0, 0, 0}},
		{"test screen: white level 250 nits", {250, 1000, 0, 1, 2, 250, 0, 0, 0, 0, 0, 0}},
	};
	for (const auto& c : cases)
	{
		const uint32_t* words = run(hdr, c.k, pic, ui);
		double worst = 0.0;
		int bad = 0, at = -1;
		const uint32_t rows = c.k.cal > 0.5f ? 6 : H; // the test screen: the rows outside its pattern
		for (uint32_t y = 0; y < rows; y++)
		{
			for (uint32_t x = 0; x < W; x++)
			{
				const uint32_t w = words[y * W + x];
				const double got[3] = {double((w >> 20) & 0x3FF), double((w >> 10) & 0x3FF), double(w & 0x3FF)};
				double want[3];
				expect(&pic[(y * W + x) * 4], &ui[(y * W + x) * 4], c.k, want);
				for (int i = 0; i < 3; i++)
				{
					const double d = std::fabs(got[i] - want[i]);
					worst = std::max(worst, d);
					if (d > 1.0)
					{
						bad++;
						if (at < 0)
							at = static_cast<int>(y * W + x);
					}
				}
			}
		}
		const uint32_t wWhite = words[255], wBlack = words[0], wMenu = words[5 * W + 255];
		std::printf("  %-38s worst %.2f codes, %d off by more than one%s; white R=%u (%.0f nits), menus' white %u (%.0f nits), black %u\n",
			c.name, worst, bad, bad ? " FAIL" : "", (wWhite >> 20) & 0x3FF, nitsOfCode((wWhite >> 20) & 0x3FF), (wMenu >> 20) & 0x3FF,
			nitsOfCode((wMenu >> 20) & 0x3FF), (wBlack >> 20) & 0x3FF);
		if (bad)
		{
			const uint32_t w = words[at];
			double want[3];
			expect(&pic[at * 4], &ui[at * 4], c.k, want);
			std::printf("    first at row %d x %d: got %u %u %u, want %.1f %.1f %.1f\n", at / int(W), at % int(W), (w >> 20) & 0x3FF,
				(w >> 10) & 0x3FF, w & 0x3FF, want[0], want[1], want[2]);
		}
		auto code = [&](uint32_t x, uint32_t y) { return double((words[y * W + x] >> 20) & 0x3FF); };
		auto landmark = [&](const char* what, uint32_t x, uint32_t y, double nits) {
			const double got = code(x, y), want = pqCode(nits);
			const bool ok = std::fabs(got - want) <= 1.0 && code(x, y) == double((words[y * W + x] >> 10) & 0x3FF) &&
							code(x, y) == double(words[y * W + x] & 0x3FF);
			std::printf("    %-44s %4.0f, want %6.1f (%g nits)%s\n", what, got, want, nits, ok ? "" : " FAIL");
			bad += !ok;
		};
		if (c.k.cal > 0.5f && c.k.cal < 1.5f)
		{
			const float side = 0.40f * H;
			landmark("the X in the middle", W / 2, H / 2, c.k.level);
			landmark("the square beside it", W / 2 + static_cast<uint32_t>(0.4f * side), H / 2, 10000.0);
		}
		else if (c.k.cal > 1.5f)
		{
			const float cw = 0.64f * W, ch = 0.40f * H, x0 = (W - cw) * 0.5f, y0 = (H - ch) * 0.5f;
			for (int k : {0, 2, 5, 10})
			{
				char what[64];
				std::snprintf(what, sizeof(what), "grey step %d of 10", k);
				landmark(what, static_cast<uint32_t>(x0 + (k + 0.5f) / 11.0f * cw), static_cast<uint32_t>(y0 + 0.2f * ch),
					std::pow(k / 10.0, 2.2) * c.k.level);
			}
			landmark("the white patch", W / 2, static_cast<uint32_t>(y0 + 0.7f * ch), c.k.level);
		}
		failed += bad != 0;
	}

	// ---- Debanding: gradients in steps, and hard edges ----
	// Rows 0-99: a slow ramp in 8-bit colour's steps (one level every 25 pixels: 32-bit colour's banding at a high
	// resolution); rows 100-199: in 16-bit colour's (one 5-bit level, 8 of 255, every 50 pixels); rows 200-299:
	// columns of black and white 40 pixels wide (edges). The error is against the straight line the steps stand for.
	std::vector<uint8_t> grad(W * H * 4), none(W * H * 4, 0);
	const auto ideal = [](uint32_t x, uint32_t y) -> double {
		if (y < 100)
			return 40.0 + (x - 12.0) / 25.0;
		if (y < 200)
			return 40.0 + 8.0 * (x - 25.0) / 50.0;
		return ((x / 40) % 2) ? 255.0 : 0.0;
	};
	for (uint32_t y = 0; y < H; y++)
	{
		for (uint32_t x = 0; x < W; x++)
		{
			const uint8_t v = static_cast<uint8_t>(y < 100 ? 40 + x / 25 : y < 200 ? 40 + 8 * (x / 50) : ideal(x, y));
			uint8_t* p = &grad[(y * W + x) * 4];
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	// What the eye sees of the bands: the error of the picture's local mean (9 x 9 pixels, which the dither's
	// noise averages out in) against the straight line, in levels of 255.
	const auto bandError = [&](const uint8_t* bgra, uint32_t y0, uint32_t y1, uint32_t margin) {
		double sum = 0.0;
		int count = 0;
		for (uint32_t y = y0 + margin; y < y1 - margin; y++)
		{
			for (uint32_t x = margin; x < W - margin; x++)
			{
				double mean = 0.0;
				for (int dy = -4; dy <= 4; dy++)
				{
					for (int dx = -4; dx <= 4; dx++)
						mean += bgra[((y + dy) * W + (x + dx)) * 4 + 1];
				}
				sum += std::fabs(mean / 81.0 - ideal(x, y));
				count++;
			}
		}
		return sum / count;
	};
	for (int setting : {1, 2})
	{
		const PS5Hdr::Deband d = PS5Hdr::DebandOf(setting, H * 2); // (a reach of 40 pixels: the test's picture is small)
		const Push k{203, 1000, 0, 1, 0, 0, 0, 0, d.step, d.dither, d.reach, 1};
		const uint8_t* out = reinterpret_cast<const uint8_t*>(run(deband, k, grad, none));
		const double before8 = bandError(grad.data(), 0, 100, 45), after8 = bandError(out, 0, 100, 45);
		const double before16 = bandError(grad.data(), 100, 200, 45), after16 = bandError(out, 100, 200, 45);
		// The edges: no pixel moved by more than a band's step (and a level of the dither), and the edges' own
		// pixels kept: black still black and white still white within that.
		int worst = 0;
		for (uint32_t y = 200; y < H; y++)
		{
			for (uint32_t x = 0; x < W; x++)
				worst = std::max(worst, std::abs(out[(y * W + x) * 4 + 1] - static_cast<int>(ideal(x, y))));
		}
		const int allowed = static_cast<int>(std::lround(d.step * 255.0f)) + 1;
		const bool ok8 = after8 < before8 * 0.5;
		const bool ok16 = setting == 1 || after16 < before16 * 0.5;
		const bool ok = ok8 && ok16 && worst <= allowed;
		std::printf("  debanding %s: 8-bit steps' error %.3f -> %.3f levels, 16-bit steps' %.3f -> %.3f; edges: at most %d levels moved "
					"(the step: %d)%s\n",
			setting == 1 ? "low " : "high", before8, after8, before16, after16, worst, allowed - 1, ok ? "" : " FAIL");
		failed += !ok;
	}
	// HDR with debanding on: a flat picture comes out as without it, within the dither (a code or two).
	{
		std::vector<uint8_t> flat(W * H * 4);
		for (size_t i = 0; i < flat.size(); i += 4)
		{
			flat[i] = 90;
			flat[i + 1] = 120;
			flat[i + 2] = 150;
			flat[i + 3] = 255;
		}
		const PS5Hdr::Deband d = PS5Hdr::DebandOf(2, H * 2);
		const Push off{203, 1000, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, on{203, 1000, 0, 1, 0, 0, 0, 0, d.step, d.dither, d.reach, 1};
		std::vector<uint32_t> a(W * H);
		std::memcpy(a.data(), run(hdr, off, flat, none), a.size() * 4);
		const uint32_t* b = run(hdr, on, flat, none);
		int worst = 0;
		for (size_t i = 0; i < a.size(); i++)
		{
			for (int s = 0; s <= 20; s += 10)
				worst = std::max(worst, std::abs(int((a[i] >> s) & 0x3FF) - int((b[i] >> s) & 0x3FF)));
		}
		std::printf("  HDR with debanding, a flat picture: at most %d codes from without it%s\n", worst, worst <= 3 ? "" : " FAIL");
		failed += worst > 3;
	}
	std::printf("final passes on the PC: %s\n", failed ? "FAILED" : "PASS");
	return failed ? 1 : 0;
}

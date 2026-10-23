/* Crux2 - FSR 1 as the title runs it, run on the PC by a software Vulkan driver.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0+
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 *   fsr_test <easu.spv> <rcas.spv> <vert.spv> <out folder>
 *   fsr_test <easu.spv> <rcas.spv> <vert.spv> --compile-only
 *
 * The two fragment shaders are the title's own (resources/shaders/vulkan/fsr1.glsl with AMD's
 * headers put in, under the header PCSX2 gives its utility shaders: run-fsr-tests.sh makes them),
 * and the constants are worked out as GSDevice::FSR works them out. A picture that is known at
 * every size (drawn by a formula) is made small, enlarged by the two passes, read back, and held
 * against the same picture made at the large size, and against a bilinear enlargement of the
 * small one: FSR has to be the closer of the two. A plain colour has to stay that colour, and a
 * picture that sits in a part of a larger texture (as a game's does) has to come out the same as
 * when it fills its texture.
 *
 * What this cannot show is the console's own driver running them (RADV compiles them there on
 * the first use) or what it costs: that is for a console.
 */
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(what, ...) \
	do \
	{ \
		s_checks++; \
		if (!(what)) \
		{ \
			s_failed++; \
			printf("FAILED %s:%d: %s  ", __FILE__, __LINE__, #what); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)
#define VK(call) \
	do \
	{ \
		const VkResult r_ = (call); \
		if (r_ != VK_SUCCESS) \
		{ \
			fprintf(stderr, "%s: %d\n", #call, (int)r_); \
			exit(2); \
		} \
	} while (0)

static VkInstance s_instance;
static VkPhysicalDevice s_gpu;
static VkDevice s_device;
static VkQueue s_queue;
static uint32_t s_family;
static VkCommandPool s_pool;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(s_gpu, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
	{
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	}
	fprintf(stderr, "no memory type\n");
	exit(2);
}

struct image
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	uint32_t width, height;
};

static struct image make_image(uint32_t width, uint32_t height, VkRenderPass pass)
{
	struct image out = {.width = width, .height = height};
	const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {width, height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
	VK(vkCreateImage(s_device, &info, NULL, &out.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s_device, out.image, &req);
	const VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VK(vkAllocateMemory(s_device, &alloc, NULL, &out.memory));
	VK(vkBindImageMemory(s_device, out.image, out.memory, 0));
	const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out.image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VK(vkCreateImageView(s_device, &view, NULL, &out.view));
	const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass, .attachmentCount = 1,
		.pAttachments = &out.view, .width = width, .height = height, .layers = 1};
	VK(vkCreateFramebuffer(s_device, &fb, NULL, &out.framebuffer));
	return out;
}

static void free_image(struct image* image)
{
	vkDestroyFramebuffer(s_device, image->framebuffer, NULL);
	vkDestroyImageView(s_device, image->view, NULL);
	vkDestroyImage(s_device, image->image, NULL);
	vkFreeMemory(s_device, image->memory, NULL);
}

struct buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	void* data;
};

static struct buffer make_buffer(VkDeviceSize size)
{
	struct buffer out;
	const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
	VK(vkCreateBuffer(s_device, &info, NULL, &out.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s_device, out.buffer, &req);
	const VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VK(vkAllocateMemory(s_device, &alloc, NULL, &out.memory));
	VK(vkBindBufferMemory(s_device, out.buffer, out.memory, 0));
	VK(vkMapMemory(s_device, out.memory, 0, size, 0, &out.data));
	return out;
}

static void free_buffer(struct buffer* buffer)
{
	vkDestroyBuffer(s_device, buffer->buffer, NULL);
	vkFreeMemory(s_device, buffer->memory, NULL);
}

static VkShaderModule load_shader(const char* path)
{
	FILE* const fp = fopen(path, "rb");
	if (!fp)
	{
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	fseek(fp, 0, SEEK_END);
	const long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint32_t* const code = malloc((size_t)size);
	if (fread(code, 1, (size_t)size, fp) != (size_t)size)
		exit(2);
	fclose(fp);
	const VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
	VkShaderModule module;
	VK(vkCreateShaderModule(s_device, &info, NULL, &module));
	free(code);
	return module;
}

static void layout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
	const VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* ---- the picture that is known at every size: u, v in 0..1 */
static void scene(double u, double v, double rgb[3])
{
	/* A sky's gradient, a sun, slanted edges of every angle, thin lines, a checker in a corner, and
	 * letters' worth of small squares: what a game's picture is made of. */
	double r = 0.15 + 0.5 * v, g = 0.25 + 0.4 * v, b = 0.55 - 0.2 * v;
	const double dx = u - 0.72, dy = (v - 0.28) * 0.7;
	const double d = sqrt(dx * dx + dy * dy);
	if (d < 0.11)
	{
		r = 1.0;
		g = 0.85;
		b = 0.3;
	}
	for (int i = 0; i < 9; i++)
	{
		const double angle = 0.12 + i * 0.17, cx = 0.08 + i * 0.1;
		const double side = (u - cx) * cos(angle) + (v - 0.75) * sin(angle);
		if (fabs(side) < 0.012 && v > 0.5)
		{
			r = 0.95 - i * 0.07;
			g = 0.2 + i * 0.07;
			b = 0.25;
		}
	}
	if (u < 0.3 && v < 0.3)
	{
		const int cell = ((int)floor(u * 40) + (int)floor(v * 28)) & 1;
		r = g = b = cell ? 0.9 : 0.1;
	}
	if (v > 0.4 && v < 0.47 && u > 0.35)
	{
		const double t = (u - 0.35) * 60;
		if (fmod(t, 2.0) < 1.0 && fmod((v - 0.4) * 200, 3.0) < 2.0)
			r = g = b = 0.97;
	}
	rgb[0] = r;
	rgb[1] = g;
	rgb[2] = b;
}

/* The scene at a size: each pixel the mean of 4 x 4 points in it, as a renderer's would be. */
static void render_scene(uint8_t* pixels, uint32_t width, uint32_t height)
{
	for (uint32_t y = 0; y < height; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			double sum[3] = {0, 0, 0};
			for (int sy = 0; sy < 4; sy++)
			{
				for (int sx = 0; sx < 4; sx++)
				{
					double rgb[3];
					scene((x + (sx + 0.5) / 4) / width, (y + (sy + 0.5) / 4) / height, rgb);
					sum[0] += rgb[0];
					sum[1] += rgb[1];
					sum[2] += rgb[2];
				}
			}
			uint8_t* const p = pixels + ((size_t)y * width + x) * 4;
			for (int c = 0; c < 3; c++)
				p[c] = (uint8_t)lrint(fmin(fmax(sum[c] / 16, 0), 1) * 255);
			p[3] = 255;
		}
	}
}

static void bilinear(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh)
{
	for (uint32_t y = 0; y < dh; y++)
	{
		for (uint32_t x = 0; x < dw; x++)
		{
			const double fx = fmin(fmax((x + 0.5) * sw / dw - 0.5, 0), sw - 1), fy = fmin(fmax((y + 0.5) * sh / dh - 0.5, 0), sh - 1);
			const uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy, x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
			const double tx = fx - x0, ty = fy - y0;
			for (int c = 0; c < 4; c++)
			{
				const double top = src[((size_t)y0 * sw + x0) * 4 + c] * (1 - tx) + src[((size_t)y0 * sw + x1) * 4 + c] * tx;
				const double bottom = src[((size_t)y1 * sw + x0) * 4 + c] * (1 - tx) + src[((size_t)y1 * sw + x1) * 4 + c] * tx;
				dst[((size_t)y * dw + x) * 4 + c] = (uint8_t)lrint(top * (1 - ty) + bottom * ty);
			}
		}
	}
}

static double psnr(const uint8_t* a, const uint8_t* b, uint32_t width, uint32_t height)
{
	double sum = 0;
	for (size_t i = 0; i < (size_t)width * height; i++)
	{
		for (int c = 0; c < 3; c++)
		{
			const double d = (double)a[i * 4 + c] - b[i * 4 + c];
			sum += d * d;
		}
	}
	const double mse = sum / ((double)width * height * 3);
	return mse <= 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

static void write_ppm(const char* folder, const char* name, const uint8_t* pixels, uint32_t width, uint32_t height)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
	FILE* const fp = fopen(path, "wb");
	if (!fp)
		return;
	fprintf(fp, "P6\n%u %u\n255\n", width, height);
	for (size_t i = 0; i < (size_t)width * height; i++)
		fwrite(pixels + i * 4, 1, 3, fp);
	fclose(fp);
}

static uint32_t bits(float value)
{
	uint32_t out;
	memcpy(&out, &value, 4);
	return out;
}

static VkRenderPass s_pass;
static VkPipelineLayout s_layout;
static VkDescriptorSetLayout s_set_layout;
static VkDescriptorPool s_descriptors;
static VkPipeline s_easu, s_rcas;
static VkSampler s_sampler;

static VkPipeline make_pipeline(VkShaderModule vert, VkShaderModule frag)
{
	const VkPipelineShaderStageCreateInfo stages[2] = {
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
	const VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	const VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	const VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
	const VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
	const VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 0xf};
	const VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &attachment};
	const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	const VkPipelineDynamicStateCreateInfo dynamics = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
		.pDynamicStates = dynamic};
	const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages, .pVertexInputState = &input, .pInputAssemblyState = &assembly, .pViewportState = &viewport,
		.pRasterizationState = &raster, .pMultisampleState = &samples, .pColorBlendState = &blend, .pDynamicState = &dynamics,
		.layout = s_layout, .renderPass = s_pass};
	VkPipeline pipeline;
	VK(vkCreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
	return pipeline;
}

static void draw(VkCommandBuffer cmd, VkPipeline pipeline, const struct image* from, const struct image* to, const uint32_t* constants, uint32_t count)
{
	VkDescriptorSet set;
	const VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s_descriptors,
		.descriptorSetCount = 1, .pSetLayouts = &s_set_layout};
	VK(vkAllocateDescriptorSets(s_device, &alloc, &set));
	const VkDescriptorImageInfo image = {.sampler = s_sampler, .imageView = from->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
	vkUpdateDescriptorSets(s_device, 1, &write, 0, NULL);

	const VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s_pass, .framebuffer = to->framebuffer,
		.renderArea = {{0, 0}, {to->width, to->height}}};
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = {0, 0, (float)to->width, (float)to->height, 0, 1};
	const VkRect2D scissor = {{0, 0}, {to->width, to->height}};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, count * 4, constants);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

/* The two passes over `source` (a texture tw x th of which the picture is the part at (ox, oy),
 * sw x sh), to dw x dh; the result's pixels into `out`. `easu_only`: the first pass's.
 * The constants are GSDevice::FSR's. */
static void fsr(const uint8_t* source, uint32_t tw, uint32_t th, uint32_t ox, uint32_t oy, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
	float sharpness_percent, int easu_only, uint8_t* out)
{
	struct image src = make_image(tw, th, s_pass), mid = make_image(dw, dh, s_pass), dst = make_image(dw, dh, s_pass);
	struct buffer upload = make_buffer((VkDeviceSize)tw * th * 4), download = make_buffer((VkDeviceSize)dw * dh * 4);
	memcpy(upload.data, source, (size_t)tw * th * 4);

	uint32_t constants[20] = {0};
	const float iw = (float)sw, ih = (float)sh, fw = (float)tw, fh = (float)th;
	constants[0] = bits(iw / (float)dw);
	constants[1] = bits(ih / (float)dh);
	constants[2] = bits(0.5f * iw / (float)dw - 0.5f + (float)ox);
	constants[3] = bits(0.5f * ih / (float)dh - 0.5f + (float)oy);
	constants[4] = bits(1.0f / fw);
	constants[5] = bits(1.0f / fh);
	constants[6] = bits(1.0f / fw);
	constants[7] = bits(-1.0f / fh);
	constants[8] = bits(-1.0f / fw);
	constants[9] = bits(2.0f / fh);
	constants[10] = bits(1.0f / fw);
	constants[11] = bits(2.0f / fh);
	constants[12] = bits(0.0f);
	constants[13] = bits(4.0f / fh);
	constants[16] = bits(exp2f(-(1.0f - sharpness_percent * 0.01f) * 2.0f));

	const VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer cmd;
	VK(vkAllocateCommandBuffers(s_device, &alloc, &cmd));
	const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VK(vkBeginCommandBuffer(cmd, &begin));
	layout(cmd, src.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkBufferImageCopy up = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {tw, th, 1}};
	vkCmdCopyBufferToImage(cmd, upload.buffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
	layout(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	layout(cmd, mid.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	layout(cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	draw(cmd, s_easu, &src, &mid, constants, 16);
	layout(cmd, mid.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	draw(cmd, s_rcas, &mid, &dst, constants + 16, 4);
	const struct image* const result = easu_only ? &mid : &dst;
	layout(cmd, result->image, easu_only ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	const VkBufferImageCopy down = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {dw, dh, 1}};
	vkCmdCopyImageToBuffer(cmd, result->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download.buffer, 1, &down);
	VK(vkEndCommandBuffer(cmd));
	const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	VK(vkQueueSubmit(s_queue, 1, &submit, VK_NULL_HANDLE));
	VK(vkQueueWaitIdle(s_queue));
	memcpy(out, download.data, (size_t)dw * dh * 4);
	vkFreeCommandBuffers(s_device, s_pool, 1, &cmd);
	VK(vkResetDescriptorPool(s_device, s_descriptors, 0));
	free_buffer(&upload);
	free_buffer(&download);
	free_image(&src);
	free_image(&mid);
	free_image(&dst);
}

int main(int argc, char** argv)
{
	if (argc != 5)
	{
		fprintf(stderr, "usage: fsr_test <easu.spv> <rcas.spv> <vert.spv> <out folder | --compile-only>\n");
		return 2;
	}
	const char* const out = argv[4];
	/* --compile-only: the two pipelines are made and nothing is drawn. For a driver that compiles
	 * and cannot draw: the console's own, built for the PC with no graphics chip under it. */
	const int compile_only = (strcmp(out, "--compile-only") == 0);

	const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	const VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VK(vkCreateInstance(&instance, NULL, &s_instance));
	uint32_t count = 1;
	vkEnumeratePhysicalDevices(s_instance, &count, &s_gpu);
	if (count == 0)
	{
		fprintf(stderr, "no Vulkan device\n");
		return 2;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(s_gpu, &props);
	printf("  on: %s\n", props.deviceName);
	VkQueueFamilyProperties families[16];
	count = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &count, families);
	for (s_family = 0; s_family < count && !(families[s_family].queueFlags & VK_QUEUE_GRAPHICS_BIT); s_family++)
	{
	}
	const float priority = 1;
	const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = s_family, .queueCount = 1,
		.pQueuePriorities = &priority};
	const VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
	VK(vkCreateDevice(s_gpu, &device, NULL, &s_device));
	vkGetDeviceQueue(s_device, s_family, 0, &s_queue);
	const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = s_family};
	VK(vkCreateCommandPool(s_device, &pool, NULL, &s_pool));

	const VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	const VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &reference};
	const VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment,
		.subpassCount = 1, .pSubpasses = &subpass};
	VK(vkCreateRenderPass(s_device, &pass, NULL, &s_pass));

	const VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
	const VkDescriptorSetLayoutCreateInfo set_layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
	VK(vkCreateDescriptorSetLayout(s_device, &set_layout, NULL, &s_set_layout));
	/* (As PCSX2's utility pipelines: 96 bytes of push constants for both stages.) */
	const VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
	const VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &s_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
	VK(vkCreatePipelineLayout(s_device, &pipeline_layout, NULL, &s_layout));
	const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
	const VkDescriptorPoolCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 1, .pPoolSizes = &size};
	VK(vkCreateDescriptorPool(s_device, &descriptors, NULL, &s_descriptors));
	const VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
	VK(vkCreateSampler(s_device, &sampler, NULL, &s_sampler));

	const VkShaderModule vert = load_shader(argv[3]), easu = load_shader(argv[1]), rcas = load_shader(argv[2]);
	s_easu = make_pipeline(vert, easu);
	s_rcas = make_pipeline(vert, rcas);
	if (compile_only)
	{
		printf("  both pipelines were made by: %s\n", props.deviceName);
		return 0;
	}

	/* A game drawn at twice and at three times the PlayStation 2's size, shown at 4K's height. */
	static const struct
	{
		uint32_t sw, sh, dw, dh;
		const char* name;
	} cases[] = {{1280, 896, 3072, 2150, "2x-to-4k"}, {1920, 1344, 3072, 2150, "3x-to-4k"}, {640, 448, 1280, 896, "1x-to-2x"}};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		const uint32_t sw = cases[i].sw, sh = cases[i].sh, dw = cases[i].dw, dh = cases[i].dh;
		uint8_t* const small = malloc((size_t)sw * sh * 4);
		uint8_t* const truth = malloc((size_t)dw * dh * 4);
		uint8_t* const smooth = malloc((size_t)dw * dh * 4);
		uint8_t* const easu_out = malloc((size_t)dw * dh * 4);
		uint8_t* const fsr_out = malloc((size_t)dw * dh * 4);
		render_scene(small, sw, sh);
		render_scene(truth, dw, dh);
		bilinear(small, sw, sh, smooth, dw, dh);
		fsr(small, sw, sh, 0, 0, sw, sh, dw, dh, 50, 1, easu_out);
		fsr(small, sw, sh, 0, 0, sw, sh, dw, dh, 50, 0, fsr_out);
		const double p_bilinear = psnr(smooth, truth, dw, dh), p_easu = psnr(easu_out, truth, dw, dh), p_fsr = psnr(fsr_out, truth, dw, dh);
		printf("  %s (%ux%u to %ux%u): against the true picture, bilinear %.2f dB, the first pass %.2f dB, both passes %.2f dB\n", cases[i].name, sw,
			sh, dw, dh, p_bilinear, p_easu, p_fsr);
		CHECK(p_easu > p_bilinear + 0.5, "%.2f against %.2f", p_easu, p_bilinear);
		CHECK(p_fsr > p_bilinear, "%.2f against %.2f", p_fsr, p_bilinear);
		/* Nothing is shifted: the first pass against bilinear is far closer than a pixel's shift would leave it. */
		CHECK(psnr(easu_out, smooth, dw, dh) > 28.0, "%.2f", psnr(easu_out, smooth, dw, dh));
		if (i == 0)
		{
			write_ppm(out, "small", small, sw, sh);
			write_ppm(out, "truth", truth, dw, dh);
			write_ppm(out, "bilinear", smooth, dw, dh);
			write_ppm(out, "fsr", fsr_out, dw, dh);
		}

		/* The same picture in a part of a larger texture, with other things around it: the same result
		 * (but for the pixels at its edge, where the filter looks two texels beyond it). */
		{
			const uint32_t tw = sw + 96, th = sh + 64, ox = 40, oy = 24;
			uint8_t* const sheet = malloc((size_t)tw * th * 4);
			for (size_t p = 0; p < (size_t)tw * th; p++)
			{
				sheet[p * 4 + 0] = 255;
				sheet[p * 4 + 1] = 0;
				sheet[p * 4 + 2] = 255;
				sheet[p * 4 + 3] = 255;
			}
			for (uint32_t y = 0; y < sh; y++)
				memcpy(sheet + ((size_t)(y + oy) * tw + ox) * 4, small + (size_t)y * sw * 4, (size_t)sw * 4);
			uint8_t* const part = malloc((size_t)dw * dh * 4);
			fsr(sheet, tw, th, ox, oy, sw, sh, dw, dh, 50, 0, part);
			const uint32_t margin = 3 * dw / sw + 4;
			size_t differing = 0, inside = 0;
			int worst = 0;
			for (uint32_t y = margin; y < dh - margin; y++)
			{
				for (uint32_t x = margin; x < dw - margin; x++)
				{
					inside++;
					for (int c = 0; c < 3; c++)
					{
						const int d = abs((int)part[((size_t)y * dw + x) * 4 + c] - (int)fsr_out[((size_t)y * dw + x) * 4 + c]);
						if (d > worst)
							worst = d;
						if (d > 2)
						{
							differing++;
							break;
						}
					}
				}
			}
			CHECK(differing * 1000 < inside, "%zu of %zu pixels differ, by up to %d", differing, inside, worst);
			free(sheet);
			free(part);
		}
		free(small);
		free(truth);
		free(smooth);
		free(easu_out);
		free(fsr_out);
	}

	/* A plain colour stays that colour, at every sharpness. */
	{
		const uint32_t sw = 320, sh = 224, dw = 800, dh = 560;
		uint8_t* const flat = malloc((size_t)sw * sh * 4);
		uint8_t* const result = malloc((size_t)dw * dh * 4);
		for (size_t p = 0; p < (size_t)sw * sh; p++)
		{
			flat[p * 4 + 0] = 200;
			flat[p * 4 + 1] = 90;
			flat[p * 4 + 2] = 30;
			flat[p * 4 + 3] = 255;
		}
		for (int sharpness = 0; sharpness <= 100; sharpness += 50)
		{
			fsr(flat, sw, sh, 0, 0, sw, sh, dw, dh, (float)sharpness, 0, result);
			int worst = 0;
			for (size_t p = 0; p < (size_t)dw * dh; p++)
			{
				const int d0 = abs(result[p * 4] - 200), d1 = abs(result[p * 4 + 1] - 90), d2 = abs(result[p * 4 + 2] - 30);
				worst = d0 > worst ? d0 : worst;
				worst = d1 > worst ? d1 : worst;
				worst = d2 > worst ? d2 : worst;
			}
			CHECK(worst <= 1, "sharpness %d: a plain colour is off by %d", sharpness, worst);
		}
		free(flat);
		free(result);
	}

	/* More sharpness is more contrast at an edge, and never a colour out of range. */
	{
		const uint32_t sw = 640, sh = 448, dw = 1600, dh = 1120;
		uint8_t* const small = malloc((size_t)sw * sh * 4);
		uint8_t* const soft = malloc((size_t)dw * dh * 4);
		uint8_t* const sharp = malloc((size_t)dw * dh * 4);
		render_scene(small, sw, sh);
		fsr(small, sw, sh, 0, 0, sw, sh, dw, dh, 0, 0, soft);
		fsr(small, sw, sh, 0, 0, sw, sh, dw, dh, 100, 0, sharp);
		double contrast_soft = 0, contrast_sharp = 0;
		for (uint32_t y = 0; y < dh; y++)
		{
			for (uint32_t x = 1; x < dw; x++)
			{
				contrast_soft += abs(soft[((size_t)y * dw + x) * 4 + 1] - soft[((size_t)y * dw + x - 1) * 4 + 1]);
				contrast_sharp += abs(sharp[((size_t)y * dw + x) * 4 + 1] - sharp[((size_t)y * dw + x - 1) * 4 + 1]);
			}
		}
		printf("  sharpness: the sum of steps between neighbours is %.0f at 0%% and %.0f at 100%%\n", contrast_soft, contrast_sharp);
		CHECK(contrast_sharp > contrast_soft * 1.02, "%.0f against %.0f", contrast_sharp, contrast_soft);
		free(small);
		free(soft);
		free(sharp);
	}

	printf("FSR 1: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}

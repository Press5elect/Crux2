/* Crux2 - frame generation as the title runs it, run on the PC by a software Vulkan driver.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0+
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 *   fg_test <folder with pass0.spv .. pass7.spv and vert.spv> <out folder>
 *   fg_test <that folder> --compile-only
 *
 * The eight fragment shaders are the title's own (resources/shaders/vulkan/framegen.glsl under
 * the header PCSX2 gives its utility shaders: run-framegen-tests.sh makes them), and they are run
 * in the order and with the sizes GSDevice::FrameGenMake runs them. Two pictures of a scene whose
 * every moment can be drawn (a formula) go in, the picture between them comes out and is held
 * against the scene drawn at that moment: it has to be nearer to it than the older picture
 * shown again (what a screen shows without frame generation) and than the two pictures mixed.
 * A scene that stands still has to come out as it went in, and after a cut to another scene the
 * older picture has to stay.
 *
 * What this cannot show is the console's driver running the passes, what they cost there, or
 * how a game looks with them: that is for a console.
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

#define PASSES 8
#define SMALL VK_FORMAT_R16G16B16A16_SFLOAT /* GSTexture::Format::ColorHDR */
#define PICTURE VK_FORMAT_R8G8B8A8_UNORM    /* GSTexture::Format::Color */

static VkInstance s_instance;
static VkPhysicalDevice s_gpu;
static VkDevice s_device;
static VkQueue s_queue;
static uint32_t s_family;
static VkCommandPool s_pool;
static VkRenderPass s_pass_small, s_pass_picture;
static VkPipelineLayout s_layout;
static VkDescriptorSetLayout s_set_layout;
static VkDescriptorPool s_descriptors;
static VkPipeline s_pipelines[PASSES];
static VkSampler s_sampler;

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
	VkFormat format;
};

static struct image make_image(uint32_t width, uint32_t height, VkFormat format)
{
	struct image out = {.width = width, .height = height, .format = format};
	const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
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
		.format = format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VK(vkCreateImageView(s_device, &view, NULL, &out.view));
	const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = (format == PICTURE) ? s_pass_picture : s_pass_small, .attachmentCount = 1, .pAttachments = &out.view, .width = width,
		.height = height, .layers = 1};
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

static VkShaderModule load_shader(const char* folder, const char* name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", folder, name);
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

static VkRenderPass make_pass(VkFormat format)
{
	const VkAttachmentDescription attachment = {.format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	const VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &reference};
	const VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment,
		.subpassCount = 1, .pSubpasses = &subpass};
	VkRenderPass out;
	VK(vkCreateRenderPass(s_device, &pass, NULL, &out));
	return out;
}

static VkPipeline make_pipeline(VkShaderModule vert, VkShaderModule frag, VkRenderPass pass)
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
		.layout = s_layout, .renderPass = pass};
	VkPipeline pipeline;
	VK(vkCreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
	return pipeline;
}

/* One pass: `to` written from up to four pictures, as GSDeviceVK::DoFrameGenPass does it. The
 * three constants' rows are the shader's: the target's size, the first picture's, and the time
 * with what the shader's u_params are told (how the pass is run, whether the search reads between
 * pixels, how far around its start it looks). */
static void draw(VkCommandBuffer cmd, int pass, const struct image* to, const struct image* const* from, uint32_t from_count, float time,
	int how, int soft, int reach)
{
	VkDescriptorSet set;
	const VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s_descriptors,
		.descriptorSetCount = 1, .pSetLayouts = &s_set_layout};
	VK(vkAllocateDescriptorSets(s_device, &alloc, &set));
	VkDescriptorImageInfo images[4];
	VkWriteDescriptorSet writes[4];
	for (uint32_t i = 0; i < 4; i++)
	{
		/* (A binding a pass does not read is given the first picture: something has to be there.) */
		images[i] = (VkDescriptorImageInfo){.sampler = s_sampler, .imageView = from[(i < from_count) ? i : 0]->view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[i]};
	}
	vkUpdateDescriptorSets(s_device, 4, writes, 0, NULL);

	const float constants[12] = {(float)to->width, (float)to->height, 1.0f / (float)to->width, 1.0f / (float)to->height,
		(float)from[0]->width, (float)from[0]->height, 1.0f / (float)from[0]->width, 1.0f / (float)from[0]->height, time, (float)how, (float)soft, (float)reach};
	const VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = (to->format == PICTURE) ? s_pass_picture : s_pass_small, .framebuffer = to->framebuffer,
		.renderArea = {{0, 0}, {to->width, to->height}}};
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = {0, 0, (float)to->width, (float)to->height, 0, 1};
	const VkRect2D scissor = {{0, 0}, {to->width, to->height}};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipelines[pass]);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	layout(cmd, to->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* A half float: sign, 5 bits of exponent, 10 of fraction. */
static double half_float(uint16_t raw)
{
	const int exponent = (raw >> 10) & 31, fraction = raw & 1023;
	const double value = (exponent == 0) ? ldexp(fraction, -24) : ldexp(1024 + fraction, exponent - 25);
	return (raw & 0x8000) ? -value : value;
}

static uint32_t half(uint32_t size)
{
	return (size + 1) / 2 ? (size + 1) / 2 : 1;
}

/* The picture between `older` and `newer` (both w x h, RGBA) at `time`, into `out`. The sizes and
 * the order are GSDevice::FrameGenMake's. `no_match` and `wild`, when given, get the two numbers
 * the passes find for the picture as a whole (a cut to another scene is told by them). */
static void between(const uint8_t* older, const uint8_t* newer, uint32_t w, uint32_t h, float time, uint8_t* out, double* no_match, double* wild)
{
	uint32_t pw[5], ph[5]; /* the pairs: a quarter, an eighth, a sixteenth, a thirty-second, a sixty-fourth */
	pw[0] = half(half(w));
	ph[0] = half(half(h));
	for (int i = 1; i < 5; i++)
	{
		pw[i] = half(pw[i - 1]);
		ph[i] = half(ph[i - 1]);
	}
	struct image a = make_image(w, h, PICTURE), b = make_image(w, h, PICTURE), result = make_image(w, h, PICTURE);
	struct image pair[5], first64 = make_image(pw[4], ph[4], SMALL), move64 = make_image(pw[4], ph[4], SMALL),
						  spread = make_image(pw[4], ph[4], SMALL), move32 = make_image(pw[3], ph[3], SMALL), move16 = make_image(pw[2], ph[2], SMALL),
						  move8 = make_image(pw[1], ph[1], SMALL), moves = make_image(pw[1], ph[1], SMALL), none = make_image(1, 1, SMALL),
						  camera0 = make_image(1, 1, SMALL), camera = make_image(1, 1, SMALL);
	for (int i = 0; i < 5; i++)
		pair[i] = make_image(pw[i], ph[i], SMALL);
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
	struct buffer up_a = make_buffer(bytes), up_b = make_buffer(bytes), down = make_buffer(bytes), down_none = make_buffer(8);
	memcpy(up_a.data, older, bytes);
	memcpy(up_b.data, newer, bytes);

	const VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer cmd;
	VK(vkAllocateCommandBuffers(s_device, &alloc, &cmd));
	const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VK(vkBeginCommandBuffer(cmd, &begin));
	const VkBufferImageCopy whole = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
	layout(cmd, a.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	layout(cmd, b.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(cmd, up_a.buffer, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
	vkCmdCopyBufferToImage(cmd, up_b.buffer, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
	layout(cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	layout(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	struct image* const targets[] = {&result, &pair[0], &pair[1], &pair[2], &pair[3], &pair[4], &first64, &move64, &spread, &move32, &move16,
		&move8, &moves, &none, &camera0, &camera};
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
		layout(cmd, targets[i]->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
#define FROM(...) (const struct image* const[]){__VA_ARGS__}
#define AGAIN(picture) layout(cmd, (picture).image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)

	draw(cmd, 0, &pair[0], FROM(&a, &b), 2, 0, 0, 0, 0);
	for (int i = 1; i < 5; i++)
		draw(cmd, 1, &pair[i], FROM(&pair[i - 1]), 1, 0, 0, 0, 0);
	/* The smallest pair: searched from nothing, the camera's move from that (twice: the mean, then
	 * the mean of what is near it), and searched again from the camera's move. (The first runs
	 * are handed a picture they do not read: `camera`, which is written before it is.) */
	layout(cmd, camera.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	draw(cmd, 2, &first64, FROM(&pair[4], &camera), 2, 0, 0, 1, 4);
	draw(cmd, 3, &camera0, FROM(&first64, &camera), 2, 0, 0, 0, 0);
	AGAIN(camera);
	draw(cmd, 3, &camera, FROM(&first64, &camera0), 2, 0, 1, 0, 0);
	draw(cmd, 2, &move64, FROM(&pair[4], &camera), 2, 0, 1, 1, 4);
	/* Blocks that are not sure take the sure ones' answer: there and back between two pictures. */
	draw(cmd, 4, &spread, FROM(&move64), 1, 0, 0, 0, 0);
	AGAIN(move64);
	draw(cmd, 4, &move64, FROM(&spread), 1, 0, 0, 0, 0);
	/* The larger pairs, each from the answer of the one before; the last one read as it is. */
	draw(cmd, 2, &move32, FROM(&pair[3], &move64), 2, 0, 1, 2, 3);
	draw(cmd, 2, &move16, FROM(&pair[2], &move32), 2, 0, 1, 2, 3);
	draw(cmd, 2, &move8, FROM(&pair[1], &move16), 2, 0, 2, 0, 1);
	draw(cmd, 5, &moves, FROM(&move8), 1, 0, 0, 0, 0);
	draw(cmd, 6, &none, FROM(&moves, &move8), 2, 0, 0, 0, 0);
	draw(cmd, 7, &result, FROM(&a, &b, &moves, &none), 4, time, 0, 0, 0);

	layout(cmd, result.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cmd, result.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buffer, 1, &whole);
	layout(cmd, none.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	const VkBufferImageCopy one = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
	vkCmdCopyImageToBuffer(cmd, none.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down_none.buffer, 1, &one);
	/* (For a look at the moves at each size: FG_MOVES names a folder for them.) */
	struct image* const looked_at[] = {&move64, &move32, &move16, &move8, &moves};
	struct buffer looks[5];
	if (getenv("FG_MOVES"))
	{
		for (int i = 0; i < 5; i++)
		{
			looks[i] = make_buffer((VkDeviceSize)looked_at[i]->width * looked_at[i]->height * 8);
			layout(cmd, looked_at[i]->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			const VkBufferImageCopy all = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				.imageExtent = {looked_at[i]->width, looked_at[i]->height, 1}};
			vkCmdCopyImageToBuffer(cmd, looked_at[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, looks[i].buffer, 1, &all);
		}
	}
	VK(vkEndCommandBuffer(cmd));
	const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	VK(vkQueueSubmit(s_queue, 1, &submit, VK_NULL_HANDLE));
	VK(vkQueueWaitIdle(s_queue));
	memcpy(out, down.data, bytes);
	if (getenv("FG_MOVES"))
	{
		static int number = 0;
		static const char* const names[5] = {"move64", "move32", "move16", "move8", "moves"};
		for (int i = 0; i < 5; i++)
		{
			char path[1024];
			snprintf(path, sizeof(path), "%s/%s%02d-%ux%u.bin", getenv("FG_MOVES"), names[i], number, looked_at[i]->width, looked_at[i]->height);
			FILE* const fp = fopen(path, "wb");
			if (fp)
			{
				fwrite(looks[i].data, 8, (size_t)looked_at[i]->width * looked_at[i]->height, fp);
				fclose(fp);
			}
			free_buffer(&looks[i]);
		}
		number++;
	}
	if (no_match)
	{
		uint16_t raw[2];
		memcpy(raw, down_none.data, 4);
		*no_match = half_float(raw[0]);
		*wild = half_float(raw[1]);
	}
	vkFreeCommandBuffers(s_device, s_pool, 1, &cmd);
	VK(vkResetDescriptorPool(s_device, s_descriptors, 0));
	free_buffer(&up_a);
	free_buffer(&up_b);
	free_buffer(&down);
	free_buffer(&down_none);
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
		free_image(targets[i]);
	free_image(&a);
	free_image(&b);
}

/* ---- scenes whose every moment can be drawn */

static double hash2(int x, int y)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (double)((h ^ (h >> 16)) & 0xffffff) / (double)0xffffff;
}

/* Smooth noise, the grain of a wall or of grass: `scale` lumps across the picture's width. */
static double grain(double u, double v, double scale)
{
	const double x = u * scale, y = v * scale;
	const int ix = (int)floor(x), iy = (int)floor(y);
	const double fx = x - ix, fy = y - iy;
	const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
	const double top = hash2(ix, iy) + (hash2(ix + 1, iy) - hash2(ix, iy)) * sx;
	const double bottom = hash2(ix, iy + 1) + (hash2(ix + 1, iy + 1) - hash2(ix, iy + 1)) * sx;
	return top + (bottom - top) * sy;
}

/* A landscape without end in every direction (u, v in parts of the picture's height): ground
 * with grain at three sizes, a sky above, posts and a fence, tiles in places. */
static void land(double u, double v, double rgb[3])
{
	const double g = 0.5 * grain(u, v, 9) + 0.3 * grain(u, v, 31) + 0.2 * grain(u, v, 97);
	double r, gr, b;
	if (v < 0.38 + 0.05 * sin(u * 2.3))
	{
		/* sky, with clouds */
		const double c = grain(u * 0.6, v * 1.7, 5);
		r = 0.35 + 0.4 * c;
		gr = 0.5 + 0.35 * c;
		b = 0.8 + 0.15 * c;
	}
	else
	{
		r = 0.25 + 0.45 * g;
		gr = 0.3 + 0.5 * g;
		b = 0.15 + 0.25 * g;
		/* tiles in every third stretch */
		if (fmod(floor(u * 1.5) + 300.0, 3.0) < 1.0 && v > 0.7)
		{
			const int cell = ((int)floor(u * 24) + (int)floor(v * 24)) & 1;
			r = gr = b = (cell ? 0.75 : 0.3) + 0.1 * g;
		}
	}
	/* posts */
	const double post = fmod(u * 3.0 + 300.0, 1.0);
	if (post < 0.035 && v > 0.25 && v < 0.8)
	{
		r = 0.35 + 0.2 * g;
		gr = 0.22 + 0.1 * g;
		b = 0.1;
	}
	/* a fence's rails */
	if (fabs(v - 0.62) < 0.006 || fabs(v - 0.68) < 0.006)
	{
		r = 0.85;
		gr = 0.82;
		b = 0.7;
	}
	rgb[0] = r;
	rgb[1] = gr;
	rgb[2] = b;
}

/* What moves in front of it (cx, cy its middle, in parts of the picture's height). A ball with
 * stripes and rings, which rolls: `turn` is how far. Or a disc that slides, painted as things
 * in a game are, with blotches of its own and a rim (`plain`). Returns whether (u, v) is on it. */
static int ball(double u, double v, double cx, double cy, double radius, double turn, int plain, double rgb[3])
{
	const double dx = u - cx, dy = v - cy;
	if (dx * dx + dy * dy > radius * radius)
		return 0;
	const double ring = sqrt(dx * dx + dy * dy) / radius;
	if (plain)
	{
		const double g = 0.6 * grain(dx + 7.0, dy + 3.0, 40) + 0.4 * grain(dx + 1.0, dy + 9.0, 110);
		rgb[0] = (ring > 0.9) ? 0.15 : 0.55 + 0.45 * g;
		rgb[1] = (ring > 0.9) ? 0.1 : 0.15 + 0.5 * g * g;
		rgb[2] = (ring > 0.9) ? 0.1 : 0.1 + 0.2 * g;
		return 1;
	}
	const double angle = atan2(dy, dx) + turn;
	const int stripe = ((int)floor(angle * 6 / 3.14159265 + 100.0) + (int)floor(ring * 4)) & 1;
	rgb[0] = stripe ? 0.9 : 0.75;
	rgb[1] = stripe ? 0.2 : 0.7;
	rgb[2] = stripe ? 0.15 : 0.1;
	return 1;
}

struct shot
{
	const char* name;
	double pan_x, pan_y;   /* the camera's move from the older picture to the newer, in pixels */
	double zoom;           /* and how much nearer it is in the newer one (0.04: 4%) */
	double ball_x, ball_y; /* the ball's own move on the screen, in pixels; 0, 0 with no ball */
	int has_ball;
	int slides;            /* it is a disc that slides, not a ball that rolls */
	double fade;           /* how much darker the newer picture is (0.3: 30%) */
	int cut;               /* the newer picture is of another place altogether */
};

/* The shot at the moment `time` (0 the older picture, 1 the newer), w x h, 3 x 3 points a pixel. */
static void render(const struct shot* shot, double time, uint8_t* pixels, uint32_t w, uint32_t h)
{
	const double place = (shot->cut && time > 0.75) ? 40.0 : 0.0;
	for (uint32_t y = 0; y < h; y++)
	{
		for (uint32_t x = 0; x < w; x++)
		{
			double sum[3] = {0, 0, 0};
			for (int sy = 0; sy < 3; sy++)
			{
				for (int sx = 0; sx < 3; sx++)
				{
					const double px = x + (sx + 0.5) / 3, py = y + (sy + 0.5) / 3;
					double rgb[3];
					if (!shot->has_ball ||
						!ball(px / h, py / h, (0.45 * w + shot->ball_x * (time - 0.5)) / h, (0.55 * h + shot->ball_y * (time - 0.5)) / h, 0.11,
							time * shot->ball_x / (0.11 * h), shot->slides, rgb))
					{
						const double scale = 1.0 / (1.0 + shot->zoom * time);
						const double u = ((px - 0.5 * w) * scale + 0.5 * w + shot->pan_x * time) / h + place;
						const double v = ((py - 0.5 * h) * scale + 0.5 * h + shot->pan_y * time) / h + place * 0.37;
						land(u, v, rgb);
					}
					for (int c = 0; c < 3; c++)
						sum[c] += rgb[c] * (1.0 - shot->fade * time);
				}
			}
			uint8_t* const p = pixels + ((size_t)y * w + x) * 4;
			for (int c = 0; c < 3; c++)
				p[c] = (uint8_t)lrint(fmin(fmax(sum[c] / 9, 0), 1) * 255);
			p[3] = 255;
		}
	}
}

/* How near two pictures are (dB; 99 for the same). The outer `margin` pixels are left out: what
 * comes into the picture from outside it was in neither of the two pictures. */
static double psnr(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h, uint32_t margin)
{
	double sum = 0;
	size_t count = 0;
	for (uint32_t y = margin; y < h - margin; y++)
	{
		for (uint32_t x = margin; x < w - margin; x++)
		{
			for (int c = 0; c < 3; c++)
			{
				const double d = (double)a[((size_t)y * w + x) * 4 + c] - (double)b[((size_t)y * w + x) * 4 + c];
				sum += d * d;
				count++;
			}
		}
	}
	return (sum == 0) ? 99.0 : 10.0 * log10(255.0 * 255.0 * (double)count / sum);
}

static void write_ppm(const char* folder, const char* name, const uint8_t* pixels, uint32_t w, uint32_t h)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
	FILE* const fp = fopen(path, "wb");
	if (!fp)
		return;
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (size_t i = 0; i < (size_t)w * h; i++)
		fwrite(pixels + i * 4, 1, 3, fp);
	fclose(fp);
}

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		fprintf(stderr, "usage: fg_test <folder with the passes' .spv> <out folder | --compile-only>\n");
		return 2;
	}
	const char* const shaders = argv[1];
	const char* const out = argv[2];
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

	s_pass_small = make_pass(SMALL);
	s_pass_picture = make_pass(PICTURE);
	/* Four pictures a pass can read, as the title's utility passes have them since v0.9.0. */
	VkDescriptorSetLayoutBinding bindings[4];
	for (uint32_t i = 0; i < 4; i++)
		bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
	const VkDescriptorSetLayoutCreateInfo set_layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4,
		.pBindings = bindings};
	VK(vkCreateDescriptorSetLayout(s_device, &set_layout, NULL, &s_set_layout));
	const VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
	const VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &s_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
	VK(vkCreatePipelineLayout(s_device, &pipeline_layout, NULL, &s_layout));
	const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
	const VkDescriptorPoolCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64, .poolSizeCount = 1,
		.pPoolSizes = &pool_size};
	VK(vkCreateDescriptorPool(s_device, &descriptors, NULL, &s_descriptors));
	/* PCSX2's linear sampler for a utility pass: clamped at the picture's edge. */
	const VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
	VK(vkCreateSampler(s_device, &sampler, NULL, &s_sampler));

	const VkShaderModule vert = load_shader(shaders, "vert.spv");
	for (int pass = 0; pass < PASSES; pass++)
	{
		char name[32];
		snprintf(name, sizeof(name), "pass%d.spv", pass);
		const VkShaderModule frag = load_shader(shaders, name);
		/* (The last pass writes a picture, the others the small ones with room for a move.) */
		s_pipelines[pass] = make_pipeline(vert, frag, (pass == PASSES - 1) ? s_pass_picture : s_pass_small);
		vkDestroyShaderModule(s_device, frag, NULL);
	}
	if (compile_only)
	{
		printf("  all %d pipelines were made by: %s\n", PASSES, props.deviceName);
		return 0;
	}

	/* A picture as a 4x game's is shown on a 1080p screen, a third of a 4K one each way: what is
	 * 60 pixels here is 180 on a 4K screen. */
	const uint32_t w = 1280, h = 720;
	const size_t bytes = (size_t)w * h * 4;
	uint8_t* const older = malloc(bytes);
	uint8_t* const newer = malloc(bytes);
	uint8_t* const truth = malloc(bytes);
	uint8_t* const made = malloc(bytes);
	uint8_t* const mixed = malloc(bytes);

	const struct shot shots[] = {
		{.name = "a slow turn of the camera (12 pixels)", .pan_x = 12},
		{.name = "a turn (48 pixels)", .pan_x = 48, .pan_y = 6},
		{.name = "a fast turn (140 pixels)", .pan_x = 140, .pan_y = -10},
		{.name = "a whip of the camera (230 pixels)", .pan_x = 230},
		{.name = "looking up (36 pixels)", .pan_y = -36},
		{.name = "moving in (4%)", .zoom = 0.04},
		{.name = "a disc sliding across a still picture (70)", .has_ball = 1, .slides = 1, .ball_x = 70, .ball_y = -12},
		{.name = "a ball rolling across a still picture (70)", .has_ball = 1, .ball_x = 70, .ball_y = -12},
		{.name = "a disc one way, the camera the other", .has_ball = 1, .slides = 1, .ball_x = 60, .pan_x = -40},
		{.name = "a ball one way, the camera the other", .has_ball = 1, .ball_x = 60, .pan_x = -40},
		{.name = "the camera following a ball", .has_ball = 1, .pan_x = 90},
		{.name = "a turn while the picture fades", .pan_x = 30, .fade = 0.25},
	};
	printf("  %ux%u; against the picture as it would have been drawn at that moment:\n", w, h);
	double worst_gain = 99;
	for (size_t i = 0; i < sizeof(shots) / sizeof(shots[0]); i++)
	{
		const struct shot* const shot = &shots[i];
		render(shot, 0.0, older, w, h);
		render(shot, 1.0, newer, w, h);
		render(shot, 0.5, truth, w, h);
		double none = 0, wild = 0;
		between(older, newer, w, h, 0.5f, made, &none, &wild);
		for (size_t b = 0; b < bytes; b++)
			mixed[b] = (uint8_t)((older[b] + newer[b] + 1) / 2);
		/* (What came into the picture during the move is left out: half the camera's move.) */
		const uint32_t margin = 8 + (uint32_t)ceil(fmax(fabs(shot->pan_x), fabs(shot->pan_y)) * 0.5 + shot->zoom * w * 0.25);
		const double p_made = psnr(made, truth, w, h, margin), p_again = psnr(older, truth, w, h, margin),
					 p_mixed = psnr(mixed, truth, w, h, margin);
		printf("    %-44s made %5.2f dB, shown again %5.2f, mixed %5.2f  (no match in %2.0f%%, wild %2.0f%%)\n", shot->name, p_made, p_again,
			p_mixed, none * 100, wild * 100);
		/* A ball that rolls an eighth of a turn between two pictures on a picture that stands
		 * still: its pattern is nowhere in the other picture to be found, and what is made of it
		 * is no better than the two pictures mixed (the rest of the picture stands still, so there
		 * is nothing else to gain). It must not be worse than that, and that is all that is asked. */
		const int rolls_alone = (shot->has_ball && !shot->slides && shot->pan_x == 0 && shot->pan_y == 0 && shot->zoom == 0);
		CHECK(p_made > p_again + 1.0, "%s: %.2f against %.2f", shot->name, p_made, p_again);
		CHECK(p_made > p_mixed + (rolls_alone ? -0.5 : 1.0), "%s: %.2f against %.2f", shot->name, p_made, p_mixed);
		CHECK(none < 0.5 && wild < 0.35, "%s: taken for another scene (%.2f, %.2f)", shot->name, none, wild);
		if (!rolls_alone)
			worst_gain = fmin(worst_gain, p_made - fmax(p_again, p_mixed));
		if (i == 1 || i == 6 || i == 8 || getenv("FG_MOVES"))
		{
			char name[64];
			snprintf(name, sizeof(name), "shot%zu-made", i);
			write_ppm(out, name, made, w, h);
			snprintf(name, sizeof(name), "shot%zu-truth", i);
			write_ppm(out, name, truth, w, h);
			snprintf(name, sizeof(name), "shot%zu-mixed", i);
			write_ppm(out, name, mixed, w, h);
		}
	}
	printf("  the least it gained over the better of the two, the ball rolling alone left out: %.2f dB\n", worst_gain);

	/* Other moments than the middle: a game at 20 pictures a second on a 60 Hz screen. */
	{
		const struct shot shot = {.name = "a turn, a third of the way", .pan_x = 60, .pan_y = 9};
		render(&shot, 0.0, older, w, h);
		render(&shot, 1.0, newer, w, h);
		for (int third = 1; third <= 2; third++)
		{
			const double time = third / 3.0;
			render(&shot, time, truth, w, h);
			between(older, newer, w, h, (float)time, made, NULL, NULL);
			const double p_made = psnr(made, truth, w, h, 40), p_older = psnr(older, truth, w, h, 40), p_newer = psnr(newer, truth, w, h, 40);
			printf("    a turn (60 pixels) at %d/3 of the way:           made %5.2f dB, the older %5.2f, the newer %5.2f\n", third, p_made, p_older,
				p_newer);
			CHECK(p_made > fmax(p_older, p_newer) + 1.0, "%.2f against %.2f and %.2f", p_made, p_older, p_newer);
		}
	}

	/* A picture that stands still comes out as it went in. */
	{
		const struct shot shot = {.name = "still", .has_ball = 1};
		render(&shot, 0.0, older, w, h);
		double none = 0, wild = 0;
		between(older, older, w, h, 0.5f, made, &none, &wild);
		int worst = 0;
		for (size_t b = 0; b < bytes; b++)
			worst = abs((int)made[b] - (int)older[b]) > worst ? abs((int)made[b] - (int)older[b]) : worst;
		CHECK(worst <= 1, "a still picture is off by %d", worst);
		CHECK(none < 0.01, "a still picture found no match in %.2f", none);
	}

	/* After a cut to another scene the older picture stays: nothing is made up between the two. */
	{
		const struct shot shot = {.name = "cut", .cut = 1, .pan_x = 20};
		render(&shot, 0.0, older, w, h);
		render(&shot, 1.0, newer, w, h);
		double none = 0, wild = 0;
		between(older, newer, w, h, 0.5f, made, &none, &wild);
		size_t differing = 0;
		for (size_t b = 0; b < bytes; b++)
			differing += (made[b] != older[b]);
		printf("    a cut to another scene: no match in %.0f%% of the picture, %.0f%% of it wild; %zu values differ from the older picture\n",
			none * 100, wild * 100, differing);
		CHECK(none > 0.5 || wild > 0.35, "a cut was not seen (%.2f, %.2f)", none, wild);
		CHECK(differing == 0, "%zu values differ after a cut", differing);
	}

	printf("frame generation: %d checks, %d failed\n", s_checks, s_failed);
	free(older);
	free(newer);
	free(truth);
	free(made);
	free(mixed);
	return s_failed ? 1 : 0;
}

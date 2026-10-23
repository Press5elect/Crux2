// Crux2 - the graphics driver's threaded recording, with the commands PCSX2 sends it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// RADV for the PS5 can record an application's commands on a thread of its own
// (RADV_THREADED_RECORDING=1). ps5/patches/mesa/0002 makes it queue vkCmdPushDescriptorSet, which
// PCSX2's renderer calls before most draws, instead of waiting for that thread at each one.
//
// This runs on the PC against a PC build of the same driver sources, over Mesa's amdgpu "noop"
// shim (no AMD GPU is needed; nothing is executed, the commands are only recorded). It records
// frames shaped like the renderer's: a render pass on a target that is also read, textures and a
// constant buffer pushed as descriptors, pipelines switched, and runs of draws with a barrier
// before each (what accurate blending is on a driver without framebuffer fetch).
//
//   threaded_recording_test <libvulkan_radeon.so> <vert.spv> <frag.spv> check
//       one frame, submitted: with RADV_DEBUG=dumpibs the driver prints the recorded commands,
//       and run-driver-tests.sh compares that text with and without threaded recording
//   threaded_recording_test <libvulkan_radeon.so> <vert.spv> <frag.spv> time <frames> <draws>
//       records frames and prints what the recording cost the calling thread
//
// After every push the test overwrites the arrays it passed, so a queue that kept a pointer
// to them instead of a copy records rubbish (and the comparison fails).

#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(call) \
	do \
	{ \
		const VkResult check_result = (call); \
		if (check_result != VK_SUCCESS) \
		{ \
			fprintf(stderr, "FAILED %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)check_result); \
			exit(1); \
		} \
	} while (0)

static PFN_vkGetInstanceProcAddr s_instance_proc;
static PFN_vkGetDeviceProcAddr s_device_proc;
static VkInstance s_instance;
static VkDevice s_device;

#define INSTANCE_FN(name) PFN_vk##name name = (PFN_vk##name)s_instance_proc(s_instance, "vk" #name)
#define DEVICE_FN(name) PFN_vk##name name = (PFN_vk##name)s_device_proc(s_device, "vk" #name)

static double now_ms(clockid_t clock)
{
	struct timespec ts;
	clock_gettime(clock, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void* read_file(const char* path, size_t* size)
{
	FILE* file = fopen(path, "rb");
	if (!file)
	{
		fprintf(stderr, "cannot read %s\n", path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	*size = (size_t)ftell(file);
	fseek(file, 0, SEEK_SET);
	void* data = malloc(*size);
	if (fread(data, 1, *size, file) != *size)
		exit(1);
	fclose(file);
	return data;
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags wanted)
{
	INSTANCE_FN(GetPhysicalDeviceMemoryProperties);
	VkPhysicalDeviceMemoryProperties properties;
	GetPhysicalDeviceMemoryProperties(physical, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
	{
		if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	}
	fprintf(stderr, "no memory type\n");
	exit(1);
}

static VkImageView make_image(VkPhysicalDevice physical, uint32_t width, uint32_t height, VkImageUsageFlags usage,
	VkImage* image_out)
{
	DEVICE_FN(CreateImage);
	DEVICE_FN(GetImageMemoryRequirements);
	DEVICE_FN(AllocateMemory);
	DEVICE_FN(BindImageMemory);
	DEVICE_FN(CreateImageView);

	const VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {width, height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	VkImage image;
	CHECK(CreateImage(s_device, &image_info, NULL, &image));
	VkMemoryRequirements requirements;
	GetImageMemoryRequirements(s_device, image, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits, 0),
	};
	VkDeviceMemory memory;
	CHECK(AllocateMemory(s_device, &allocate, NULL, &memory));
	CHECK(BindImageMemory(s_device, image, memory, 0));
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	VkImageView view;
	CHECK(CreateImageView(s_device, &view_info, NULL, &view));
	if (image_out)
		*image_out = image;
	return view;
}

static VkBuffer make_buffer(VkPhysicalDevice physical, VkDeviceSize size, VkBufferUsageFlags usage)
{
	DEVICE_FN(CreateBuffer);
	DEVICE_FN(GetBufferMemoryRequirements);
	DEVICE_FN(AllocateMemory);
	DEVICE_FN(BindBufferMemory);

	const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
	VkBuffer buffer;
	CHECK(CreateBuffer(s_device, &info, NULL, &buffer));
	VkMemoryRequirements requirements;
	GetBufferMemoryRequirements(s_device, buffer, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits, 0),
	};
	VkDeviceMemory memory;
	CHECK(AllocateMemory(s_device, &allocate, NULL, &memory));
	CHECK(BindBufferMemory(s_device, buffer, memory, 0));
	return buffer;
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		fprintf(stderr, "usage: %s <libvulkan_radeon.so> <vert.spv> <frag.spv> check | time <frames> <draws>\n", argv[0]);
		return 2;
	}
	const int timing = (strcmp(argv[4], "time") == 0);
	const unsigned frames = (timing && argc > 5) ? (unsigned)atoi(argv[5]) : 1;
	const unsigned draws = (timing && argc > 6) ? (unsigned)atoi(argv[6]) : 96;

	void* library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
	if (!library)
	{
		fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror());
		return 1;
	}
	s_instance_proc = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
	if (!s_instance_proc)
	{
		fprintf(stderr, "no vk_icdGetInstanceProcAddr in %s\n", argv[1]);
		return 1;
	}

	// ---- the instance and the device
	PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)s_instance_proc(NULL, "vkCreateInstance");
	const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
	const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &application};
	CHECK(CreateInstance(&instance_info, NULL, &s_instance));

	INSTANCE_FN(EnumeratePhysicalDevices);
	INSTANCE_FN(GetPhysicalDeviceProperties);
	INSTANCE_FN(CreateDevice);
	uint32_t physical_count = 1;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	const VkResult enumerated = EnumeratePhysicalDevices(s_instance, &physical_count, &physical);
	if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || physical_count == 0)
	{
		fprintf(stderr, "no device (is the amdgpu noop shim preloaded?)\n");
		return 1;
	}
	VkPhysicalDeviceProperties properties;
	GetPhysicalDeviceProperties(physical, &properties);

	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0,
		.queueCount = 1,
		.pQueuePriorities = &priority};
	const char* extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback_loop = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
		.attachmentFeedbackLoopLayout = VK_TRUE,
	};
	const VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &feedback_loop,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = extensions,
	};
	CHECK(CreateDevice(physical, &device_info, NULL, &s_device));
	s_device_proc = (PFN_vkGetDeviceProcAddr)s_instance_proc(s_instance, "vkGetDeviceProcAddr");

	DEVICE_FN(GetDeviceQueue);
	DEVICE_FN(CreateSampler);
	DEVICE_FN(CreateRenderPass);
	DEVICE_FN(CreateFramebuffer);
	DEVICE_FN(CreateDescriptorSetLayout);
	DEVICE_FN(CreatePipelineLayout);
	DEVICE_FN(CreateShaderModule);
	DEVICE_FN(CreateGraphicsPipelines);
	DEVICE_FN(CreateCommandPool);
	DEVICE_FN(AllocateCommandBuffers);
	DEVICE_FN(BeginCommandBuffer);
	DEVICE_FN(EndCommandBuffer);
	DEVICE_FN(ResetCommandBuffer);
	DEVICE_FN(CmdBeginRenderPass);
	DEVICE_FN(CmdEndRenderPass);
	DEVICE_FN(CmdBindPipeline);
	DEVICE_FN(CmdSetViewport);
	DEVICE_FN(CmdSetScissor);
	DEVICE_FN(CmdBindVertexBuffers);
	DEVICE_FN(CmdBindIndexBuffer);
	DEVICE_FN(CmdPushConstants);
	DEVICE_FN(CmdPushDescriptorSetKHR);
	DEVICE_FN(CmdPipelineBarrier);
	DEVICE_FN(CmdDrawIndexed);
	DEVICE_FN(QueueSubmit);
	DEVICE_FN(DeviceWaitIdle);
	DEVICE_FN(DestroyDevice);

	VkQueue queue;
	GetDeviceQueue(s_device, 0, 0, &queue);

	// ---- what a frame draws with
	const uint32_t width = 640, height = 448;
	VkImage target_image;
	const VkImageView target = make_image(physical, width, height,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT,
		&target_image);
	VkImageView textures[4];
	for (int i = 0; i < 4; i++)
		textures[i] = make_image(physical, 64u << i, 64, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
	const VkBuffer vertices = make_buffer(physical, 1 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	const VkBuffer indices = make_buffer(physical, 1 << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	const VkBuffer constants = make_buffer(physical, 1 << 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	const VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_NEAREST};
	VkSampler sampler;
	CHECK(CreateSampler(s_device, &sampler_info, NULL, &sampler));

	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
		.finalLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
	};
	const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT};
	const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color};
	// The subpass depends on itself: a draw reads what an earlier draw of the pass wrote.
	const VkSubpassDependency dependency = {
		.srcSubpass = 0,
		.dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT,
	};
	const VkRenderPassCreateInfo pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 1,
		.pDependencies = &dependency,
	};
	VkRenderPass pass;
	CHECK(CreateRenderPass(s_device, &pass_info, NULL, &pass));
	const VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = pass,
		.attachmentCount = 1,
		.pAttachments = &target,
		.width = width,
		.height = height,
		.layers = 1,
	};
	VkFramebuffer framebuffer;
	CHECK(CreateFramebuffer(s_device, &framebuffer_info, NULL, &framebuffer));

	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
		{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
	};
	const VkDescriptorSetLayoutCreateInfo set_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
		.bindingCount = 3,
		.pBindings = bindings,
	};
	VkDescriptorSetLayout set_layout;
	CHECK(CreateDescriptorSetLayout(s_device, &set_layout_info, NULL, &set_layout));
	const VkPushConstantRange push_range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	VkPipelineLayout layout;
	CHECK(CreatePipelineLayout(s_device, &layout_info, NULL, &layout));

	VkShaderModule modules[2];
	for (int i = 0; i < 2; i++)
	{
		size_t size;
		void* code = read_file(argv[2 + i], &size);
		const VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = size,
			.pCode = code};
		CHECK(CreateShaderModule(s_device, &module_info, NULL, &modules[i]));
		free(code);
	}

	// Two pipelines that differ in blending, as a renderer's do from draw to draw.
	VkPipeline pipelines[2];
	for (int i = 0; i < 2; i++)
	{
		const VkPipelineShaderStageCreateInfo stages[] = {
			{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = modules[0],
				.pName = "main"},
			{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = modules[1],
				.pName = "main"},
		};
		const VkVertexInputBindingDescription vertex_binding = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
		const VkVertexInputAttributeDescription attributes[] = {
			{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
			{1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
		};
		const VkPipelineVertexInputStateCreateInfo vertex_input = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &vertex_binding,
			.vertexAttributeDescriptionCount = 2,
			.pVertexAttributeDescriptions = attributes,
		};
		const VkPipelineInputAssemblyStateCreateInfo assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		const VkPipelineViewportStateCreateInfo viewport = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		};
		const VkPipelineRasterizationStateCreateInfo rasterization = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.lineWidth = 1.0f,
		};
		const VkPipelineMultisampleStateCreateInfo multisample = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		const VkPipelineColorBlendAttachmentState blend_attachment = {
			.blendEnable = (i == 1),
			.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
			.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			.colorBlendOp = VK_BLEND_OP_ADD,
			.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
			.alphaBlendOp = VK_BLEND_OP_ADD,
			.colorWriteMask = 0xf,
		};
		const VkPipelineColorBlendStateCreateInfo blend = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &blend_attachment,
		};
		const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		const VkPipelineDynamicStateCreateInfo dynamic = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2,
			.pDynamicStates = dynamic_states,
		};
		const VkGraphicsPipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.flags = VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT,
			.stageCount = 2,
			.pStages = stages,
			.pVertexInputState = &vertex_input,
			.pInputAssemblyState = &assembly,
			.pViewportState = &viewport,
			.pRasterizationState = &rasterization,
			.pMultisampleState = &multisample,
			.pColorBlendState = &blend,
			.pDynamicState = &dynamic,
			.layout = layout,
			.renderPass = pass,
		};
		CHECK(CreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipelines[i]));
	}

	const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
	VkCommandPool pool;
	CHECK(CreateCommandPool(s_device, &pool_info, NULL, &pool));
	const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1};
	VkCommandBuffer commands;
	CHECK(AllocateCommandBuffers(s_device, &command_info, &commands));

	// ---- the frames
	// The barrier PCSX2's renderer puts before a draw that reads its target
	// (GSDeviceVK::GetColorBufferFeedbackBarrier).
	const VkImageMemoryBarrier feedback_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
		.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = target_image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};

	unsigned long long pushes = 0, barriers = 0, draw_calls = 0;
	const double wall_start = now_ms(CLOCK_MONOTONIC), cpu_start = now_ms(CLOCK_THREAD_CPUTIME_ID);
	for (unsigned frame = 0; frame < frames; frame++)
	{
		if (frame != 0)
			CHECK(ResetCommandBuffer(commands, 0));
		const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		CHECK(BeginCommandBuffer(commands, &begin));

		const VkRenderPassBeginInfo pass_begin = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = pass,
			.framebuffer = framebuffer,
			.renderArea = {{0, 0}, {width, height}},
		};
		CmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
		const VkViewport viewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
		CmdSetViewport(commands, 0, 1, &viewport);
		const VkDeviceSize zero = 0;
		CmdBindVertexBuffers(commands, 0, 1, &vertices, &zero);
		CmdBindIndexBuffer(commands, indices, 0, VK_INDEX_TYPE_UINT16);
		CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);

		for (unsigned draw = 0; draw < draws; draw++)
		{
			// Most draws come with another texture: the texture, the target as a texture, and
			// every fourth time the constants, in arrays that are overwritten once pushed.
			if (draw % 3 != 2)
			{
				VkDescriptorImageInfo images[2] = {
					{sampler, textures[(draw + frame) % 4], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
					{sampler, target, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT},
				};
				VkDescriptorBufferInfo buffer = {constants, (draw % 16) * 256, 256};
				VkWriteDescriptorSet writes[3] = {
					{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						.dstBinding = 0,
						.descriptorCount = 1,
						.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
						.pImageInfo = &images[0],
						// Not read for this type: anything may be in them.
						.pBufferInfo = (const VkDescriptorBufferInfo*)(uintptr_t)0x10,
						.pTexelBufferView = (const VkBufferView*)(uintptr_t)0x20},
					{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						.dstBinding = 1,
						.descriptorCount = 1,
						.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
						.pImageInfo = &images[1]},
					{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						.dstBinding = 2,
						.descriptorCount = 1,
						.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
						.pImageInfo = (const VkDescriptorImageInfo*)(uintptr_t)0x30,
						.pBufferInfo = &buffer},
				};
				CmdPushDescriptorSetKHR(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (draw % 4 == 0) ? 3 : 2, writes);
				memset(images, 0xAA, sizeof(images));
				memset(&buffer, 0xAA, sizeof(buffer));
				memset(writes, 0xAA, sizeof(writes));
				pushes++;
			}
			if (draw % 5 == 0)
				CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[(draw / 5) & 1]);

			float offset[4] = {(float)(draw % 7) * 0.01f, (float)(frame % 3) * 0.01f, 0.0f, 0.0f};
			CmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(offset), offset);
			memset(offset, 0xAA, sizeof(offset));
			const VkRect2D scissor = {{(int32_t)(draw % 16), 0}, {width - 16, height}};
			CmdSetScissor(commands, 0, 1, &scissor);

			// Every other draw is one that reads the target: each of its pieces is drawn after
			// a barrier (GSDeviceVK::SendHWDraw with a full barrier).
			const unsigned pieces = (draw % 2 == 0) ? 1 : 8 + (draw % 24);
			for (unsigned piece = 0; piece < pieces; piece++)
			{
				if (pieces > 1)
				{
					VkImageMemoryBarrier barrier = feedback_barrier;
					CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT, 0, NULL, 0, NULL, 1, &barrier);
					memset(&barrier, 0xAA, sizeof(barrier));
					barriers++;
				}
				CmdDrawIndexed(commands, 6, 1, piece * 6, 0, 0);
				draw_calls++;
			}
		}

		CmdEndRenderPass(commands);
		CHECK(EndCommandBuffer(commands));

		if (!timing)
		{
			const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &commands};
			CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
		}
	}
	const double wall = now_ms(CLOCK_MONOTONIC) - wall_start, cpu = now_ms(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
	CHECK(DeviceWaitIdle(s_device));

	const char* threaded = getenv("RADV_THREADED_RECORDING");
	printf("%s, threaded recording %s: %u frame(s), %llu draw calls, %llu barriers, %llu pushes\n", properties.deviceName,
		(threaded && atoi(threaded)) ? "on" : "off", frames, draw_calls, barriers, pushes);
	if (timing)
	{
		printf("  a frame: %.3f ms on the clock, %.3f ms of the calling thread's processor time (%.0f ns a draw call)\n",
			wall / frames, cpu / frames, cpu * 1e6 / (double)draw_calls);
	}

	// The destroys go through the layer too: each first waits for the worker.
	DEVICE_FN(DestroyCommandPool);
	DEVICE_FN(DestroyPipeline);
	DEVICE_FN(DestroyShaderModule);
	DEVICE_FN(DestroyPipelineLayout);
	DEVICE_FN(DestroyDescriptorSetLayout);
	DEVICE_FN(DestroyFramebuffer);
	DEVICE_FN(DestroyRenderPass);
	DEVICE_FN(DestroySampler);
	DestroyCommandPool(s_device, pool, NULL);
	for (int i = 0; i < 2; i++)
	{
		DestroyPipeline(s_device, pipelines[i], NULL);
		DestroyShaderModule(s_device, modules[i], NULL);
	}
	DestroyPipelineLayout(s_device, layout, NULL);
	DestroyDescriptorSetLayout(s_device, set_layout, NULL);
	DestroyFramebuffer(s_device, framebuffer, NULL);
	DestroyRenderPass(s_device, pass, NULL);
	DestroySampler(s_device, sampler, NULL);
	// (The images, buffers and their memory are left to the end of the process.)
	DestroyDevice(s_device, NULL);
	return 0;
}

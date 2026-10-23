// Crux2 - FSR 3's own frame interpolation passes on Crux2's optical flow, for comparison (tests only).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Route 1 of the two ways to make the picture between two pictures from the FSR 3 optical flow:
// AMD's frame interpolation (FidelityFX SDK 2.3.0's ffx_frameinterpolation.cpp, MIT: the passes
// of its "prepare" and "dispatch", in its order, with its sizes and constants), fed what a PS2
// game can give it: no motion vectors (zero), no depth (a constant), and the flow of the module
// (OpticalFlowFG's own FSR 3 flow, the newer picture's blocks, as FSR 3 frame generation feeds
// it). The shaders are AMD's GLSL entry points (FidelityFX SDK 1.1.4, fsr3fi/, unchanged) over
// the vendored 2.3.0 pass headers; run-computefx-tests.sh compiles them. Route 2 is the module's
// own composite. This is here to measure route 1, not to ship it: it always makes the middle
// picture (FSR 3 interpolates at t = 0.5 only), needs storage formats and unformatted image
// loads PCSX2 does not turn on, and ten passes of its own besides the flow.
//
// Included by computefx_test.cpp after its helpers (one translation unit).

#pragma once

namespace Fsr3FiRoute
{
	// The passes, their files (fsr3fi/<name>.glsl -> <name>.spv) and their bindings: binding number and
	// kind (S sampled image, I storage image, U uniform buffer, B storage buffer, X sampler), as the
	// SDK's GLSL entry points and ffx_frameinterpolation_callbacks_glsl.h declare them.
	struct PassInfo
	{
		const char* name;
		const char* bindings;
	};
	enum Pass
	{
		kReconstructAndDilate,
		kSetup,
		kReconstructPreviousDepth,
		kGameMotionVectorField,
		kGameVectorFieldPyramid,
		kOpticalFlowVectorField,
		kDisocclusionMask,
		kInterpolate,
		kInpaintingPyramid,
		kInpainting,
		kPassCount
	};
	static const PassInfo kPasses[kPassCount] = {
		{"ffx_frameinterpolation_reconstruct_and_dilate_pass", "0S 1S 2I 3I 4I 5U 1000X"},
		{"ffx_frameinterpolation_setup_pass", "0S 1I 2I 3I 4I 5I 6B 7U 1000X"},
		{"ffx_frameinterpolation_reconstruct_previous_depth_pass", "0S 1S 2S 3S 4I 5U 1000X"},
		{"ffx_frameinterpolation_game_motion_vector_field_pass", "0S 1S 2S 3S 4S 5I 6I 7U 1000X"},
		{"ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass",
			"0S 1S 2B 3I 4I 5I 6I 7I 8I 9I 10I 11I 12I 13I 14I 15I 16U 17U 1000X"},
		{"ffx_frameinterpolation_optical_flow_vector_field_pass", "0S 1S 2S 3S 4S 6I 7I 8U 1000X"},
		{"ffx_frameinterpolation_disocclusion_mask_pass", "0S 1S 2S 3S 4S 5S 6S 7I 8U 1000X"},
		{"ffx_frameinterpolation_pass", "0S 1S 2S 3S 4S 5S 6S 7S 8B 9I 10U 1000X"},
		{"ffx_frameinterpolation_compute_inpainting_pyramid_pass", "0S 1B 2I 3I 4I 5I 6I 7I 8I 9I 10I 11I 12I 13I 14I 15U 16U 1000X"},
		{"ffx_frameinterpolation_inpainting_pass", "0S 1S 2S 3S 7I 8U 1000X"},
	};

	// AMD's FrameInterpolationConstants (ffx_frameinterpolation_private.h), std140 as cbFI.
	struct Constants
	{
		int32_t renderSize[2];
		int32_t displaySize[2];
		float displaySizeRcp[2];
		float cameraNear;
		float cameraFar;
		int32_t upscalerTargetSize[2];
		int32_t mode;
		int32_t reset;
		float deviceToViewDepth[4];
		float deltaTime;
		int32_t hudLessAttachedFactor;
		int32_t distortionFieldSize[2];
		float opticalFlowScale[2];
		int32_t opticalFlowBlockSize;
		uint32_t dispatchFlags;
		int32_t maxRenderSize[2];
		int32_t opticalFlowHalfResMode;
		int32_t numInstances;
		int32_t interpolationRectBase[2];
		int32_t interpolationRectSize[2];
		float debugBarColor[3];
		uint32_t backBufferTransferFunction;
		float minMaxLuminance[2];
		float tanHalfFov;
		float pad1;
		float jitter[2];
		float motionVectorScale[2];
	};
	static_assert(sizeof(Constants) == 176);

	struct PyramidConstants // cbInpaintingPyramid
	{
		uint32_t mips, numWorkGroups, workGroupOffset[2];
	};

	struct Binding
	{
		uint32_t number;
		VkDescriptorType type;
	};

	static std::vector<Binding> parse(const char* spec)
	{
		std::vector<Binding> out;
		for (const char* c = spec; *c;)
		{
			char* end;
			const uint32_t number = (uint32_t)std::strtoul(c, &end, 10);
			const char kind = *end;
			out.push_back({number, kind == 'S' ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
								   kind == 'I' ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
								   kind == 'U' ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
								   kind == 'B' ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
												 VK_DESCRIPTOR_TYPE_SAMPLER});
			c = end + 1;
			while (*c == ' ')
				c++;
		}
		return out;
	}

	// A resource bound to a binding: an image view, a buffer, or the sampler.
	struct Res
	{
		VkImageView view = VK_NULL_HANDLE;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize range = VK_WHOLE_SIZE;
	};

	struct State
	{
		bool ready = false, failed = false;
		VkDescriptorSetLayout layouts[kPassCount] = {};
		VkPipelineLayout pipelineLayouts[kPassCount] = {};
		VkPipeline pipelines[kPassCount] = {};
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		TestImage inputMotion, inputDepth, distortion, dilatedMotion, dilatedDepth, previousDepth, interpolatedDepth, gameX, gameY, flowX, flowY,
			disocclusion, output, sceneChange, flowConfidence;
		VkImage pyramid = VK_NULL_HANDLE;
		VkDeviceMemory pyramidMemory = VK_NULL_HANDLE;
		VkImageView pyramidAll = VK_NULL_HANDLE, pyramidMips[13] = {};
		uint32_t pyramidLevels = 0;
		Buffer counters, constants, gamePyramid, colourPyramid;
	};
	static State s;

	static VkShaderModule load(const char* folder, const char* name)
	{
		const std::string path = std::string(folder) + "/" + name + ".spv";
		FILE* const fp = std::fopen(path.c_str(), "rb");
		if (!fp)
			return VK_NULL_HANDLE;
		std::fseek(fp, 0, SEEK_END);
		const long size = std::ftell(fp);
		std::fseek(fp, 0, SEEK_SET);
		std::vector<uint32_t> code((size_t)size / 4);
		const size_t got = std::fread(code.data(), 1, (size_t)size, fp);
		std::fclose(fp);
		if (got != (size_t)size)
			return VK_NULL_HANDLE;
		VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		info.codeSize = (size_t)size;
		info.pCode = code.data();
		VkShaderModule module = VK_NULL_HANDLE;
		vkCreateShaderModule(s_device, &info, nullptr, &module);
		return module;
	}

	// AMD's setupDeviceDepthToViewSpaceDepthParams for a standard (not inverted, not infinite) depth.
	static void depthParams(Constants& c, float fovY)
	{
		const float fMin = c.cameraNear, fMax = c.cameraFar;
		const float fQ = fMax / (fMin - fMax);
		c.deviceToViewDepth[0] = -1.0f * fQ;
		c.deviceToViewDepth[1] = fQ * fMin;
		const float aspect = (float)c.renderSize[0] / (float)c.renderSize[1];
		const float cotHalfFovY = std::cos(0.5f * fovY) / std::sin(0.5f * fovY);
		c.deviceToViewDepth[2] = 1.0f / (cotHalfFovY / aspect);
		c.deviceToViewDepth[3] = 1.0f / cotHalfFovY;
	}

	static void spdSetup(uint32_t w, uint32_t h, uint32_t groups[2], PyramidConstants& p)
	{
		// ffxSpdSetup over the whole picture, mips from its size.
		groups[0] = (w - 1) / 64 + 1;
		groups[1] = (h - 1) / 64 + 1;
		p.workGroupOffset[0] = p.workGroupOffset[1] = 0;
		p.numWorkGroups = groups[0] * groups[1];
		p.mips = (uint32_t)std::fmin(std::floor(std::log2((double)std::max(w, h))), 12.0);
	}

	static bool init(const char* folder, uint32_t w, uint32_t h)
	{
		for (int p = 0; p < kPassCount; p++)
		{
			const std::vector<Binding> binds = parse(kPasses[p].bindings);
			std::vector<VkDescriptorSetLayoutBinding> lb;
			for (const Binding& b : binds)
				lb.push_back({b.number, b.type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
			VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			li.bindingCount = (uint32_t)lb.size();
			li.pBindings = lb.data();
			VK(vkCreateDescriptorSetLayout(s_device, &li, nullptr, &s.layouts[p]));
			VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pl.setLayoutCount = 1;
			pl.pSetLayouts = &s.layouts[p];
			VK(vkCreatePipelineLayout(s_device, &pl, nullptr, &s.pipelineLayouts[p]));
			const VkShaderModule module = load(folder, kPasses[p].name);
			if (!module)
			{
				std::printf("  (FSR 3's frame interpolation: no %s/%s.spv)\n", folder, kPasses[p].name);
				return false;
			}
			VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			ci.stage.module = module;
			ci.stage.pName = "main";
			ci.layout = s.pipelineLayouts[p];
			const VkResult r = vkCreateComputePipelines(s_device, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipelines[p]);
			vkDestroyShaderModule(s_device, module, nullptr);
			if (r != VK_SUCCESS)
				return false;
		}
		const VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16}};
		VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pi.maxSets = 16;
		pi.poolSizeCount = 5;
		pi.pPoolSizes = sizes;
		VK(vkCreateDescriptorPool(s_device, &pi, nullptr, &s.pool));
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		VK(vkCreateSampler(s_device, &si, nullptr, &s.sampler));

		const VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		s.inputMotion = make_image(w, h, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
		s.inputDepth = make_image(w, h, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
		s.distortion = make_image(1, 1, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
		s.dilatedMotion = make_image(w, h, VK_FORMAT_R16G16_SFLOAT, rw);
		s.dilatedDepth = make_image(w, h, VK_FORMAT_R32_SFLOAT, rw);
		s.previousDepth = make_image(w, h, VK_FORMAT_R32_UINT, rw);
		s.interpolatedDepth = make_image(w, h, VK_FORMAT_R32_UINT, rw);
		s.gameX = make_image(w, h, VK_FORMAT_R32_UINT, rw);
		s.gameY = make_image(w, h, VK_FORMAT_R32_UINT, rw);
		s.flowX = make_image(w, h, VK_FORMAT_R32_UINT, rw);
		s.flowY = make_image(w, h, VK_FORMAT_R32_UINT, rw);
		s.disocclusion = make_image(w, h, VK_FORMAT_R8G8_UNORM, rw);
		s.output = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, rw);
		s.sceneChange = make_image(3, 1, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_SAMPLED_BIT);
		s.flowConfidence = make_image((w + 7) / 8, (h + 7) / 8, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_SAMPLED_BIT);

		// The inpainting pyramid: half the picture, all its mips (AMD's 13 bindings: the ones past
		// the last mip get the last mip).
		s.pyramidLevels = (uint32_t)std::floor(std::log2((double)std::max(w / 2, h / 2))) + 1;
		VkImageCreateInfo pyr = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		pyr.imageType = VK_IMAGE_TYPE_2D;
		pyr.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		pyr.extent = {w / 2, h / 2, 1};
		pyr.mipLevels = s.pyramidLevels;
		pyr.arrayLayers = 1;
		pyr.samples = VK_SAMPLE_COUNT_1_BIT;
		pyr.tiling = VK_IMAGE_TILING_OPTIMAL;
		pyr.usage = rw;
		VK(vkCreateImage(s_device, &pyr, nullptr, &s.pyramid));
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(s_device, s.pyramid, &req);
		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK(vkAllocateMemory(s_device, &alloc, nullptr, &s.pyramidMemory));
		VK(vkBindImageMemory(s_device, s.pyramid, s.pyramidMemory, 0));
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = s.pyramid;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, s.pyramidLevels, 0, 1};
		VK(vkCreateImageView(s_device, &vi, nullptr, &s.pyramidAll));
		for (uint32_t m = 0; m < 13; m++)
		{
			vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, std::min(m, s.pyramidLevels - 1), 1, 0, 1};
			VK(vkCreateImageView(s_device, &vi, nullptr, &s.pyramidMips[m]));
		}
		const VkBufferUsageFlags bound = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		s.counters = make_buffer(16, bound);
		s.constants = make_buffer(sizeof(Constants), bound);
		s.gamePyramid = make_buffer(sizeof(PyramidConstants), bound);
		s.colourPyramid = make_buffer(sizeof(PyramidConstants), bound);
		VkCommandBuffer cmd = begin();
		for (TestImage* image : {&s.inputMotion, &s.inputDepth, &s.distortion, &s.dilatedMotion, &s.dilatedDepth, &s.previousDepth,
				 &s.interpolatedDepth, &s.gameX, &s.gameY, &s.flowX, &s.flowY, &s.disocclusion, &s.output, &s.sceneChange, &s.flowConfidence})
			layout(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = s.pyramid;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, s.pyramidLevels, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		finish(cmd);
		return true;
	}

	static VkDescriptorSet makeSet(Pass pass, const std::vector<Res>& res)
	{
		const std::vector<Binding> binds = parse(kPasses[pass].bindings);
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = s.pool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &s.layouts[pass];
		VkDescriptorSet set;
		VK(vkAllocateDescriptorSets(s_device, &ai, &set));
		std::vector<VkDescriptorImageInfo> images(binds.size());
		std::vector<VkDescriptorBufferInfo> buffers(binds.size());
		std::vector<VkWriteDescriptorSet> writes(binds.size());
		size_t r = 0;
		for (size_t i = 0; i < binds.size(); i++)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = set;
			writes[i].dstBinding = binds[i].number;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = binds[i].type;
			if (binds[i].type == VK_DESCRIPTOR_TYPE_SAMPLER)
			{
				images[i] = {s.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
				writes[i].pImageInfo = &images[i];
				continue;
			}
			const Res& item = res[r++];
			if (binds[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || binds[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			{
				buffers[i] = {item.buffer, 0, item.range};
				writes[i].pBufferInfo = &buffers[i];
			}
			else
			{
				images[i] = {VK_NULL_HANDLE, item.view, VK_IMAGE_LAYOUT_GENERAL};
				writes[i].pImageInfo = &images[i];
			}
		}
		vkUpdateDescriptorSets(s_device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
		return set;
	}

	static void dispatch(VkCommandBuffer cmd, Pass pass, VkDescriptorSet set, uint32_t x, uint32_t y)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelines[pass]);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayouts[pass], 0, 1, &set, 0, nullptr);
		vkCmdDispatch(cmd, x, y, 1);
		VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
			&barrier, 0, nullptr, 0, nullptr);
	}

	// The flow as FSR 3's "game" motion vectors: the module's refined field (the newer picture's
	// blocks: where each was in the older picture, in pixels, which is FSR 3's sense of a motion
	// vector), read between block middles for every pixel, as RG16F.
	static void flowAsGameVectors(const OpticalFlowFG& fg, uint32_t w, uint32_t h)
	{
		const uint32_t fw = fg.FieldWidth(), fh = fg.FieldHeight();
		TestImage field;
		field.image = fg.DebugImage(2);
		field.width = fw;
		field.height = fh;
		std::vector<uint16_t> raw((size_t)fw * fh * 4);
		download(field, VK_IMAGE_LAYOUT_GENERAL, raw.data(), raw.size() * 2);
		std::vector<uint16_t> dense((size_t)w * h * 2);
		const float bx = 8.0f * w / fg.FlowWidth(), by = 8.0f * h / fg.FlowHeight();
		for (uint32_t y = 0; y < h; y++)
		{
			const float fy = std::fmin(std::fmax((y + 0.5f) / by - 0.5f, 0.0f), fh - 1.0f);
			const uint32_t y0 = (uint32_t)fy, y1 = std::min(y0 + 1, fh - 1);
			for (uint32_t x = 0; x < w; x++)
			{
				const float fx = std::fmin(std::fmax((x + 0.5f) / bx - 0.5f, 0.0f), fw - 1.0f);
				const uint32_t x0 = (uint32_t)fx, x1 = std::min(x0 + 1, fw - 1);
				for (int c = 0; c < 2; c++)
				{
					auto at = [&](uint32_t xx, uint32_t yy) { return half_to_float(raw[((size_t)yy * fw + xx) * 4 + c]); };
					const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * (fx - x0);
					const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * (fx - x0);
					dense[((size_t)y * w + x) * 2 + c] = float_to_half(top + (bottom - top) * (fy - y0));
				}
			}
		}
		upload(s.inputMotion, dense.data(), dense.size() * 2, VK_IMAGE_LAYOUT_GENERAL);
	}

	// The picture FSR 3 makes between `older` and `newer` (RGBA8, sampled, in
	// SHADER_READ_ONLY_OPTIMAL) from the flow `fg` found for them in its last record; with
	// `gameVectors`, the flow is also given as the game's motion vectors.
	bool Run(const char* folder, const OpticalFlowFG& fg, const TestImage& older, const TestImage& newer, uint8_t* out, bool gameVectors)
	{
		const uint32_t w = older.width, h = older.height;
		if (s.failed)
			return false;
		if (!s.ready)
		{
			if (!init(folder, w, h))
			{
				s.failed = true;
				return false;
			}
			s.ready = true;
		}
		if (fg.FlowWidth() != w || fg.FlowHeight() != h)
			return false; // (FSR 3 runs its flow on the picture itself: the module's best quality at 720p)
		VK(vkResetDescriptorPool(s_device, s.pool, 0));
		if (gameVectors)
			flowAsGameVectors(fg, w, h);

		Constants c = {};
		c.renderSize[0] = c.displaySize[0] = c.upscalerTargetSize[0] = c.maxRenderSize[0] = c.interpolationRectSize[0] = (int32_t)w;
		c.renderSize[1] = c.displaySize[1] = c.upscalerTargetSize[1] = c.maxRenderSize[1] = c.interpolationRectSize[1] = (int32_t)h;
		c.displaySizeRcp[0] = 1.0f / w;
		c.displaySizeRcp[1] = 1.0f / h;
		c.cameraNear = 0.1f;
		c.cameraFar = 1000.0f;
		c.deltaTime = 1000.0f / 60.0f;
		c.distortionFieldSize[0] = c.distortionFieldSize[1] = 1;
		// The flow at the picture's size, a vector a block of 8 (as FSR 3 frame generation passes it).
		c.opticalFlowScale[0] = 1.0f / w;
		c.opticalFlowScale[1] = 1.0f / h;
		c.opticalFlowBlockSize = 8;
		c.numInstances = 1;
		c.minMaxLuminance[1] = 1.0f;
		const float fovY = 1.0f;
		c.tanHalfFov = std::tan(std::atan(std::tan(fovY / 2) * (float)w / h));
		c.motionVectorScale[0] = 1.0f / w;
		c.motionVectorScale[1] = 1.0f / h;
		depthParams(c, fovY);
		std::memcpy(s.constants.data, &c, sizeof(c));
		uint32_t gameGroups[2], colourGroups[2];
		PyramidConstants pyramid;
		spdSetup(w, h, gameGroups, pyramid);
		std::memcpy(s.gamePyramid.data, &pyramid, sizeof(pyramid));
		spdSetup(w, h, colourGroups, pyramid);
		std::memcpy(s.colourPyramid.data, &pyramid, sizeof(pyramid));

		const Res cb = {VK_NULL_HANDLE, s.constants.buffer}, counters = {VK_NULL_HANDLE, s.counters.buffer},
				  gameCb = {VK_NULL_HANDLE, s.gamePyramid.buffer}, colourCb = {VK_NULL_HANDLE, s.colourPyramid.buffer};
		auto V = [](VkImageView view) { return Res{view}; };
		std::vector<Res> mips;
		for (uint32_t m = 0; m < 13; m++)
			mips.push_back(V(s.pyramidMips[m]));

		const VkDescriptorSet sets[kPassCount] = {
			makeSet(kReconstructAndDilate, {V(s.inputMotion.view), V(s.inputDepth.view), V(s.previousDepth.view), V(s.dilatedMotion.view),
											   V(s.dilatedDepth.view), cb}),
			makeSet(kSetup, {V(s.sceneChange.view), V(s.gameX.view), V(s.gameY.view), V(s.flowX.view), V(s.flowY.view), V(s.disocclusion.view),
								counters, cb}),
			makeSet(kReconstructPreviousDepth, {V(s.dilatedMotion.view), V(s.dilatedDepth.view), V(newer.view), V(s.distortion.view),
												   V(s.interpolatedDepth.view), cb}),
			makeSet(kGameMotionVectorField, {V(s.dilatedMotion.view), V(s.dilatedDepth.view), V(older.view), V(newer.view), V(s.distortion.view),
												V(s.gameX.view), V(s.gameY.view), cb}),
			[&] {
				std::vector<Res> r = {V(s.gameX.view), V(s.gameY.view), counters};
				r.insert(r.end(), mips.begin(), mips.end());
				r.push_back(cb);
				r.push_back(gameCb);
				return makeSet(kGameVectorFieldPyramid, r);
			}(),
			makeSet(kOpticalFlowVectorField, {V(fg.DebugView(0)), V(s.flowConfidence.view), V(s.dilatedDepth.view), V(older.view), V(newer.view),
												 V(s.flowX.view), V(s.flowY.view), cb}),
			makeSet(kDisocclusionMask, {V(s.gameX.view), V(s.gameY.view), V(s.previousDepth.view), V(s.dilatedDepth.view), V(s.interpolatedDepth.view),
										   V(s.pyramidAll), V(s.distortion.view), V(s.disocclusion.view), cb}),
			makeSet(kInterpolate, {V(s.gameX.view), V(s.gameY.view), V(s.flowX.view), V(s.flowY.view), V(older.view), V(newer.view),
									  V(s.disocclusion.view), V(s.pyramidAll), counters, V(s.output.view), cb}),
			[&] {
				std::vector<Res> r = {V(s.output.view), counters};
				r.insert(r.end(), mips.begin(), mips.end());
				r.push_back(cb);
				r.push_back(colourCb);
				return makeSet(kInpaintingPyramid, r);
			}(),
			makeSet(kInpainting, {V(s.sceneChange.view), V(s.pyramidAll), V(newer.view), V(newer.view), V(s.output.view), cb}),
		};

		// (The sampled pictures are in SHADER_READ_ONLY_OPTIMAL; the sets say GENERAL for everything,
		// so the two are put in GENERAL for this and back.)
		VkCommandBuffer cmd = begin();
		layout(cmd, older.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		layout(cmd, newer.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkClearColorValue zero = {}, farDepth = {}, halfDepth = {};
		farDepth.uint32[0] = 0x3f800000u; // 1.0f: the far plane, what AMD clears the nearest-depth images to
		halfDepth.float32[0] = 0.5f;      // one depth for the whole picture
		if (!gameVectors)
			vkCmdClearColorImage(cmd, s.inputMotion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, s.inputDepth.image, VK_IMAGE_LAYOUT_GENERAL, &halfDepth, 1, &whole);
		vkCmdClearColorImage(cmd, s.distortion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, s.sceneChange.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, s.flowConfidence.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole);
		vkCmdClearColorImage(cmd, s.previousDepth.image, VK_IMAGE_LAYOUT_GENERAL, &farDepth, 1, &whole);
		vkCmdClearColorImage(cmd, s.interpolatedDepth.image, VK_IMAGE_LAYOUT_GENERAL, &farDepth, 1, &whole);
		// Frames since the last reset: well past FSR 3's first 10 (in which it only mixes the two).
		vkCmdFillBuffer(cmd, s.counters.buffer, 0, 4, 0);
		vkCmdFillBuffer(cmd, s.counters.buffer, 4, 4, 20);

		const uint32_t gx = (w + 7) / 8, gy = (h + 7) / 8;
		VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		// ffxFrameInterpolationPrepare
		dispatch(cmd, kReconstructAndDilate, sets[kReconstructAndDilate], gx, gy);
		// ffxFrameInterpolationDispatch (not a reset: its preparation passes run)
		dispatch(cmd, kSetup, sets[kSetup], gx, gy);
		dispatch(cmd, kReconstructPreviousDepth, sets[kReconstructPreviousDepth], gx, gy);
		dispatch(cmd, kGameMotionVectorField, sets[kGameMotionVectorField], gx, gy);
		dispatch(cmd, kGameVectorFieldPyramid, sets[kGameVectorFieldPyramid], gameGroups[0], gameGroups[1]);
		dispatch(cmd, kOpticalFlowVectorField, sets[kOpticalFlowVectorField], (uint32_t)(w / 8.0f + 7) / 8, (uint32_t)(h / 8.0f + 7) / 8);
		dispatch(cmd, kDisocclusionMask, sets[kDisocclusionMask], gx, gy);
		dispatch(cmd, kInterpolate, sets[kInterpolate], gx, gy);
		dispatch(cmd, kInpaintingPyramid, sets[kInpaintingPyramid], colourGroups[0], colourGroups[1]);
		dispatch(cmd, kInpainting, sets[kInpainting], gx, gy);
		layout(cmd, older.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		layout(cmd, newer.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		finish(cmd);
		download(s.output, VK_IMAGE_LAYOUT_GENERAL, out, (size_t)w * h * 4);
		return true;
	}

	void Release()
	{
		if (!s.pool)
			return;
		for (int p = 0; p < kPassCount; p++)
		{
			vkDestroyPipeline(s_device, s.pipelines[p], nullptr);
			vkDestroyPipelineLayout(s_device, s.pipelineLayouts[p], nullptr);
			vkDestroyDescriptorSetLayout(s_device, s.layouts[p], nullptr);
		}
		vkDestroyDescriptorPool(s_device, s.pool, nullptr);
		vkDestroySampler(s_device, s.sampler, nullptr);
		for (TestImage* image : {&s.inputMotion, &s.inputDepth, &s.distortion, &s.dilatedMotion, &s.dilatedDepth, &s.previousDepth,
				 &s.interpolatedDepth, &s.gameX, &s.gameY, &s.flowX, &s.flowY, &s.disocclusion, &s.output, &s.sceneChange, &s.flowConfidence})
			free_image(*image);
		for (VkImageView view : s.pyramidMips)
			vkDestroyImageView(s_device, view, nullptr);
		vkDestroyImageView(s_device, s.pyramidAll, nullptr);
		vkDestroyImage(s_device, s.pyramid, nullptr);
		vkFreeMemory(s_device, s.pyramidMemory, nullptr);
		for (Buffer* b : {&s.counters, &s.constants, &s.gamePyramid, &s.colourPyramid})
			free_buffer(*b);
		s = State{};
	}
} // namespace Fsr3FiRoute

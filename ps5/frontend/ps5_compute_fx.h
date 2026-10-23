// Crux2 - frame generation from AMD's FSR 3 optical flow, and NVIDIA Image Scaling, as Vulkan compute.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Two effects recorded into the caller's command buffer, each a handful of compute dispatches
// with every barrier they need inside. Neither needs anything from the game but its pictures.
//
//  * OpticalFlowFG makes the picture between two pictures, at any time t between them. The motion
//    is found from the two pictures alone (a PS2 game has no motion vectors to give) by AMD's
//    FidelityFX FSR 3 optical flow (FidelityFX SDK 2.3.0, MIT): a luma pyramid, block matching
//    down it level by level with a median filter after each, run both ways (where the newer
//    picture's 8x8 blocks were, where the older picture's went). FSR 3's scene change detection
//    (the two pictures' histograms and how far apart they are) is weighed with how well the blocks
//    found their matches, and on a cut the output is one of the two pictures as it is. Crux2's own
//    passes then find the camera's move, make sure of each block's vector (against the camera's
//    move and blocks further off: a block matcher locks onto the wrong repeat of a repeating
//    pattern, and misses the edges of things that move), refine it to a fraction of a pixel, and
//    draw the picture at t: every pixel from the motion that fits it best, what is covered in one
//    picture taken from the other alone, and the two mixed where nothing fits (a rolling ball).
//    ps5/frontend/third_party/fidelityfx/README.md has the passes, what differs from the SDK, and
//    why FSR 3's own frame interpolation passes are not the ones used (they were measured).
//  * NIS scales a picture by 1x to 2x on each axis and sharpens it: NVIDIA Image Scaling v1.0.3
//    (MIT), its 6-tap scaler with edge-directed filters and an adaptive sharpener, or the
//    sharpener alone at 1x (ps5/frontend/third_party/nis/README.md).
//
// The Vulkan functions are called as vkXxx: in the title they are PCSX2's function pointers
// (build with PS5_COMPUTE_FX_PCSX2 defined and pcsx2/ on the include path), elsewhere libvulkan's
// prototypes. Only core Vulkan 1.1 is used, and nothing PCSX2 does not already turn on: compute
// shaders, subgroup arithmetic in compute shaders (a property, which RADV has, not a feature),
// storage images of formats every device has. The SPIR-V is compiled ahead
// (ps5_compute_fx_spirv.inc, made by ps5/tools/make-compute-fx.sh): no shader compiler runs in
// the title. Tests: ps5/tests/computefx/run-computefx-tests.sh.

#pragma once

#ifdef PS5_COMPUTE_FX_PCSX2
#include "GS/Renderers/Vulkan/VKLoader.h"
#else
#include <vulkan/vulkan.h>
#endif

#include <cstdint>
#include <vector>

namespace PS5ComputeFX
{
	// The device the effects run on, and what it was created with.
	struct Device
	{
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		const VkAllocationCallbacks* allocator = nullptr;
		VkPipelineCache pipelineCache = VK_NULL_HANDLE;
		// Records of one effect that can be waiting for the GPU at once (each has descriptors of
		// its own, which a new record must not change under it): PCSX2 has 3 command buffers.
		uint32_t framesInFlight = 3;
		// Features that were turned on at vkCreateDevice (not merely supported). With them, NIS can
		// use its half precision scaler, and outputs of other formats than RGBA8 and RGBA16F work.
		bool shaderFloat16 = false;
		bool shaderStorageImageWriteWithoutFormat = false;
	};

	// Where the optical flow runs and how hard the picture is looked at: the flow runs on the
	// picture scaled to at most this many lines (the cost of the flow and the refine goes with its
	// size; the composite's with the output's), and the composite also tries the vectors of this
	// many blocks around each pixel's own.
	enum class Quality : uint32_t
	{
		Performance = 0, // at most 540 lines; the block's own vector
		Balanced = 1,    // at most 720 lines; and its 4 neighbours'
		Best = 2,        // at most 1080 lines; and all 8 neighbours'
	};

	// A timestamp query pool to time the passes with (optional): OpticalFlowFG writes
	// kTimestampCount queries from `first` on, NIS writes 2.
	struct Timestamps
	{
		VkQueryPool pool = VK_NULL_HANDLE;
		uint32_t first = 0;
	};

	struct FrameGenInput
	{
		// The two pictures, the same size as the effect was made for: sampled images in
		// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (RGBA8 or RGB10A2, or any colour format a shader
		// can sample). They stay in that layout. They may be the same image.
		VkImageView previous = VK_NULL_HANDLE;
		VkImageView current = VK_NULL_HANDLE;
		// The picture made, the same size: an image with VK_IMAGE_USAGE_STORAGE_BIT in
		// VK_IMAGE_LAYOUT_GENERAL, its view of the format the effect was made for (a UNORM or
		// SFLOAT view, not SRGB). It is left in GENERAL with the writes visible to every stage.
		VkImageView output = VK_NULL_HANDLE;
		// When between the two: 0 the previous picture, 1 the current one.
		float t = 0.5f;
		// After a cut between the two the output is one of them as it is: the current one (true)
		// or the previous one (false).
		bool showCurrentOnCut = true;
	};

	class OpticalFlowFG
	{
	public:
		// The timestamps written (with Timestamps given): before anything, after the luma
		// pictures and the scene change detection, after each of the two flows, after the camera,
		// refine and verdict passes, and after the composite.
		static constexpr uint32_t kTimestampCount = 6;
		static const char* TimestampName(uint32_t index);

		OpticalFlowFG();
		~OpticalFlowFG();
		OpticalFlowFG(const OpticalFlowFG&) = delete;
		OpticalFlowFG& operator=(const OpticalFlowFG&) = delete;

		// Pictures of width x height, made into an image of `outputFormat` (RGBA8_UNORM or
		// R16G16B16A16_SFLOAT; any other storage format with shaderStorageImageWriteWithoutFormat).
		// False if the device cannot (the reason is in LastError()); nothing is left made then.
		bool Create(const Device& device, uint32_t width, uint32_t height, VkFormat outputFormat, Quality quality);
		void Destroy();
		bool IsCreated() const { return m_device.device != VK_NULL_HANDLE; }

		// The composite's weights (see third_party/fidelityfx/crux2/fg_warp.comp). The defaults are
		// the ones the tests were measured with.
		struct Tuning
		{
			float oneSidedCost = 0.12f; // what showing content seen in one picture only costs
			float giveUp = 0.15f;       // over this nothing fits, and the two pictures are mixed
			float tolerance = 1.0f;     // pixels (+8% of the motion) a field may be off a motion
			float otherCost = 0.0f;     // on top for any motion but the fields' own at the pixel (0: tried, no gain)
			bool debugView = false;     // the output shows what was decided (fg_warp.comp), not a picture
		};
		void SetTuning(const Tuning& tuning) { m_tuning = tuning; }
		const Tuning& GetTuning() const { return m_tuning; }

		// Records the picture at in.t between in.previous and in.current into in.output. Nothing
		// is submitted. At most Device::framesInFlight records may wait for the GPU at once.
		bool Record(VkCommandBuffer cmd, const FrameGenInput& in, const Timestamps* timestamps = nullptr);

		// What a record (the number LastRecord() gives after Record()) found about a cut, once its
		// command buffer has finished; false if that record's slot has been used again since. The
		// GPU acts on a cut by itself (the output is one of the two pictures as it is); this is
		// for the caller's own bookkeeping (pacing, statistics, holding a cut back a frame).
		struct SceneChangeInfo
		{
			float divergence;   // FSR 3's: how far apart the two pictures' histograms are
			bool detector;      // FSR 3's detector fired (divergence over 0.45)
			float matchedShare; // of the blocks with something in them, the share that found a match
			bool cut;           // the verdict: the detector fired and under half of them matched
		};
		bool SceneChange(uint64_t record, SceneChangeInfo* info) const;
		uint64_t LastRecord() const { return m_records - 1; }

		uint32_t Width() const { return m_width; }
		uint32_t Height() const { return m_height; }
		// The size the flow runs at (the luma picture) and of its vector field (a vector a block of
		// 8x8 luma pixels), and the pyramid's levels.
		uint32_t FlowWidth() const { return m_lumaWidth; }
		uint32_t FlowHeight() const { return m_lumaHeight; }
		uint32_t FieldWidth() const { return m_fieldWidth; }
		uint32_t FieldHeight() const { return m_fieldHeight; }
		uint32_t Levels() const { return m_levels; }
		// Device memory the effect holds.
		uint64_t MemoryBytes() const { return m_memoryBytes; }
		// Workgroups one Record() dispatches, pass by pass (for an estimate of its cost).
		struct PassCount
		{
			const char* name;
			uint32_t dispatches;
			uint64_t workgroups;
			uint32_t threadsPerGroup;
		};
		std::vector<PassCount> Workload() const;
		const char* LastError() const { return m_error; }

		// For tests: the images the last record left (in VK_IMAGE_LAYOUT_GENERAL). 0, 1 the flow's
		// whole-pixel vectors (RGBA16_SINT, FieldWidth x FieldHeight, luma pixels; 0 for the
		// current picture's blocks, 1 for the previous picture's), 2, 3 the refined vectors
		// (RGBA16F, output pixels: xy the vector, z what is left after the match, w the block's
		// variance), 4 the scene change detection's output (R32_UINT, 3x1: the divergence as a
		// float's bits, the history of cuts, a counter), 5 the verdict (R32_UINT, 4x1: cut, the
		// detector, matched share in thousandths, blocks counted), 6 the camera (RGBA32F, 2x2: each
		// way the move, zoom and turn in luma pixels, and the share agreeing and the middle).
		VkImageView DebugView(uint32_t which) const;
		VkImage DebugImage(uint32_t which) const;

	private:
		struct Image
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			uint32_t width = 0, height = 0;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImageUsageFlags usage = 0;
		};
		enum Pipeline : uint32_t
		{
			kPrepareLuma,
			kLumaPyramid,
			kScdHistogram,
			kScdDivergence,
			kSearch,
			kFilter,
			kScale,
			kRefine,
			kComposite,
			kCamera,
			kVerdict,
			kLumaGain,
			kPipelineCount
		};
		static constexpr uint32_t kMaxLevels = 7;

		bool Fail(const char* error);
		bool MakeImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
		bool AllocateImages();
		bool MakePipelines();
		bool MakeStaticSets();
		VkDescriptorSet StaticSet(Pipeline pipeline);
		void Dispatch(VkCommandBuffer cmd, Pipeline pipeline, VkDescriptorSet set, const void* constants, uint32_t size, uint32_t x,
			uint32_t y, uint32_t z);
		void FlowConstants(void* out, uint32_t level, uint32_t direction) const;

		Device m_device{};
		uint32_t m_width = 0, m_height = 0;
		VkFormat m_outputFormat = VK_FORMAT_UNDEFINED;
		Quality m_quality = Quality::Balanced;
		uint32_t m_lumaWidth = 0, m_lumaHeight = 0, m_lumaPadWidth = 0, m_lumaPadHeight = 0;
		uint32_t m_fieldWidth = 0, m_fieldHeight = 0, m_levels = 0;
		uint32_t m_flowWidth[kMaxLevels] = {}, m_flowHeight[kMaxLevels] = {};
		const char* m_error = "";
		Tuning m_tuning{};

		// Images: the two luma pyramids (0 the previous picture, 1 the current), the flow's two
		// working fields at each level, each direction's result and refined field, and the scene
		// change detection's histograms and results.
		Image m_luma[2][kMaxLevels];
		Image m_flowA[kMaxLevels], m_flowB[kMaxLevels];
		Image m_flowFinal[2], m_field[2];
		Image m_histogram[2], m_filtered[2], m_scdTemp, m_scdOutput, m_camera, m_verdict, m_lumaGain;
		std::vector<Image*> m_images;
		std::vector<VkDeviceMemory> m_memory;
		uint64_t m_memoryBytes = 0;
		bool m_initialised = false; // the images are in GENERAL

		VkBuffer m_readback = VK_NULL_HANDLE;
		VkDeviceMemory m_readbackMemory = VK_NULL_HANDLE;
		const uint32_t* m_readbackData = nullptr;

		VkSampler m_linear = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_setLayouts[kPipelineCount] = {};
		VkPipelineLayout m_pipelineLayouts[kPipelineCount] = {};
		VkPipeline m_pipelines[kPipelineCount] = {};
		VkDescriptorPool m_staticPool = VK_NULL_HANDLE;
		std::vector<VkDescriptorPool> m_framePools;
		// The sets that never change: [picture] pyramid and histogram, [run] divergence,
		// [direction][level] search, filter and scale.
		VkDescriptorSet m_pyramidSet[2] = {}, m_histogramSet[2] = {}, m_divergenceSet[2] = {};
		VkDescriptorSet m_searchSet[2][kMaxLevels] = {}, m_filterSet[2][kMaxLevels] = {}, m_scaleSet[2][kMaxLevels] = {};
		VkDescriptorSet m_cameraSet = VK_NULL_HANDLE, m_verdictSet = VK_NULL_HANDLE, m_lumaGainSet = VK_NULL_HANDLE;
		uint32_t m_gainLevel = 0;
		uint64_t m_records = 0;
		std::vector<uint64_t> m_slotRecord;
	};

	struct NISInput
	{
		// The picture to scale: sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (left so).
		VkImageView input = VK_NULL_HANDLE;
		uint32_t inputWidth = 0, inputHeight = 0;
		// The picture made: storage, VK_IMAGE_LAYOUT_GENERAL (left so, the writes visible to every
		// stage), a view of the format the effect was made for. 1x to 2x the input on each axis;
		// the same size sharpens only.
		VkImageView output = VK_NULL_HANDLE;
		uint32_t outputWidth = 0, outputHeight = 0;
		// 0 (none) to 1, NVIDIA's slider (0.5 is its default).
		float sharpness = 0.5f;
	};

	class NIS
	{
	public:
		NIS();
		~NIS();
		NIS(const NIS&) = delete;
		NIS& operator=(const NIS&) = delete;

		bool Create(const Device& device, VkFormat outputFormat);
		void Destroy();
		bool IsCreated() const { return m_device.device != VK_NULL_HANDLE; }
		// False (and nothing recorded) for sizes NIS cannot do: smaller, or over 2x.
		bool Record(VkCommandBuffer cmd, const NISInput& in, const Timestamps* timestamps = nullptr);
		bool UsesHalfPrecision() const { return m_half; }
		const char* LastError() const { return m_error; }

	private:
		bool Fail(const char* error);

		Device m_device{};
		VkFormat m_outputFormat = VK_FORMAT_UNDEFINED;
		bool m_half = false;
		const char* m_error = "";
		VkSampler m_linear = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_scaler = VK_NULL_HANDLE, m_sharpener = VK_NULL_HANDLE;
		// NVIDIA's filter tables (64 phases of 8 taps: 2x64 RGBA32F), filled by the first record.
		VkImage m_coefficients[2] = {};
		VkImageView m_coefficientViews[2] = {};
		VkBuffer m_upload = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		bool m_uploaded = false;
		std::vector<VkDescriptorPool> m_framePools;
		uint64_t m_records = 0;
	};
} // namespace PS5ComputeFX

// Crux2 - the compute effects (FSR 3 optical flow frame generation, NIS) run on the PC by a software Vulkan driver.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   computefx_test <out folder>                 every test; pictures of some of them into the folder
//   computefx_test <out folder> --compile-only  only make the effects (all pipelines) and stop
//   computefx_test <out folder> --fsr3-fi <dir> also run FSR 3's own frame interpolation passes on
//                                               the same flow (the .spv run-computefx-tests.sh
//                                               made in <dir>) and print them beside Crux2's
//
// Frame generation: two pictures of a scene whose every moment can be drawn (the scenes of
// ps5/tests/framegen/fg_test.c: camera turns of 12 to 230 pixels at 720p, a disc or a ball moving
// over a still or moving background, a picture that fades, a cut) go in, the picture between them
// comes out and is held against the scene drawn at that moment (PSNR, the outer band that came
// into view during the move left out). It has to be nearer to it than the older picture shown
// again and than the two mixed half and half. A still picture comes out as it went in; after a cut
// the newer picture is shown as it is and the scene change detection says so.
// NIS: a pattern drawn at the output's size is the truth; NIS's 2x and 1.5x of the pattern drawn
// small has to be nearer to it (and sharper) than a bilinear stretch, a flat colour has to stay
// flat, and the output has to be the size asked for with its edges right.
// The passes are timed with timestamps (on a CPU driver: it shows they run, not what they cost on
// the console; the estimate for that is from the workgroup counts printed with them).
//
// For looking into a scene that comes out wrong (environment variables):
//   CFX_QUICK=n        frame generation only, one quality (CFX_QUALITY=0..2, else the best), the
//                      first n scenes (or CFX_SHOTS=6,7,8), the still picture with CFX_STILL=1
//   CFX_FIELDS=1       the camera's move, the vectors' medians and the verdict of every record;
//                      CFX_FIELD_ROW=y a row of vectors, CFX_FIELD_DIR=folder pictures of them
//   CFX_PPM_ALL=1      every scene's picture into the out folder
//   CFX_TUNE=a,b,c,d   OpticalFlowFG::Tuning (oneSidedCost, giveUp, tolerance, otherCost);
//                      with CFX_DEBUG_VIEW=1 the pictures show what the composite decided
//   CFX_NO_FG=1        NIS only

#include "../../frontend/ps5_compute_fx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace PS5ComputeFX;

static int s_checks = 0, s_failed = 0;
#define CHECK(what, ...) \
	do \
	{ \
		s_checks++; \
		if (!(what)) \
		{ \
			s_failed++; \
			std::printf("FAILED %s:%d: %s  ", __FILE__, __LINE__, #what); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)
#define VK(call) \
	do \
	{ \
		const VkResult r_ = (call); \
		if (r_ != VK_SUCCESS) \
		{ \
			std::fprintf(stderr, "%s: %d\n", #call, (int)r_); \
			std::exit(2); \
		} \
	} while (0)

// ---- the device

static VkInstance s_instance;
static VkPhysicalDevice s_gpu;
static VkDevice s_device;
static VkQueue s_queue;
static uint32_t s_family;
static VkCommandPool s_pool;
static VkQueryPool s_queries;
static double s_tickNs = 1.0;
static bool s_float16 = false;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(s_gpu, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
	{
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	}
	std::fprintf(stderr, "no memory type\n");
	std::exit(2);
}

struct TestImage
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
};

static TestImage make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage)
{
	TestImage out;
	out.width = w;
	out.height = h;
	out.format = format;
	VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = {w, h, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VK(vkCreateImage(s_device, &info, nullptr, &out.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s_device, out.image, &req);
	VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK(vkAllocateMemory(s_device, &alloc, nullptr, &out.memory));
	VK(vkBindImageMemory(s_device, out.image, out.memory, 0));
	VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view.image = out.image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VK(vkCreateImageView(s_device, &view, nullptr, &out.view));
	return out;
}

static void free_image(TestImage& image)
{
	vkDestroyImageView(s_device, image.view, nullptr);
	vkDestroyImage(s_device, image.image, nullptr);
	vkFreeMemory(s_device, image.memory, nullptr);
	image = TestImage{};
}

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* data = nullptr;
};

static Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
{
	Buffer out;
	VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	info.size = size;
	info.usage = usage;
	VK(vkCreateBuffer(s_device, &info, nullptr, &out.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s_device, out.buffer, &req);
	VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK(vkAllocateMemory(s_device, &alloc, nullptr, &out.memory));
	VK(vkBindBufferMemory(s_device, out.buffer, out.memory, 0));
	VK(vkMapMemory(s_device, out.memory, 0, size, 0, &out.data));
	return out;
}

static void free_buffer(Buffer& buffer)
{
	vkDestroyBuffer(s_device, buffer.buffer, nullptr);
	vkFreeMemory(s_device, buffer.memory, nullptr);
	buffer = Buffer{};
}

static void layout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static VkCommandBuffer begin()
{
	VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = s_pool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VK(vkAllocateCommandBuffers(s_device, &alloc, &cmd));
	VkCommandBufferBeginInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK(vkBeginCommandBuffer(cmd, &info));
	return cmd;
}

// Submits and waits; the wall time it took, in milliseconds.
static double finish(VkCommandBuffer cmd)
{
	VK(vkEndCommandBuffer(cmd));
	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	const auto start = std::chrono::steady_clock::now();
	VK(vkQueueSubmit(s_queue, 1, &submit, VK_NULL_HANDLE));
	VK(vkQueueWaitIdle(s_queue));
	const auto end = std::chrono::steady_clock::now();
	vkFreeCommandBuffers(s_device, s_pool, 1, &cmd);
	return std::chrono::duration<double, std::milli>(end - start).count();
}

// RGBA8 pixels (or RGB10A2 words, or RGBA16F) into an image left in `final`.
static void upload(TestImage& image, const void* pixels, size_t bytes, VkImageLayout final)
{
	Buffer staging = make_buffer(bytes);
	std::memcpy(staging.data, pixels, bytes);
	VkCommandBuffer cmd = begin();
	layout(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {image.width, image.height, 1}};
	vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	layout(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final);
	finish(cmd);
	free_buffer(staging);
}

static void download(const TestImage& image, VkImageLayout current, void* pixels, size_t bytes)
{
	Buffer staging = make_buffer(bytes);
	VkCommandBuffer cmd = begin();
	const VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {image.width, image.height, 1}};
	vkCmdCopyImageToBuffer(cmd, image.image, current, staging.buffer, 1, &copy);
	finish(cmd);
	std::memcpy(pixels, staging.data, bytes);
	free_buffer(staging);
}

static void set_general(TestImage& image)
{
	VkCommandBuffer cmd = begin();
	layout(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	finish(cmd);
}

// ---- scenes whose every moment can be drawn (ps5/tests/framegen/fg_test.c's)

static double hash2(int x, int y)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (double)((h ^ (h >> 16)) & 0xffffff) / (double)0xffffff;
}

static double grain(double u, double v, double scale)
{
	const double x = u * scale, y = v * scale;
	const int ix = (int)std::floor(x), iy = (int)std::floor(y);
	const double fx = x - ix, fy = y - iy;
	const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
	const double top = hash2(ix, iy) + (hash2(ix + 1, iy) - hash2(ix, iy)) * sx;
	const double bottom = hash2(ix, iy + 1) + (hash2(ix + 1, iy + 1) - hash2(ix, iy + 1)) * sx;
	return top + (bottom - top) * sy;
}

static void land(double u, double v, double rgb[3])
{
	const double g = 0.5 * grain(u, v, 9) + 0.3 * grain(u, v, 31) + 0.2 * grain(u, v, 97);
	double r, gr, b;
	if (v < 0.38 + 0.05 * std::sin(u * 2.3))
	{
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
		if (std::fmod(std::floor(u * 1.5) + 300.0, 3.0) < 1.0 && v > 0.7)
		{
			const int cell = ((int)std::floor(u * 24) + (int)std::floor(v * 24)) & 1;
			r = gr = b = (cell ? 0.75 : 0.3) + 0.1 * g;
		}
	}
	const double post = std::fmod(u * 3.0 + 300.0, 1.0);
	if (post < 0.035 && v > 0.25 && v < 0.8)
	{
		r = 0.35 + 0.2 * g;
		gr = 0.22 + 0.1 * g;
		b = 0.1;
	}
	if (std::fabs(v - 0.62) < 0.006 || std::fabs(v - 0.68) < 0.006)
	{
		r = 0.85;
		gr = 0.82;
		b = 0.7;
	}
	rgb[0] = r;
	rgb[1] = gr;
	rgb[2] = b;
}

static int ball(double u, double v, double cx, double cy, double radius, double turn, int plain, double rgb[3])
{
	const double dx = u - cx, dy = v - cy;
	if (dx * dx + dy * dy > radius * radius)
		return 0;
	const double ring = std::sqrt(dx * dx + dy * dy) / radius;
	if (plain)
	{
		const double g = 0.6 * grain(dx + 7.0, dy + 3.0, 40) + 0.4 * grain(dx + 1.0, dy + 9.0, 110);
		rgb[0] = (ring > 0.9) ? 0.15 : 0.55 + 0.45 * g;
		rgb[1] = (ring > 0.9) ? 0.1 : 0.15 + 0.5 * g * g;
		rgb[2] = (ring > 0.9) ? 0.1 : 0.1 + 0.2 * g;
		return 1;
	}
	const double angle = std::atan2(dy, dx) + turn;
	const int stripe = ((int)std::floor(angle * 6 / 3.14159265 + 100.0) + (int)std::floor(ring * 4)) & 1;
	rgb[0] = stripe ? 0.9 : 0.75;
	rgb[1] = stripe ? 0.2 : 0.7;
	rgb[2] = stripe ? 0.15 : 0.1;
	return 1;
}

struct Shot
{
	const char* name;
	double pan_x = 0, pan_y = 0;   // the camera's move from the older picture to the newer, in pixels
	double zoom = 0;               // how much nearer it is in the newer one
	double ball_x = 0, ball_y = 0; // the ball's own move on the screen
	int has_ball = 0;
	int slides = 0;
	double fade = 0;
	int cut = 0;
};

static void render(const Shot& shot, double time, uint8_t* pixels, uint32_t w, uint32_t h)
{
	const double place = (shot.cut && time > 0.75) ? 40.0 : 0.0;
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
					if (!shot.has_ball || !ball(px / h, py / h, (0.45 * w + shot.ball_x * (time - 0.5)) / h, (0.55 * h + shot.ball_y * (time - 0.5)) / h,
											  0.11, time * shot.ball_x / (0.11 * h), shot.slides, rgb))
					{
						const double scale = 1.0 / (1.0 + shot.zoom * time);
						const double u = ((px - 0.5 * w) * scale + 0.5 * w + shot.pan_x * time) / h + place;
						const double v = ((py - 0.5 * h) * scale + 0.5 * h + shot.pan_y * time) / h + place * 0.37;
						land(u, v, rgb);
					}
					for (int c = 0; c < 3; c++)
						sum[c] += rgb[c] * (1.0 - shot.fade * time);
				}
			}
			uint8_t* const p = pixels + ((size_t)y * w + x) * 4;
			for (int c = 0; c < 3; c++)
				p[c] = (uint8_t)std::lrint(std::fmin(std::fmax(sum[c] / 9, 0), 1) * 255);
			p[3] = 255;
		}
	}
}

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
	return (sum == 0) ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * (double)count / sum);
}

static void write_ppm(const std::string& folder, const char* name, const uint8_t* pixels, uint32_t w, uint32_t h)
{
	const std::string path = folder + "/" + name + ".ppm";
	FILE* const fp = std::fopen(path.c_str(), "wb");
	if (!fp)
		return;
	std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (size_t i = 0; i < (size_t)w * h; i++)
		std::fwrite(pixels + i * 4, 1, 3, fp);
	std::fclose(fp);
}

static float half_to_float(uint16_t h);

static uint16_t float_to_half(float f)
{
	uint32_t bits;
	std::memcpy(&bits, &f, 4);
	const uint32_t sign = (bits >> 16) & 0x8000u;
	const int exponent = (int)((bits >> 23) & 255) - 127 + 15;
	const uint32_t mantissa = bits & 0x7fffffu;
	if (exponent <= 0)
		return (uint16_t)sign; // (tiny: zero)
	if (exponent >= 31)
		return (uint16_t)(sign | 0x7c00u);
	return (uint16_t)(sign | ((uint32_t)exponent << 10) | ((mantissa + 0x1000u) >> 13));
}

// FSR 3's own frame interpolation (route 1), for the comparison.
#include "fsr3_fi_route.h"

// ---- frame generation

static const uint32_t W = 1280, H = 720;
static const size_t BYTES = (size_t)W * H * 4;

struct Rig
{
	OpticalFlowFG fg;
	TestImage older, newer, out;
	std::vector<double> passMs; // the timestamps' intervals, summed over the records
	uint32_t timed = 0;
	double wallMs = 0;
};

static Device device_info()
{
	Device d;
	d.physicalDevice = s_gpu;
	d.device = s_device;
	d.framesInFlight = 2;
	d.shaderFloat16 = s_float16;
	return d;
}

// (CFX_FIELDS=1: what the flow found, each way: the median vector and how many blocks are within
// a pixel of it; for looking at a scene that comes out wrong.)
static void print_fields(const OpticalFlowFG& fg)
{
	const uint32_t fw = fg.FieldWidth(), fh = fg.FieldHeight();
	{
		TestImage camera;
		camera.image = fg.DebugImage(6);
		camera.width = 2;
		camera.height = 2;
		float c[16];
		download(camera, VK_IMAGE_LAYOUT_GENERAL, c, sizeof(c));
		for (int d = 0; d < 2; d++)
			std::printf("      camera %d: move (%.2f, %.2f) zoom %.4f turn %.4f, %.0f%% agree\n", d, c[d * 4], c[d * 4 + 1], c[d * 4 + 2], c[d * 4 + 3],
				c[8 + d * 4] * 100);
	}
	for (uint32_t which = 0; which < 4; which++)
	{
		TestImage view;
		view.image = fg.DebugImage(which);
		view.width = fw;
		view.height = fh;
		std::vector<int16_t> raw((size_t)fw * fh * 4);
		download(view, VK_IMAGE_LAYOUT_GENERAL, raw.data(), raw.size() * 2);
		std::vector<float> xs, ys;
		for (size_t i = 0; i < (size_t)fw * fh; i++)
		{
			const float x = (which < 2) ? raw[i * 4] : half_to_float((uint16_t)raw[i * 4]);
			const float y = (which < 2) ? raw[i * 4 + 1] : half_to_float((uint16_t)raw[i * 4 + 1]);
			xs.push_back(x);
			ys.push_back(y);
		}
		std::vector<float> sx = xs, sy = ys;
		std::nth_element(sx.begin(), sx.begin() + sx.size() / 2, sx.end());
		std::nth_element(sy.begin(), sy.begin() + sy.size() / 2, sy.end());
		const float mx = sx[sx.size() / 2], my = sy[sy.size() / 2];
		size_t near = 0;
		for (size_t i = 0; i < xs.size(); i++)
			near += (std::fabs(xs[i] - mx) <= 1.0f && std::fabs(ys[i] - my) <= 1.0f);
		if (std::getenv("CFX_FIELD_DIR"))
		{
			// |vector - median| as a grey picture, a pixel a block (white: 8 pixels off or more).
			static int number = 0;
			const std::string path = std::string(std::getenv("CFX_FIELD_DIR")) + "/field" + std::to_string(number / 4) + "-" + std::to_string(which) + ".pgm";
			number++;
			if (FILE* fp = std::fopen(path.c_str(), "wb"))
			{
				std::fprintf(fp, "P5\n%u %u\n255\n", fw, fh);
				for (size_t i = 0; i < xs.size(); i++)
				{
					const float d = std::fmax(std::fabs(xs[i] - mx), std::fabs(ys[i] - my));
					std::fputc((int)std::fmin(d * 32.0f, 255.0f), fp);
				}
				std::fclose(fp);
			}
		}
		if (const char* row = std::getenv("CFX_FIELD_ROW")) // the vectors along one row of blocks
		{
			const uint32_t y = (uint32_t)std::atoi(row);
			std::printf("      row %u of field %u:", y, which);
			for (uint32_t x = 0; x < fw; x += 2)
				std::printf(" %d:(%.1f,%.1f)", x, xs[(size_t)y * fw + x], ys[(size_t)y * fw + x]);
			std::printf("\n");
		}
		static const char* const names[4] = {"flow, current's blocks", "flow, previous's blocks", "refined, current's", "refined, previous's"};
		std::printf("      %-24s median (%7.2f, %7.2f), %5.1f%% of blocks within a pixel of it\n", names[which], mx, my, 100.0 * near / xs.size());
	}
}

// The picture at t between `a` and `b` (RGBA8, W x H) into `made`; whether the GPU took it for a cut.
static bool between(Rig& rig, const uint8_t* a, const uint8_t* b, float t, uint8_t* made, bool showCurrent = true,
	OpticalFlowFG::SceneChangeInfo* scene = nullptr)
{
	upload(rig.older, a, BYTES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	upload(rig.newer, b, BYTES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	VkCommandBuffer cmd = begin();
	FrameGenInput in;
	in.previous = rig.older.view;
	in.current = rig.newer.view;
	in.output = rig.out.view;
	in.t = t;
	in.showCurrentOnCut = showCurrent;
	Timestamps ts;
	ts.pool = s_queries;
	ts.first = 0;
	CHECK(rig.fg.Record(cmd, in, &ts), "record");
	rig.wallMs += finish(cmd);
	uint64_t stamps[OpticalFlowFG::kTimestampCount];
	if (vkGetQueryPoolResults(s_device, s_queries, 0, OpticalFlowFG::kTimestampCount, sizeof(stamps), stamps, sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
	{
		rig.passMs.resize(OpticalFlowFG::kTimestampCount - 1, 0.0);
		for (uint32_t i = 1; i < OpticalFlowFG::kTimestampCount; i++)
			rig.passMs[i - 1] += (double)(stamps[i] - stamps[i - 1]) * s_tickNs * 1e-6;
		rig.timed++;
	}
	download(rig.out, VK_IMAGE_LAYOUT_GENERAL, made, BYTES);
	if (std::getenv("CFX_FIELDS"))
		print_fields(rig.fg);
	OpticalFlowFG::SceneChangeInfo info = {};
	CHECK(rig.fg.SceneChange(rig.fg.LastRecord(), &info), "scene change readback");
	if (scene)
		*scene = info;
	if (std::getenv("CFX_FIELDS"))
		std::printf("      scene: divergence %.3f (%s), matched %.0f%%, %s\n", info.divergence, info.detector ? "fired" : "quiet", info.matchedShare * 100,
			info.cut ? "a cut" : "no cut");
	return info.cut;
}

static void make_rig(Rig& rig, Quality quality, VkFormat inputFormat = VK_FORMAT_R8G8B8A8_UNORM, VkFormat outputFormat = VK_FORMAT_R8G8B8A8_UNORM)
{
	if (!rig.fg.Create(device_info(), W, H, outputFormat, quality))
	{
		std::fprintf(stderr, "OpticalFlowFG::Create: %s\n", rig.fg.LastError());
		std::exit(2);
	}
	if (const char* tune = std::getenv("CFX_TUNE")) // "oneSidedCost,giveUp,tolerance", for trying weights
	{
		OpticalFlowFG::Tuning tuning;
		std::sscanf(tune, "%f,%f,%f,%f", &tuning.oneSidedCost, &tuning.giveUp, &tuning.tolerance, &tuning.otherCost);
		tuning.debugView = std::getenv("CFX_DEBUG_VIEW") != nullptr;
		rig.fg.SetTuning(tuning);
	}
	rig.older = make_image(W, H, inputFormat, VK_IMAGE_USAGE_SAMPLED_BIT);
	rig.newer = make_image(W, H, inputFormat, VK_IMAGE_USAGE_SAMPLED_BIT);
	rig.out = make_image(W, H, outputFormat, VK_IMAGE_USAGE_STORAGE_BIT);
	set_general(rig.out);
}

static void free_rig(Rig& rig)
{
	rig.fg.Destroy();
	free_image(rig.older);
	free_image(rig.newer);
	free_image(rig.out);
}

static const char* const kQualityNames[3] = {"performance", "balanced", "best"};

// RGBA8 to A2B10G10R10 (VK_FORMAT_A2B10G10R10_UNORM_PACK32, the RGB10A2 a PS2 game's output is).
static std::vector<uint32_t> to_rgb10a2(const uint8_t* rgba, size_t pixels)
{
	std::vector<uint32_t> out(pixels);
	for (size_t i = 0; i < pixels; i++)
	{
		const uint32_t r = (uint32_t)std::lrint(rgba[i * 4 + 0] * 1023.0 / 255.0), g = (uint32_t)std::lrint(rgba[i * 4 + 1] * 1023.0 / 255.0),
					   b = (uint32_t)std::lrint(rgba[i * 4 + 2] * 1023.0 / 255.0);
		out[i] = r | (g << 10) | (b << 20) | (3u << 30);
	}
	return out;
}

static float half_to_float(uint16_t h)
{
	const int exponent = (h >> 10) & 31, fraction = h & 1023;
	const double value = (exponent == 0) ? std::ldexp(fraction, -24) : std::ldexp(1024 + fraction, exponent - 25);
	return (float)((h & 0x8000) ? -value : value);
}

static void test_frame_generation(const std::string& out, const char* fiFolder)
{
	const Shot shots[] = {
		{"a slow turn of the camera (12 pixels)", 12},
		{"a turn (48 pixels)", 48, 6},
		{"a fast turn (140 pixels)", 140, -10},
		{"a whip of the camera (230 pixels)", 230},
		{"looking up (36 pixels)", 0, -36},
		{"moving in (4%)", 0, 0, 0.04},
		{"a disc sliding across a still picture (70)", 0, 0, 0, 70, -12, 1, 1},
		{"a ball rolling across a still picture (70)", 0, 0, 0, 70, -12, 1, 0},
		{"a disc one way, the camera the other", -40, 0, 0, 60, 0, 1, 1},
		{"a ball one way, the camera the other", -40, 0, 0, 60, 0, 1, 0},
		{"the camera following a ball", 90, 0, 0, 0, 0, 1, 0},
		{"a turn while the picture fades", 30, 0, 0, 0, 0, 0, 0, 0.25},
		{"a turn of 25.4 by 3.3 (fractions of a pixel)", 25.4, 3.3},
	};
	const size_t count = std::getenv("CFX_QUICK") ? std::min<size_t>(std::atoi(std::getenv("CFX_QUICK")), sizeof(shots) / sizeof(shots[0])) :
												  sizeof(shots) / sizeof(shots[0]);
	std::printf("  drawing the scenes (%ux%u, 3x3 points a pixel)...\n", W, H);
	std::vector<std::vector<uint8_t>> older(count), newer(count), truth(count);
	std::vector<bool> wanted(count, true);
	if (const char* list = std::getenv("CFX_SHOTS")) // "6,7,8": only these (quick mode)
	{
		wanted.assign(count, false);
		for (const char* c = list; *c;)
		{
			const size_t n = (size_t)std::strtoul(c, (char**)&c, 10);
			if (n < count)
				wanted[n] = true;
			if (*c)
				c++;
		}
	}
	for (size_t i = 0; i < count; i++)
	{
		if (!wanted[i])
			continue;
		older[i].resize(BYTES);
		newer[i].resize(BYTES);
		truth[i].resize(BYTES);
		render(shots[i], 0.0, older[i].data(), W, H);
		render(shots[i], 1.0, newer[i].data(), W, H);
		render(shots[i], 0.5, truth[i].data(), W, H);
	}

	std::vector<uint8_t> made(BYTES), mixed(BYTES);
	double table[3][16] = {};
	double again[16] = {}, mix[16] = {}, fi[16] = {}, fiGame[16] = {};
	static_assert(sizeof(table[0]) / sizeof(double) >= 13);
	bool haveFi = false;
	Rig rigs[3];
	// (Quick mode, CFX_QUICK: one quality only, CFX_QUALITY or the best; no NIS.)
	const int quick = std::getenv("CFX_QUALITY") ? std::atoi(std::getenv("CFX_QUALITY")) : 2;
	const int lowest = std::getenv("CFX_QUICK") ? 2 : 0;
	for (int q = 2; q >= lowest; q--)
	{
		Rig& rig = rigs[q];
		const int quality = std::getenv("CFX_QUICK") ? quick : q;
		make_rig(rig, static_cast<Quality>(quality));
		std::printf("  %s: flow on %ux%u (%u levels), %ux%u vectors, %.1f MB of device memory\n", kQualityNames[quality], rig.fg.FlowWidth(),
			rig.fg.FlowHeight(), rig.fg.Levels(), rig.fg.FieldWidth(), rig.fg.FieldHeight(), rig.fg.MemoryBytes() / 1048576.0);
		for (size_t i = 0; i < count; i++)
		{
			if (!wanted[i])
				continue;
			const bool cut = between(rig, older[i].data(), newer[i].data(), 0.5f, made.data());
			const uint32_t margin = 8 + (uint32_t)std::ceil(std::fmax(std::fabs(shots[i].pan_x), std::fabs(shots[i].pan_y)) * 0.5 + shots[i].zoom * W * 0.25);
			table[q][i] = psnr(made.data(), truth[i].data(), W, H, margin);
			if (q == 2)
			{
				for (size_t b = 0; b < BYTES; b++)
					mixed[b] = (uint8_t)((older[i][b] + newer[i][b] + 1) / 2);
				again[i] = psnr(older[i].data(), truth[i].data(), W, H, margin);
				mix[i] = psnr(mixed.data(), truth[i].data(), W, H, margin);
				if (fiFolder)
				{
					// FSR 3's own interpolation, fed the same flow (the record just made).
					std::vector<uint8_t> fiMade(BYTES);
					if (Fsr3FiRoute::Run(fiFolder, rig.fg, rig.older, rig.newer, fiMade.data(), true))
						fiGame[i] = psnr(fiMade.data(), truth[i].data(), W, H, margin);
					if (Fsr3FiRoute::Run(fiFolder, rig.fg, rig.older, rig.newer, fiMade.data(), false))
					{
						fi[i] = psnr(fiMade.data(), truth[i].data(), W, H, margin);
						haveFi = true;
						if (i == 1 || i == 6 || i == 8)
						{
							char name[64];
							std::snprintf(name, sizeof(name), "shot%zu-fsr3fi", i);
							write_ppm(out, name, fiMade.data(), W, H);
						}
					}
				}
				if (i == 1 || i == 6 || i == 8 || i == 9 || i == 10 || std::getenv("CFX_PPM_ALL"))
				{
					char name[64];
					std::snprintf(name, sizeof(name), "shot%zu-made", i);
					write_ppm(out, name, made.data(), W, H);
					std::snprintf(name, sizeof(name), "shot%zu-truth", i);
					write_ppm(out, name, truth[i].data(), W, H);
				}
			}
			CHECK(!cut, "%s (%s): taken for a cut", shots[i].name, kQualityNames[q]);
		}
	}

	if (lowest == 2 && std::getenv("CFX_STILL"))
	{
		const Shot shot = {"still", 0, 0, 0, 0, 0, 1};
		std::vector<uint8_t> a(BYTES);
		render(shot, 0.0, a.data(), W, H);
		between(rigs[2], a.data(), a.data(), 0.5f, made.data());
		int worstDiff = 0;
		size_t bad = 0;
		for (size_t b = 0; b < BYTES; b++)
		{
			worstDiff = std::max(worstDiff, std::abs((int)made[b] - (int)a[b]));
			bad += std::abs((int)made[b] - (int)a[b]) > 1;
		}
		std::printf("    still: off by %d in %zu values\n", worstDiff, bad);
		write_ppm(out, "still-made", made.data(), W, H);
	}
	if (lowest == 2)
	{
		for (size_t i = 0; i < count; i++)
			if (wanted[i])
				std::printf("    %-44s %7.2f   (again %.2f, mixed %.2f, FSR 3 FI %.2f, with the flow as game vectors %.2f)\n", shots[i].name, table[2][i],
					again[i], mix[i], fi[i], fiGame[i]);
		free_rig(rigs[2]);
		return;
	}
	std::printf("\n  PSNR of the picture made at t = 0.5 against the scene drawn at that moment (dB):\n");
	std::printf("    %-44s %7s %7s %7s %7s %7s%s\n", "", "best", "balncd", "perf", "again", "mixed", haveFi ? "  FSR3-FI  FI+game" : "");
	double worst[3] = {99, 99, 99};
	for (size_t i = 0; i < count; i++)
	{
		std::printf("    %-44s %7.2f %7.2f %7.2f %7.2f %7.2f", shots[i].name, table[2][i], table[1][i], table[0][i], again[i], mix[i]);
		if (haveFi)
			std::printf("  %7.2f  %7.2f", fi[i], fiGame[i]);
		std::printf("\n");
		// A ball that rolls on a still picture: its pattern is nowhere in the other picture, and
		// nothing better than the two mixed can be made of it (fg_test.c's rule).
		const bool rollsAlone = shots[i].has_ball && !shots[i].slides && shots[i].pan_x == 0 && shots[i].pan_y == 0 && shots[i].zoom == 0;
		for (int q = 0; q < 3; q++)
		{
			const double need = (q == 0) ? 0.5 : 1.0;
			CHECK(table[q][i] > again[i] + need, "%s (%s): %.2f against shown again %.2f", shots[i].name, kQualityNames[q], table[q][i], again[i]);
			CHECK(table[q][i] > mix[i] + (rollsAlone ? -0.5 : need), "%s (%s): %.2f against mixed %.2f", shots[i].name, kQualityNames[q],
				table[q][i], mix[i]);
			if (!rollsAlone)
				worst[q] = std::fmin(worst[q], table[q][i] - std::fmax(again[i], mix[i]));
		}
	}
	std::printf("    the least gained over the better of the two (the ball rolling alone left out): best %.2f, balanced %.2f, performance %.2f dB\n",
		worst[2], worst[1], worst[0]);

	// Other moments: a game at 20 pictures a second on a 60 Hz screen.
	{
		const Shot shot = {"a turn, a third of the way", 60, 9};
		std::vector<uint8_t> a(BYTES), b(BYTES), truthT(BYTES);
		render(shot, 0.0, a.data(), W, H);
		render(shot, 1.0, b.data(), W, H);
		for (int third = 1; third <= 2; third++)
		{
			const double time = third / 3.0;
			render(shot, time, truthT.data(), W, H);
			between(rigs[1], a.data(), b.data(), (float)time, made.data());
			const double p = psnr(made.data(), truthT.data(), W, H, 40), pa = psnr(a.data(), truthT.data(), W, H, 40),
						 pb = psnr(b.data(), truthT.data(), W, H, 40);
			std::printf("    a turn (60 pixels) at %d/3 of the way (balanced): made %5.2f dB, the older %5.2f, the newer %5.2f\n", third, p, pa, pb);
			CHECK(p > std::fmax(pa, pb) + 1.0, "%.2f against %.2f and %.2f", p, pa, pb);
		}
	}

	// A picture that stands still comes out as it went in.
	for (int q = 0; q < 3; q++)
	{
		const Shot shot = {"still", 0, 0, 0, 0, 0, 1};
		std::vector<uint8_t> a(BYTES);
		render(shot, 0.0, a.data(), W, H);
		const bool cut = between(rigs[q], a.data(), a.data(), 0.5f, made.data());
		int worstDiff = 0;
		for (size_t b = 0; b < BYTES; b++)
			worstDiff = std::max(worstDiff, std::abs((int)made[b] - (int)a[b]));
		CHECK(worstDiff <= 1, "a still picture (%s) is off by %d", kQualityNames[q], worstDiff);
		CHECK(!cut, "a still picture (%s) was taken for a cut", kQualityNames[q]);
	}

	// After a cut the newer picture (or the older, when asked) is shown as it is.
	{
		const Shot shot = {"cut", 20, 0, 0, 0, 0, 0, 0, 0, 1};
		std::vector<uint8_t> a(BYTES), b(BYTES);
		render(shot, 0.0, a.data(), W, H);
		render(shot, 1.0, b.data(), W, H);
		for (int q = 0; q < 3; q++)
		{
			for (int showCurrent = 1; showCurrent >= 0; showCurrent--)
			{
				OpticalFlowFG::SceneChangeInfo scene = {};
				const bool cut = between(rigs[q], a.data(), b.data(), 0.5f, made.data(), showCurrent != 0, &scene);
				const std::vector<uint8_t>& expect = showCurrent ? b : a;
				size_t differing = 0;
				for (size_t i = 0; i < BYTES; i++)
					differing += (made[i] != expect[i]);
				if (q == 1)
					std::printf("    a cut to another scene (%s): FSR 3's divergence %.3f (%s), %.0f%% of blocks matched: %s; %zu values differ from the %s "
								"picture\n",
						kQualityNames[q], scene.divergence, scene.detector ? "fired" : "quiet", scene.matchedShare * 100, cut ? "a cut" : "NOT a cut",
						differing, showCurrent ? "newer" : "older");
				CHECK(cut, "a cut was not seen (%s, divergence %.3f)", kQualityNames[q], scene.divergence);
				CHECK(differing == 0, "%zu values differ after a cut (%s)", differing, kQualityNames[q]);
			}
		}
		// And the scenes that are no cut: where FSR 3's detector fires, and what the vectors say.
		for (size_t i = 0; i < count; i++)
		{
			OpticalFlowFG::SceneChangeInfo scene = {};
			between(rigs[0], older[i].data(), newer[i].data(), 0.5f, made.data(), true, &scene);
			if (scene.detector || scene.matchedShare < 0.75f)
				std::printf("    %s: FSR 3's divergence %.3f (%s), %.0f%% of blocks matched: %s\n", shots[i].name, scene.divergence,
					scene.detector ? "fired" : "quiet", scene.matchedShare * 100, scene.cut ? "a cut" : "no cut");
		}
	}

	// The pictures as RGB10A2 (what PCSX2 may hand over), and the output as RGBA16F.
	{
		Rig rig10;
		make_rig(rig10, Quality::Balanced, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT);
		const size_t i = 8; // a disc one way, the camera the other
		const std::vector<uint32_t> a = to_rgb10a2(older[i].data(), (size_t)W * H), b = to_rgb10a2(newer[i].data(), (size_t)W * H);
		upload(rig10.older, a.data(), BYTES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		upload(rig10.newer, b.data(), BYTES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		VkCommandBuffer cmd = begin();
		FrameGenInput in;
		in.previous = rig10.older.view;
		in.current = rig10.newer.view;
		in.output = rig10.out.view;
		CHECK(rig10.fg.Record(cmd, in), "record (RGB10A2 in, RGBA16F out)");
		finish(cmd);
		std::vector<uint16_t> halfs((size_t)W * H * 4);
		download(rig10.out, VK_IMAGE_LAYOUT_GENERAL, halfs.data(), halfs.size() * 2);
		std::vector<uint8_t> asBytes(BYTES);
		for (size_t k = 0; k < halfs.size(); k++)
			asBytes[k] = (uint8_t)std::lrint(std::fmin(std::fmax(half_to_float(halfs[k]), 0.0f), 1.0f) * 255.0f);
		const uint32_t margin = 8 + 20;
		const double p = psnr(asBytes.data(), truth[i].data(), W, H, margin);
		std::printf("    RGB10A2 pictures in, RGBA16F out (balanced, %s): %.2f dB (RGBA8: %.2f)\n", shots[i].name, p, table[1][i]);
		CHECK(std::fabs(p - table[1][i]) < 0.5, "RGB10A2/RGBA16F %.2f against RGBA8 %.2f", p, table[1][i]);
		free_rig(rig10);
	}

	// What it took (a CPU's software driver: only that it runs) and what it is made of.
	for (int q = 2; q >= 0; q--)
	{
		Rig& rig = rigs[q];
		std::printf("  %s, %ux%u, per record on lavapipe: %.0f ms wall;", kQualityNames[q], W, H, rig.wallMs / (rig.timed ? rig.timed : 1));
		for (size_t p = 0; p < rig.passMs.size(); p++)
			std::printf(" %s %.0f", OpticalFlowFG::TimestampName((uint32_t)p + 1), rig.passMs[p] / (rig.timed ? rig.timed : 1));
		std::printf(" ms\n");
		uint64_t threads = 0;
		for (const OpticalFlowFG::PassCount& pass : rig.fg.Workload())
		{
			std::printf("    %-18s %3u dispatches %8llu workgroups of %3u\n", pass.name, pass.dispatches, (unsigned long long)pass.workgroups,
				pass.threadsPerGroup);
			threads += pass.workgroups * pass.threadsPerGroup;
		}
		std::printf("    %llu threads in all\n", (unsigned long long)threads);
	}
	for (Rig& rig : rigs)
		free_rig(rig);
	Fsr3FiRoute::Release();
}

// ---- NIS

// A pattern with every kind of detail at once, drawn at any size from the same picture: rings
// whose spacing tightens outward, hard-edged squares, a thin diagonal line and soft grain; (u, v)
// in parts of the picture's height. The rings stay under 0.3 cycles a pixel of the smallest input
// (287 lines): finer rings would already be wrong (aliased) in the picture NIS is given, and
// sharpening a wrong pattern is no test of a scaler.
static void pattern(double u, double v, double rgb[3])
{
	const double r = std::sqrt((u - 0.45) * (u - 0.45) + (v - 0.5) * (v - 0.5));
	const double rings = 0.5 + 0.5 * std::cos(r * r * 340.0);
	double base = 0.25 + 0.5 * rings;
	if (u > 1.0 && u < 1.6 && v > 0.15 && v < 0.85)
		base = ((int)std::floor(u * 12) + (int)std::floor(v * 12)) & 1 ? 0.9 : 0.1;
	if (std::fabs((u - 0.1) - (v * 0.8)) < 0.004)
		base = 0.95;
	const double g = grain(u, v, 60);
	rgb[0] = base * 0.9 + 0.1 * g;
	rgb[1] = base * 0.7 + 0.2 * g;
	rgb[2] = base * 0.5 + 0.3;
}

static void draw_pattern(uint8_t* pixels, uint32_t w, uint32_t h)
{
	for (uint32_t y = 0; y < h; y++)
	{
		for (uint32_t x = 0; x < w; x++)
		{
			double sum[3] = {0, 0, 0};
			for (int sy = 0; sy < 4; sy++)
			{
				for (int sx = 0; sx < 4; sx++)
				{
					double rgb[3];
					pattern((x + (sx + 0.5) / 4) / h, (y + (sy + 0.5) / 4) / h, rgb);
					for (int c = 0; c < 3; c++)
						sum[c] += rgb[c];
				}
			}
			uint8_t* const p = pixels + ((size_t)y * w + x) * 4;
			for (int c = 0; c < 3; c++)
				p[c] = (uint8_t)std::lrint(std::fmin(std::fmax(sum[c] / 16, 0), 1) * 255);
			p[3] = 255;
		}
	}
}

// A bilinear stretch, as a GPU's linear sampler does it (pixel centres, edges clamped).
static void bilinear(const uint8_t* in, uint32_t iw, uint32_t ih, uint8_t* out, uint32_t ow, uint32_t oh)
{
	for (uint32_t y = 0; y < oh; y++)
	{
		const double sy = std::fmin(std::fmax((y + 0.5) * ih / oh - 0.5, 0.0), ih - 1.0);
		const uint32_t y0 = (uint32_t)sy, y1 = std::min(y0 + 1, ih - 1);
		const double fy = sy - y0;
		for (uint32_t x = 0; x < ow; x++)
		{
			const double sx = std::fmin(std::fmax((x + 0.5) * iw / ow - 0.5, 0.0), iw - 1.0);
			const uint32_t x0 = (uint32_t)sx, x1 = std::min(x0 + 1, iw - 1);
			const double fx = sx - x0;
			for (int c = 0; c < 4; c++)
			{
				const double top = in[((size_t)y0 * iw + x0) * 4 + c] * (1 - fx) + in[((size_t)y0 * iw + x1) * 4 + c] * fx;
				const double bottom = in[((size_t)y1 * iw + x0) * 4 + c] * (1 - fx) + in[((size_t)y1 * iw + x1) * 4 + c] * fx;
				out[((size_t)y * ow + x) * 4 + c] = (uint8_t)std::lrint(top * (1 - fy) + bottom * fy);
			}
		}
	}
}

// How sharp a picture is: the mean of |luma gradient|.
static double sharpness_of(const uint8_t* p, uint32_t w, uint32_t h)
{
	double sum = 0;
	for (uint32_t y = 1; y < h - 1; y++)
	{
		for (uint32_t x = 1; x < w - 1; x++)
		{
			auto Y = [&](uint32_t xx, uint32_t yy) {
				const uint8_t* q = p + ((size_t)yy * w + xx) * 4;
				return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
			};
			sum += std::fabs(Y(x + 1, y) - Y(x - 1, y)) + std::fabs(Y(x, y + 1) - Y(x, y - 1));
		}
	}
	return sum / ((double)(w - 2) * (h - 2));
}

// NIS from `in` (iw x ih) to `ow` x `oh`, in an output image `imageW` x `imageH` filled with a mark
// first (so a write outside the output shows).
static std::vector<uint8_t> run_nis(NIS& nis, const uint8_t* in, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, float sharpness, double* ms,
	uint32_t imageW = 0, uint32_t imageH = 0, std::vector<uint8_t>* wholeImage = nullptr)
{
	imageW = imageW ? imageW : ow;
	imageH = imageH ? imageH : oh;
	TestImage input = make_image(iw, ih, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
	TestImage output = make_image(imageW, imageH, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
	upload(input, in, (size_t)iw * ih * 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	std::vector<uint8_t> mark((size_t)imageW * imageH * 4, 0x5a);
	upload(output, mark.data(), mark.size(), VK_IMAGE_LAYOUT_GENERAL);
	VkCommandBuffer cmd = begin();
	NISInput ni;
	ni.input = input.view;
	ni.inputWidth = iw;
	ni.inputHeight = ih;
	ni.output = output.view;
	ni.outputWidth = ow;
	ni.outputHeight = oh;
	ni.sharpness = sharpness;
	Timestamps ts;
	ts.pool = s_queries;
	CHECK(nis.Record(cmd, ni, &ts), "NIS record %ux%u -> %ux%u", iw, ih, ow, oh);
	finish(cmd);
	uint64_t stamps[2];
	if (ms && vkGetQueryPoolResults(s_device, s_queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
				  VK_SUCCESS)
		*ms = (double)(stamps[1] - stamps[0]) * s_tickNs * 1e-6;
	std::vector<uint8_t> whole((size_t)imageW * imageH * 4);
	download(output, VK_IMAGE_LAYOUT_GENERAL, whole.data(), whole.size());
	std::vector<uint8_t> out((size_t)ow * oh * 4);
	for (uint32_t y = 0; y < oh; y++)
		std::memcpy(&out[(size_t)y * ow * 4], &whole[(size_t)y * imageW * 4], (size_t)ow * 4);
	if (wholeImage)
		*wholeImage = whole;
	free_image(input);
	free_image(output);
	return out;
}

static void test_nis(const std::string& out, bool half)
{
	NIS nis;
	Device d = device_info();
	d.shaderFloat16 = half;
	if (!nis.Create(d, VK_FORMAT_R8G8B8A8_UNORM))
	{
		std::printf("FAILED NIS::Create: %s\n", nis.LastError());
		s_failed++;
		return;
	}
	std::printf("  NIS (%s precision):\n", nis.UsesHalfPrecision() ? "half" : "full");
	struct Case
	{
		uint32_t iw, ih, ow, oh;
	};
	// 2x and 1.5x of a 640x360 picture, and 1.5x of an odd size whose output ends inside a block.
	const Case cases[] = {{640, 360, 1280, 720}, {640, 360, 960, 540}, {501, 287, 751, 430}};
	for (const Case& c : cases)
	{
		std::vector<uint8_t> small((size_t)c.iw * c.ih * 4), truth((size_t)c.ow * c.oh * 4), stretched((size_t)c.ow * c.oh * 4);
		draw_pattern(small.data(), c.iw, c.ih);
		// (The truth is the same pattern drawn at the output's size: its height is the unit.)
		draw_pattern(truth.data(), c.ow, c.oh);
		bilinear(small.data(), c.iw, c.ih, stretched.data(), c.ow, c.oh);
		double ms = 0;
		std::vector<uint8_t> whole;
		const uint32_t imageW = c.ow + 40, imageH = c.oh + 30;
		// The scaler alone (sharpness 0: NVIDIA's slider at no sharpening) is held against the truth;
		// at 0.5 (NVIDIA's default) it sharpens on top, which a PSNR counts against it even where it
		// looks better, so there only the sharpening is checked.
		const std::vector<uint8_t> scaled = run_nis(nis, small.data(), c.iw, c.ih, c.ow, c.oh, 0.0f, &ms, imageW, imageH, &whole);
		const std::vector<uint8_t> sharpened = run_nis(nis, small.data(), c.iw, c.ih, c.ow, c.oh, 0.5f, nullptr);
		const double pNis = psnr(scaled.data(), truth.data(), c.ow, c.oh, 4), pBil = psnr(stretched.data(), truth.data(), c.ow, c.oh, 4),
					 pSharp = psnr(sharpened.data(), truth.data(), c.ow, c.oh, 4);
		const double sNis = sharpness_of(scaled.data(), c.ow, c.oh), sBil = sharpness_of(stretched.data(), c.ow, c.oh),
					 sTruth = sharpness_of(truth.data(), c.ow, c.oh), sSharp = sharpness_of(sharpened.data(), c.ow, c.oh);
		std::printf("    %ux%u -> %ux%u: NIS %.2f dB (at 0.5: %.2f), bilinear %.2f dB; sharpness NIS %.1f (at 0.5: %.1f), bilinear %.1f, truth %.1f; "
					"%.1f ms on lavapipe\n",
			c.iw, c.ih, c.ow, c.oh, pNis, pSharp, pBil, sNis, sSharp, sBil, sTruth, ms);
		CHECK(pNis > pBil + 0.5, "NIS %.2f dB against bilinear %.2f", pNis, pBil);
		CHECK(sNis > sBil * 1.05, "NIS sharpness %.2f against bilinear %.2f", sNis, sBil);
		CHECK(sSharp > sNis * 1.1, "NIS at 0.5 sharpness %.2f against %.2f at 0", sSharp, sNis);
		// Nothing written outside the output, and its last row and column are the picture's.
		size_t outside = 0;
		for (uint32_t y = 0; y < imageH; y++)
			for (uint32_t x = 0; x < imageW; x++)
				if (x >= c.ow || y >= c.oh)
					for (int k = 0; k < 4; k++)
						outside += (whole[((size_t)y * imageW + x) * 4 + k] != 0x5a);
		CHECK(outside == 0, "%zu values written outside the %ux%u output", outside, c.ow, c.oh);
		double edgeErr = 0;
		for (uint32_t x = 0; x < c.ow; x++)
			for (uint32_t y : {0u, c.oh - 1})
				for (int k = 0; k < 3; k++)
					edgeErr += std::fabs((double)scaled[((size_t)y * c.ow + x) * 4 + k] - truth[((size_t)y * c.ow + x) * 4 + k]);
		for (uint32_t y = 0; y < c.oh; y++)
			for (uint32_t x : {0u, c.ow - 1})
				for (int k = 0; k < 3; k++)
					edgeErr += std::fabs((double)scaled[((size_t)y * c.ow + x) * 4 + k] - truth[((size_t)y * c.ow + x) * 4 + k]);
		edgeErr /= (2.0 * (c.ow + c.oh) * 3);
		std::printf("      the outermost rows and columns differ from the truth by %.1f on average (of 255)\n", edgeErr);
		CHECK(edgeErr < 24.0, "the edges are off by %.1f", edgeErr);
		if (c.ow == 1280 && !half)
		{
			write_ppm(out, "nis-2x", scaled.data(), c.ow, c.oh);
			write_ppm(out, "nis-2x-sharpness-0.5", sharpened.data(), c.ow, c.oh);
			write_ppm(out, "nis-2x-bilinear", stretched.data(), c.ow, c.oh);
			write_ppm(out, "nis-2x-truth", truth.data(), c.ow, c.oh);
		}
	}
	// A flat colour stays flat, scaled or sharpened, at every sharpness.
	for (float sharpness : {0.0f, 0.5f, 1.0f})
	{
		std::vector<uint8_t> flat((size_t)320 * 200 * 4);
		for (size_t i = 0; i < flat.size(); i += 4)
		{
			flat[i] = 77;
			flat[i + 1] = 154;
			flat[i + 2] = 204;
			flat[i + 3] = 255;
		}
		for (int scale = 0; scale < 2; scale++)
		{
			const uint32_t ow = scale ? 640 : 320, oh = scale ? 400 : 200;
			const std::vector<uint8_t> o = run_nis(nis, flat.data(), 320, 200, ow, oh, sharpness, nullptr);
			int worst = 0;
			for (size_t i = 0; i < o.size(); i += 4)
				for (int k = 0; k < 3; k++)
					worst = std::max(worst, std::abs((int)o[i + k] - (int)flat[k]));
			CHECK(worst <= 1, "a flat colour (%s, sharpness %.1f) is off by %d", scale ? "2x" : "sharpen only", sharpness, worst);
		}
	}
	// Sharpening alone (1:1): sharper, and the same size.
	{
		std::vector<uint8_t> pic((size_t)640 * 360 * 4);
		draw_pattern(pic.data(), 640, 360);
		double ms = 0;
		const std::vector<uint8_t> sharp = run_nis(nis, pic.data(), 640, 360, 640, 360, 0.8f, &ms);
		const double before = sharpness_of(pic.data(), 640, 360), after = sharpness_of(sharp.data(), 640, 360);
		std::printf("    sharpening only (640x360, 0.8): sharpness %.2f -> %.2f, %.2f dB from the input; %.1f ms\n", before, after,
			psnr(sharp.data(), pic.data(), 640, 360, 0), ms);
		CHECK(after > before, "sharpening made it %.2f from %.2f", after, before);
	}
	// What NIS cannot do it says so, and records nothing.
	{
		VkCommandBuffer cmd = begin();
		NISInput ni;
		ni.input = ni.output = VK_NULL_HANDLE;
		CHECK(!nis.Record(cmd, ni), "a record with no images");
		finish(cmd);
	}
	nis.Destroy();
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: computefx_test <out folder> [--compile-only] [--fsr3-fi <folder with its .spv>]\n");
		return 2;
	}
	const std::string out = argv[1];
	bool compileOnly = false;
	const char* fiFolder = nullptr;
	for (int i = 2; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--compile-only"))
			compileOnly = true;
		else if (!std::strcmp(argv[i], "--fsr3-fi") && i + 1 < argc)
			fiFolder = argv[++i];
	}

	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instance = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance.pApplicationInfo = &app;
	VK(vkCreateInstance(&instance, nullptr, &s_instance));
	uint32_t count = 1;
	vkEnumeratePhysicalDevices(s_instance, &count, &s_gpu);
	if (count == 0)
	{
		std::fprintf(stderr, "no Vulkan device\n");
		return 2;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(s_gpu, &props);
	std::printf("  on: %s (Vulkan %u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
	s_tickNs = props.limits.timestampPeriod;
	VkQueueFamilyProperties families[16];
	count = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &count, families);
	for (s_family = 0; s_family < count && !(families[s_family].queueFlags & VK_QUEUE_COMPUTE_BIT); s_family++)
	{
	}
	// Half precision for NIS where the device has it (turned on, as the title would have to).
	VkPhysicalDeviceVulkan12Features have12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceFeatures2 have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	have.pNext = &have12;
	if (VK_API_VERSION_MINOR(props.apiVersion) >= 2)
		vkGetPhysicalDeviceFeatures2(s_gpu, &have);
	VkPhysicalDeviceVulkan12Features want12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	want12.shaderFloat16 = have12.shaderFloat16;
	s_float16 = have12.shaderFloat16 != 0;
	// (For FSR 3's frame interpolation only, which the effects themselves do not need: its storage
	// formats and its loads of an image without a format.)
	VkPhysicalDeviceFeatures2 want = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	want.pNext = &want12;
	VkPhysicalDeviceFeatures core;
	vkGetPhysicalDeviceFeatures(s_gpu, &core);
	want.features.shaderStorageImageExtendedFormats = core.shaderStorageImageExtendedFormats;
	want.features.shaderStorageImageReadWithoutFormat = core.shaderStorageImageReadWithoutFormat;
	want.features.shaderStorageImageWriteWithoutFormat = core.shaderStorageImageWriteWithoutFormat;
	const float priority = 1;
	VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue.queueFamilyIndex = s_family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	VkDeviceCreateInfo device = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	if (VK_API_VERSION_MINOR(props.apiVersion) < 2)
		want.pNext = nullptr;
	device.pNext = &want;
	device.queueCreateInfoCount = 1;
	device.pQueueCreateInfos = &queue;
	VK(vkCreateDevice(s_gpu, &device, nullptr, &s_device));
	vkGetDeviceQueue(s_device, s_family, 0, &s_queue);
	VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = s_family;
	VK(vkCreateCommandPool(s_device, &pool, nullptr, &s_pool));
	VkQueryPoolCreateInfo queries = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queries.queryCount = 16;
	VK(vkCreateQueryPool(s_device, &queries, nullptr, &s_queries));

	if (compileOnly)
	{
		// Every pipeline of both effects, every variant this device can have.
		for (VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT})
		{
			OpticalFlowFG fg;
			CHECK(fg.Create(device_info(), W, H, format, Quality::Balanced), "OpticalFlowFG: %s", fg.LastError());
			for (bool half : {false, true})
			{
				Device d = device_info();
				d.shaderFloat16 = half && s_float16;
				NIS nis;
				CHECK(nis.Create(d, format), "NIS: %s", nis.LastError());
			}
		}
		std::printf("  the compute effects' pipelines were all made by: %s\n", props.deviceName);
		std::printf("compute effects: %d checks, %d failed\n", s_checks, s_failed);
		return s_failed ? 1 : 0;
	}

	if (!std::getenv("CFX_NO_FG"))
		test_frame_generation(out, fiFolder);
	if (std::getenv("CFX_QUICK") && !std::getenv("CFX_NO_FG"))
		return s_failed ? 1 : 0;
	test_nis(out, false);
	if (s_float16)
		test_nis(out, true);
	else
		std::printf("  NIS half precision: not tried (the device has no shaderFloat16)\n");

	vkDestroyQueryPool(s_device, s_queries, nullptr);
	vkDestroyCommandPool(s_device, s_pool, nullptr);
	vkDestroyDevice(s_device, nullptr);
	vkDestroyInstance(s_instance, nullptr);
	std::printf("compute effects: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed ? 1 : 0;
}

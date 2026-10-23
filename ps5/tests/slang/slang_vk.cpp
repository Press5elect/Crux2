// Crux2 - libretro presets run on lavapipe through ChainPlan (chain_runner.h): validation, plausible pictures, cost.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   slang_vk <work folder> <root> <preset.slangp>... [--count] [--compile-only]
//
// Each preset is read (ps5_slang.cpp), each pass's stages compiled as the title will have them
// (glslangValidator for Vulkan 1.0, then spirv-opt -O: shaderc's "performance" level), and run on
// a test picture for 5 frames (feedback and history fill) at 640x448 -> 3840x2160 and at
// 512x448 -> 1920x1080, with the Khronos validation layer and its synchronization checks when
// they are installed. Checked: no validation message, no NaN or infinity in any pass's picture,
// the output neither black nor flat and its brightness within reason of the input's. The output
// of the last frame is saved as <work>/<preset>-<size>.png, the input as input-<size>.png.
// lavapipe's time for each pass at 4K is printed (a CPU's: only the ratios mean anything).
//
// --count: the fragment stages are compiled again with every texture fetch counted (an atomic
// add into a storage buffer), run at 640x448 -> 1920x1080; the fetches per pixel of each pass
// give the fetches a frame costs at 4K, the figure the report's GPU estimate is made from.

#include "chain_runner.h"
#include "ps5_slang.h"

#include <png.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

using namespace PS5Slang;

namespace
{
std::string g_work;
bool g_compileOnly = false; // --compile-only: pipelines made, nothing run (the console's compiler on a PC with no GPU)
int g_validationErrors = 0, g_failures = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		if (g_validationErrors++ < 20)
			std::printf("    VALIDATION: %s\n", data->pMessage);
	}
	return VK_FALSE;
}

struct Vk
{
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	ChainRunner::Device d;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	float timestampPeriod = 1.f;
	bool validation = false;
	std::string deviceName;

	bool Init()
	{
		uint32_t n = 0;
		vkEnumerateInstanceLayerProperties(&n, nullptr);
		std::vector<VkLayerProperties> layers(n);
		vkEnumerateInstanceLayerProperties(&n, layers.data());
		for (const VkLayerProperties& l : layers)
			validation |= std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "crux2-slang-test";
		app.apiVersion = VK_API_VERSION_1_1;
		const char* layer = "VK_LAYER_KHRONOS_validation";
		const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		const VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
		VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
		features.enabledValidationFeatureCount = 1;
		features.pEnabledValidationFeatures = enables;
		VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		info.pApplicationInfo = &app;
		if (validation)
		{
			info.enabledLayerCount = 1;
			info.ppEnabledLayerNames = &layer;
			info.enabledExtensionCount = 1;
			info.ppEnabledExtensionNames = &extension;
			info.pNext = &features;
		}
		if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
			return false;
		if (validation)
		{
			auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
			VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
			m.pfnUserCallback = OnMessage;
			if (create)
				create(instance, &m, nullptr, &messenger);
		}
		n = 0;
		vkEnumeratePhysicalDevices(instance, &n, nullptr);
		if (n == 0)
			return false;
		std::vector<VkPhysicalDevice> devices(n);
		vkEnumeratePhysicalDevices(instance, &n, devices.data());
		d.physical = devices[0];
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(d.physical, &props);
		deviceName = props.deviceName;
		timestampPeriod = props.limits.timestampPeriod;
		d.uniformAlign = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
		vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memory);
		n = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &n, nullptr);
		std::vector<VkQueueFamilyProperties> families(n);
		vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &n, families.data());
		for (family = 0; family < n && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++)
		{
		}
		const float priority = 1.f;
		VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		q.queueFamilyIndex = family;
		q.queueCount = 1;
		q.pQueuePriorities = &priority;
		VkPhysicalDeviceFeatures want{};
		want.fragmentStoresAndAtomics = VK_TRUE; // for --count
		VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		di.queueCreateInfoCount = 1;
		di.pQueueCreateInfos = &q;
		di.pEnabledFeatures = &want;
		if (vkCreateDevice(d.physical, &di, nullptr, &d.device) != VK_SUCCESS)
			return false;
		vkGetDeviceQueue(d.device, family, 0, &queue);
		VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pi.queueFamilyIndex = family;
		vkCreateCommandPool(d.device, &pi, nullptr, &pool);
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		vkAllocateCommandBuffers(d.device, &ai, &cmd);
		VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		vkCreateFence(d.device, &fi, nullptr, &fence);
		return true;
	}
	void Begin()
	{
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
	}
	void Submit()
	{
		vkEndCommandBuffer(cmd);
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		vkQueueSubmit(queue, 1, &si, fence);
		vkWaitForFences(d.device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(d.device, 1, &fence);
	}
	void Done()
	{
		if (!d.device)
			return;
		vkDeviceWaitIdle(d.device);
		vkDestroyFence(d.device, fence, nullptr);
		vkDestroyCommandPool(d.device, pool, nullptr);
		vkDestroyDevice(d.device, nullptr);
		if (messenger)
			reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(
				instance, messenger, nullptr);
		vkDestroyInstance(instance, nullptr);
	}
} g_vk;

// ------------------------------------------------------------------------------------------------
// Files, the compiler, PNG.

bool ReadAll(const std::string& path, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buffer[65536];
	out.clear();
	size_t n;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		out.append(buffer, n);
	std::fclose(f);
	return true;
}

bool WriteAll(const std::string& path, const std::string& text)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	return std::fclose(f) == 0 && ok;
}

// Every texture fetch of a fragment stage counted (the test's cost measurement): the fetch
// functions renamed, wrappers that count put after the #line that follows #version.
std::string Instrument(const std::string& glsl, size_t pass)
{
	static const std::regex fetch(R"(\b(texture|textureLod|texelFetch|textureGrad|textureOffset|textureLodOffset|texelFetchOffset)\b)");
	const std::string body = std::regex_replace(glsl, fetch, "crux_$1");
	const size_t at = body.find("#extension GL_GOOGLE_cpp_style_line_directive : require\n");
	if (at == std::string::npos)
		return glsl;
	const size_t after = body.find('\n', body.find('\n', at) + 1) + 1; // past the #line too
	std::string header = "layout(set = 1, binding = 0) buffer CruxFetches { uint fetches[]; } crux_fetches;\n"
						 "uint crux_count() { atomicAdd(crux_fetches.fetches[" + std::to_string(pass) + "u * 64u + (uint(gl_FragCoord.x) & 63u)], 1u); return 0u; }\n"
						 "vec4 crux_texture(sampler2D s, vec2 c) { crux_count(); return texture(s, c); }\n"
						 "vec4 crux_texture(sampler2D s, vec2 c, float b) { crux_count(); return texture(s, c, b); }\n"
						 "vec4 crux_textureLod(sampler2D s, vec2 c, float l) { crux_count(); return textureLod(s, c, l); }\n"
						 "vec4 crux_texelFetch(sampler2D s, ivec2 c, int l) { crux_count(); return texelFetch(s, c, l); }\n"
						 "vec4 crux_textureGrad(sampler2D s, vec2 c, vec2 x, vec2 y) { crux_count(); return textureGrad(s, c, x, y); }\n"
						 "#define crux_textureOffset(s, c, o) (crux_count(), textureOffset(s, c, o))\n"
						 "#define crux_textureLodOffset(s, c, l, o) (crux_count(), textureLodOffset(s, c, l, o))\n"
						 "#define crux_texelFetchOffset(s, c, l, o) (crux_count(), texelFetchOffset(s, c, l, o))\n";
	// Keep the line numbers of what follows.
	return body.substr(0, after) + header + body.substr(after);
}

std::vector<uint32_t> Compile(const std::string& name, const std::string& glsl, bool vertex, size_t pass, bool count, bool& counted)
{
	const std::string base = g_work + "/spv/" + name + "-p" + std::to_string(pass) + (count ? "-count" : "") + (vertex ? ".vert" : ".frag");
	std::string text = glsl;
	if (count && !vertex)
		text = Instrument(glsl, pass);
	for (int attempt = 0; attempt < 2; attempt++)
	{
		WriteAll(base, text);
		const std::string cmd = "glslangValidator -V --target-env vulkan1.0 -o " + base + ".spv " + base + " > " + base + ".log 2>&1 && spirv-opt -O " +
								base + ".spv -o " + base + ".opt.spv >> " + base + ".log 2>&1";
		if (std::system(cmd.c_str()) == 0)
		{
			std::string spv;
			ReadAll(base + ".opt.spv", spv);
			std::vector<uint32_t> words(spv.size() / 4);
			std::memcpy(words.data(), spv.data(), words.size() * 4);
			counted = text != glsl;
			return words;
		}
		if (text == glsl)
			break;
		text = glsl; // the counting did not compile: the plain stage, uncounted
	}
	std::printf("    FAIL: %s does not compile (%s.log)\n", base.c_str(), base.c_str());
	return {};
}

bool DecodePng(const std::string& data, std::vector<uint8_t>& pixels, uint32_t& w, uint32_t& h)
{
	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, data.data(), data.size()))
		return false;
	image.format = PNG_FORMAT_RGBA;
	pixels.resize(PNG_IMAGE_SIZE(image));
	if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr))
		return false;
	w = image.width;
	h = image.height;
	return true;
}

void WritePng(const std::string& path, const std::vector<float>& rgba, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
	for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
		for (int c = 0; c < 3; c++)
		{
			const float v = rgba[i * 4 + c];
			rgb[i * 3 + c] = static_cast<uint8_t>(std::lround(255.f * std::clamp(std::isfinite(v) ? v : 0.f, 0.f, 1.f)));
		}
	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	image.width = w;
	image.height = h;
	image.format = PNG_FORMAT_RGB;
	png_image_write_to_file(&image, path.c_str(), 0, rgb.data(), 0, nullptr);
}

// ------------------------------------------------------------------------------------------------
// The test picture: what a PlayStation 2 game's frame has in it. Colour bars, ramps, fine lines
// and checkers (interlace flicker and dot crawl show there), a disc with skin and sky tones.

std::vector<uint8_t> TestPicture(uint32_t w, uint32_t h)
{
	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
	static const uint8_t bars[8][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191}, {191, 0, 0}, {0, 0, 191}, {16, 16, 16}};
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
		{
			const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			float r, g, b;
			if (v < 0.25f)
			{
				const uint8_t* c = bars[std::min<uint32_t>(x * 8 / w, 7)];
				r = c[0] / 255.f, g = c[1] / 255.f, b = c[2] / 255.f;
			}
			else if (v < 0.375f)
				r = g = b = u;
			else if (v < 0.5f)
			{
				const int band = static_cast<int>((v - 0.375f) * 24.f);
				r = band == 0 ? u : 0.f, g = band == 1 ? u : 0.f, b = band == 2 ? u : 0.f;
			}
			else if (v < 0.75f)
			{
				const int region = static_cast<int>(u * 4.f);
				bool on = false;
				if (region == 0)
					on = x % 2 == 0;                  // one-pixel lines
				else if (region == 1)
					on = (x + y) % 2 == 0;            // one-pixel checker
				else if (region == 2)
					on = (x / 2 + y / 2) % 2 == 0;    // two-pixel checker
				else
					on = ((x + y) / 3) % 3 == 0;      // diagonals
				r = g = b = on ? 0.9f : 0.1f;
				if (region == 3)
					r = on ? 0.95f : 0.05f, g = on ? 0.2f : 0.05f, b = on ? 0.2f : 0.3f;
			}
			else
			{
				const float dx = (u - 0.5f) * w / h, dy = v - 0.875f;
				const bool disc = dx * dx + dy * dy < 0.012f;
				r = disc ? 0.92f : 0.25f * u + 0.1f;
				g = disc ? 0.72f : 0.45f * u + 0.2f;
				b = disc ? 0.58f : 0.85f;
			}
			uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
			p[0] = static_cast<uint8_t>(std::lround(r * 255.f));
			p[1] = static_cast<uint8_t>(std::lround(g * 255.f));
			p[2] = static_cast<uint8_t>(std::lround(b * 255.f));
			p[3] = 255;
		}
	return px;
}

float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
	float v;
	if (exponent == 0)
		v = std::ldexp(static_cast<float>(mantissa), -24);
	else if (exponent == 31)
		v = mantissa ? NAN : INFINITY;
	else
		v = std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
	return sign ? -v : v;
}

// A picture's first level as RGBA floats (false: a format this test does not read).
bool ReadBack(const ChainRunner::Image& image, std::vector<float>& out)
{
	uint32_t bytes = 0;
	switch (image.format)
	{
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: bytes = 4; break;
		case VK_FORMAT_R16G16B16A16_SFLOAT: bytes = 8; break;
		case VK_FORMAT_R32G32B32A32_SFLOAT: bytes = 16; break;
		default: return false;
	}
	const VkDeviceSize size = static_cast<VkDeviceSize>(image.width) * image.height * bytes;
	VkBuffer buffer;
	VkDeviceMemory memory;
	void* map = nullptr;
	if (!ChainRunner::MakeBuffer(g_vk.d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffer, memory, &map))
		return false;
	g_vk.Begin();
	ChainRunner::Barrier(g_vk.cmd, image.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {image.width, image.height, 1};
	vkCmdCopyImageToBuffer(g_vk.cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
	ChainRunner::Barrier(g_vk.cmd, image.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT);
	g_vk.Submit();
	const size_t n = static_cast<size_t>(image.width) * image.height;
	out.resize(n * 4);
	const uint8_t* src = static_cast<const uint8_t*>(map);
	for (size_t i = 0; i < n; i++)
		for (int c = 0; c < 4; c++)
		{
			float v = 0.f;
			if (bytes == 4 && image.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
			{
				uint32_t w;
				std::memcpy(&w, src + i * 4, 4);
				v = c < 3 ? ((w >> (10 * c)) & 1023) / 1023.f : (w >> 30) / 3.f;
			}
			else if (bytes == 4)
				v = src[i * 4 + c] / 255.f;
			else if (bytes == 8)
			{
				uint16_t h;
				std::memcpy(&h, src + i * 8 + c * 2, 2);
				v = HalfToFloat(h);
			}
			else
				std::memcpy(&v, src + i * 16 + c * 4, 4);
			out[i * 4 + c] = v;
		}
	vkDestroyBuffer(g_vk.d.device, buffer, nullptr);
	vkFreeMemory(g_vk.d.device, memory, nullptr);
	return true;
}

struct Stats
{
	double mean = 0, deviation = 0, max = 0;
	size_t bad = 0; // NaN or infinity
};

Stats Measure(const std::vector<float>& rgba)
{
	Stats s;
	const size_t n = rgba.size() / 4;
	double sum = 0, sum2 = 0;
	for (size_t i = 0; i < n; i++)
	{
		const float r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
		if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
		{
			s.bad++;
			continue;
		}
		const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		sum += y;
		sum2 += y * y;
		s.max = std::max(s.max, y);
	}
	const size_t good = std::max<size_t>(n - s.bad, 1);
	s.mean = sum / good;
	s.deviation = std::sqrt(std::max(0.0, sum2 / good - s.mean * s.mean));
	return s;
}

std::string Safe(const std::string& path)
{
	std::string name = path.substr(path.rfind('/') + 1);
	if (name.size() > 7 && name.compare(name.size() - 7, 7, ".slangp") == 0)
		name.resize(name.size() - 7);
	for (char& c : name)
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
			c = '_';
	return name;
}

// The game's picture: an RGBA8 image, readable, a copy source (history, mipmaps).
bool MakeInput(uint32_t w, uint32_t h, ChainRunner::Image& image, std::vector<float>& asFloats)
{
	const std::vector<uint8_t> px = TestPicture(w, h);
	asFloats.resize(px.size());
	for (size_t i = 0; i < px.size(); i++)
		asFloats[i] = px[i] / 255.f;
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent = {w, h, 1};
	info.mipLevels = info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (vkCreateImage(g_vk.d.device, &info, nullptr, &image.image) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g_vk.d.device, image.image, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = ChainRunner::MemoryType(g_vk.d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vkAllocateMemory(g_vk.d.device, &alloc, nullptr, &image.memory);
	vkBindImageMemory(g_vk.d.device, image.image, image.memory, 0);
	VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view.image = image.image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = info.format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCreateImageView(g_vk.d.device, &view, nullptr, &image.view);
	image.width = w;
	image.height = h;
	image.format = info.format;
	VkBuffer buffer;
	VkDeviceMemory memory;
	void* map = nullptr;
	ChainRunner::MakeBuffer(g_vk.d, px.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, memory, &map);
	std::memcpy(map, px.data(), px.size());
	g_vk.Begin();
	ChainRunner::Barrier(g_vk.cmd, image.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {w, h, 1};
	vkCmdCopyBufferToImage(g_vk.cmd, buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	ChainRunner::Barrier(g_vk.cmd, image.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
	g_vk.Submit();
	vkDestroyBuffer(g_vk.d.device, buffer, nullptr);
	vkFreeMemory(g_vk.d.device, memory, nullptr);
	return true;
}

struct Size
{
	uint32_t w, h, vw, vh;
};

// One preset at one size, `frames` frames; the checks on the last.
void RunPreset(const std::string& path, const std::string& root, const Size& size, bool countFetches, std::vector<double>* fetchesPerPixel)
{
	LoadOptions options;
	options.allowedRoots.push_back(root);
	Preset preset;
	std::string error;
	if (!LoadPreset(path, options, preset, error))
	{
		std::printf("    FAIL: %s is not read: %s\n", path.c_str(), error.c_str());
		g_failures++;
		return;
	}
	const std::string name = Safe(path);
	std::vector<std::vector<uint8_t>> lutPixels(preset.luts.size());
	for (size_t i = 0; i < preset.luts.size(); i++)
		if (!DecodePng(preset.luts[i].data, lutPixels[i], preset.luts[i].width, preset.luts[i].height))
		{
			std::printf("    FAIL: %s: the lookup picture %s cannot be decoded\n", name.c_str(), preset.luts[i].path.c_str());
			g_failures++;
			return;
		}
	VkBuffer counter = VK_NULL_HANDLE;
	VkDeviceMemory counterMemory = VK_NULL_HANDLE;
	void* counterMap = nullptr;
	const size_t counterBytes = preset.passes.size() * 64 * 4;
	if (countFetches)
	{
		ChainRunner::MakeBuffer(g_vk.d, counterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, counter, counterMemory, &counterMap);
		std::memset(counterMap, 0, counterBytes);
	}
	std::vector<bool> counted(preset.passes.size(), true);
	const int errorsBefore = g_validationErrors;
	{
		ChainRunner::Runner runner(g_vk.d, preset);
		g_vk.Begin();
		const bool loaded = runner.Load(g_vk.cmd,
			[&](const std::string& glsl, bool vertex, size_t pass) {
				bool c = false;
				std::vector<uint32_t> words = Compile(name, glsl, vertex, pass, countFetches, c);
				if (!vertex && countFetches && !c)
					counted[pass] = false;
				return words;
			},
			lutPixels, counter);
		g_vk.Submit();
		if (!loaded)
		{
			std::printf("    FAIL: %s: %s\n", name.c_str(), runner.error.c_str());
			g_failures++;
			return;
		}
		if (g_compileOnly)
		{
			std::printf("    compiled %s (%zu passes)\n", name.c_str(), preset.passes.size());
			return;
		}
		ChainRunner::Image input;
		std::vector<float> inputFloats;
		MakeInput(size.w, size.h, input, inputFloats);
		const std::string tag = std::to_string(size.w) + "x" + std::to_string(size.h) + "-" + std::to_string(size.vw) + "x" + std::to_string(size.vh);
		VkQueryPool queries = VK_NULL_HANDLE;
		VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qi.queryCount = static_cast<uint32_t>(preset.passes.size() * 2);
		vkCreateQueryPool(g_vk.d.device, &qi, nullptr, &queries);
		const int frames = countFetches ? 1 : 5;
		const auto start = std::chrono::steady_clock::now();
		for (int f = 0; f < frames; f++)
		{
			g_vk.Begin();
			vkCmdResetQueryPool(g_vk.cmd, queries, 0, qi.queryCount);
			if (!runner.RunFrame(g_vk.cmd, input.image, input.view, input.format, size.w, size.h, size.vw, size.vh, 1000 + f, queries))
			{
				std::printf("    FAIL: %s: %s\n", name.c_str(), runner.error.c_str());
				g_failures++;
				vkEndCommandBuffer(g_vk.cmd);
				break;
			}
			g_vk.Submit();
		}
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::vector<uint64_t> stamps(qi.queryCount);
		vkGetQueryPoolResults(g_vk.d.device, queries, 0, qi.queryCount, stamps.size() * 8, stamps.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		vkDestroyQueryPool(g_vk.d.device, queries, nullptr);

		if (countFetches)
		{
			fetchesPerPixel->assign(preset.passes.size(), -1.0);
			const uint32_t* c = static_cast<const uint32_t*>(counterMap);
			for (size_t p = 0; p < preset.passes.size(); p++)
			{
				uint64_t total = 0;
				for (int k = 0; k < 64; k++)
					total += c[p * 64 + k];
				const PassPlan& pp = runner.Plan().passes[p];
				if (counted[p])
					(*fetchesPerPixel)[p] = static_cast<double>(total) / (static_cast<double>(pp.width) * pp.height);
			}
		}
		else
		{
			// The checks: every pass's picture finite, the output a picture.
			const Stats in = Measure(inputFloats);
			size_t badPixels = 0;
			for (size_t p = 0; p < preset.passes.size(); p++)
			{
				std::vector<float> px;
				if (ReadBack(runner.PassOutput(p), px))
				{
					const Stats s = Measure(px);
					if (s.bad)
						std::printf("    FAIL: %s %s: pass %zu has %zu NaN / infinite pixels\n", name.c_str(), tag.c_str(), p, s.bad);
					badPixels += s.bad;
				}
			}
			std::vector<float> out;
			if (!ReadBack(runner.Output(), out))
			{
				std::printf("    FAIL: %s: its output's format cannot be read back\n", name.c_str());
				g_failures++;
			}
			else
			{
				const Stats s = Measure(out);
				const double ratio = s.mean / std::max(in.mean, 1e-6);
				const bool plausible = badPixels == 0 && s.mean > 0.04 && s.mean < 0.95 && s.max > 0.3 && s.deviation > 0.02 && ratio > 0.25 && ratio < 1.6;
				const int validation = g_validationErrors - errorsBefore;
				std::printf("    %s %-34s %s: brightness %.3f (input %.3f, x%.2f), spread %.3f, peak %.2f, %d validation messages, %.2f s for %d frames\n",
					plausible && validation == 0 ? "ok  " : "FAIL", name.c_str(), tag.c_str(), s.mean, in.mean, ratio, s.deviation, s.max, validation,
					seconds, frames);
				if (!plausible || validation != 0)
					g_failures++;
				WritePng(g_work + "/" + name + "-" + tag + ".png", out, runner.Output().width, runner.Output().height);
				WritePng(g_work + "/input-" + std::to_string(size.w) + "x" + std::to_string(size.h) + ".png", inputFloats, size.w, size.h);
			}
			if (size.vw == 3840)
			{
				std::printf("         lavapipe ms a pass at 4K:");
				for (size_t p = 0; p < preset.passes.size(); p++)
					std::printf(" %.0f", (stamps[2 * p + 1] - stamps[2 * p]) * g_vk.timestampPeriod * 1e-6);
				std::printf("\n");
			}
		}
		ChainRunner::Destroy(g_vk.d, input);
	}
	if (counter)
	{
		vkDestroyBuffer(g_vk.d.device, counter, nullptr);
		vkFreeMemory(g_vk.d.device, counterMemory, nullptr);
	}
}

// The fetches a 4K frame costs: each pass's fetches per pixel (measured at 1080p) times its pixels at 4K.
void Cost(const std::string& path, const std::string& root, const std::vector<double>& perPixel)
{
	LoadOptions options;
	options.allowedRoots.push_back(root);
	options.readLuts = false;
	Preset preset;
	std::string error;
	if (!LoadPreset(path, options, preset, error))
		return;
	for (Lut& l : preset.luts)
		l.width = l.height = 32;
	ChainPlan plan;
	FrameInfo frame;
	frame.sourceWidth = 640;
	frame.sourceHeight = 448;
	frame.viewportWidth = 3840;
	frame.viewportHeight = 2160;
	plan.Build(preset, frame);
	double total = 0, fullScreen = 0;
	bool unknown = false;
	// For tools/estimate-cost.py: preset, pass, its size at 4K, its fetches a pixel, its picture's bytes a pixel.
	FILE* tsv = std::fopen((g_work + "/fetches.tsv").c_str(), "a");
	std::printf("    cost %-34s fetches/pixel by pass:", Safe(path).c_str());
	for (size_t p = 0; p < plan.passes.size(); p++)
	{
		const double pixels = static_cast<double>(plan.passes[p].width) * plan.passes[p].height;
		if (perPixel[p] < 0)
		{
			unknown = true;
			std::printf(" ?");
			continue;
		}
		std::printf(" %.1f", perPixel[p]);
		const uint32_t f = plan.passes[p].vkFormat;
		if (tsv)
			std::fprintf(tsv, "%s\t%zu\t%u\t%u\t%.3f\t%d\n", Safe(path).c_str(), p, plan.passes[p].width, plan.passes[p].height, perPixel[p],
				f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : f == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : 4);
		total += pixels * perPixel[p];
		fullScreen += pixels / (3840.0 * 2160.0);
	}
	if (tsv)
		std::fclose(tsv);
	std::printf("\n         at 4K: %zu passes, %.2f full-screen passes of pixels, %.1f M fetches a frame = %.1f full-screen fetches a pixel%s\n",
		plan.passes.size(), fullScreen, total / 1e6, total / (3840.0 * 2160.0), unknown ? " (some passes uncounted)" : "");
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::fprintf(stderr, "usage: slang_vk <work> <root> <preset.slangp>... [--count]\n");
		return 2;
	}
	g_work = argv[1];
	const std::string root = argv[2];
	std::vector<std::string> presets;
	bool count = false;
	for (int i = 3; i < argc; i++)
	{
		if (std::strcmp(argv[i], "--count") == 0)
			count = true;
		else if (std::strcmp(argv[i], "--compile-only") == 0)
			g_compileOnly = true;
		else
			presets.push_back(argv[i]);
	}
	std::system(("mkdir -p " + g_work + "/spv").c_str());
	if (!g_vk.Init())
	{
		std::printf("  slang (Vulkan): NOT RUN (no Vulkan device)\n");
		return 0;
	}
	std::printf("  device: %s, validation layer %s\n", g_vk.deviceName.c_str(), g_vk.validation ? "on (with synchronization checks)" : "NOT INSTALLED");
	const Size sizes[2] = {{640, 448, 3840, 2160}, {512, 448, 1920, 1080}};
	int runs = 0;
	for (const std::string& path : presets)
	{
		if (count)
		{
			std::vector<double> perPixel;
			RunPreset(path, root, {640, 448, 1920, 1080}, true, &perPixel);
			if (!perPixel.empty())
				Cost(path, root, perPixel);
			continue;
		}
		for (const Size& s : sizes)
		{
			RunPreset(path, root, s, false, nullptr);
			runs++;
			if (g_compileOnly)
				break;
		}
	}
	g_vk.Done();
	if (!count)
		std::printf("  slang (Vulkan): %d runs of 5 frames, %d failed, %d validation messages\n", runs, g_failures, g_validationErrors);
	return g_failures == 0 && g_validationErrors == 0 ? 0 : 1;
}

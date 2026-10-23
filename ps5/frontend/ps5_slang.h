// Crux2 - libretro shader presets (.slangp) read, made ready for PCSX2's Vulkan compiler, and planned frame by frame.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A libretro "slang" preset (github.com/libretro/slang-shaders, the format RetroArch reads) is a
// .slangp file naming a chain of passes: each pass a .slang file holding a vertex and a fragment
// stage in Vulkan GLSL, drawn as one quad into a picture of its own, reading the game's picture,
// earlier passes' pictures, its own picture of the frame before and the preset's lookup pictures.
// This file is everything of that which needs no Vulkan:
//
//  * LoadPreset reads the preset (and the presets it names with #reference), each pass's .slang
//    (its #include lines put in place, its #pragma parameter / name / format lines taken out, its
//    two stages made two GLSL sources that PCSX2's runtime compiler, shaderc over glslang, takes
//    as they are: VKShaderCache::CompileShaderToSPV with a Vulkan 1.0 target), and its lookup
//    pictures' files (the caller decodes them).
//  * The reflection is done on the text, with no compiler: a small preprocessor (#define, #if and
//    the rest) finds the code glslang will see, and in it the push constant block, the uniform
//    block (set 0) and the samplers, each member's offset worked out by the std140 / std430 rules
//    glslang follows. Each member is given its meaning by name as RetroArch does (MVP,
//    OutputSize, SourceSize, FrameCount, a parameter...), each sampler what it reads (Source,
//    Original, OriginalHistoryN, PassOutputN, PassFeedbackN, an alias, a lookup picture). The PC
//    test (ps5/tests/slang) checks every offset against glslang's own reflection.
//  * ChainPlan::Build is what the chain runner asks each frame: every pass's size, the bytes of
//    its uniform block and push constants, and which picture each of its samplers gets, with the
//    sampler's filter, wrap and mipmaps. Preset::passes[i].feedback and Preset::historyFrames say
//    what the runner keeps from frame to frame.
//
// Parts are adapted from PSSwanStation (its ps5/src/slangimport.cpp and chain.cpp, by the same
// author, GPL-3.0-or-later): the preset reading, the include expansion and the semantics of
// sizes and textures. PSSwanStation compiled first and read the SPIR-V; Crux2 hands PCSX2 the
// text, so the reflection here is done on the GLSL.
//
// No exceptions, no std::filesystem: files are read through LoadOptions::files (by default
// fopen/fstat), folders listed with opendir. Errors are sentences for the screen.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PS5Slang
{
// Limits: a preset beyond them is refused with a sentence saying which.
inline constexpr int MAX_PASSES = 32;
inline constexpr int MAX_LUTS = 64;
inline constexpr int MAX_BINDINGS = 16; // a pass's bindings are 0..15 (RetroArch's SLANG_NUM_BINDINGS)
inline constexpr int MAX_PARAMETERS = 1024;
inline constexpr size_t MAX_PUSH_CONSTANT_BYTES = 128; // what every Vulkan device has, and RetroArch's own limit
inline constexpr size_t MAX_UBO_BYTES = 16384;

enum class ScaleType : uint8_t
{
	Source,   // a multiple of the picture the pass reads
	Viewport, // a multiple of the output rectangle
	Absolute, // pixels
};

enum class Wrap : uint8_t
{
	ClampToBorder, // transparent black outside (libretro's default)
	ClampToEdge,
	Repeat,
	MirroredRepeat,
};

// What a sampler (or a size) is of.
struct TextureRef
{
	enum Kind : uint8_t
	{
		History,      // index 0: the game's picture of this frame (Original, and Source of pass 0); N: N frames ago
		PassOutput,   // index: the pass whose picture of this frame it is
		PassFeedback, // index: the pass whose picture of the frame before it is
		Lut,          // index: into Preset::luts
	};
	Kind kind = History;
	uint32_t index = 0;
};

// The meaning of a member of a pass's uniform block or push constants.
enum class Semantic : uint8_t
{
	MVP,               // mat4: maps the quad (0..1) onto the picture
	OutputSize,        // vec4 (w, h, 1/w, 1/h) of the pass's own picture
	FinalViewportSize, // vec4 of the output rectangle
	FrameCount,        // the frame's number, modulo frame_count_modN when the preset sets one
	FrameDirection,    // 1 (-1 while a game would be rewound; Crux2 never does)
	FrameTimeDelta,    // microseconds from one frame to the next
	OriginalFPS,       // the game's frame rate
	Rotation,          // 0
	OriginalAspect,    // the picture's shape as shown (w / h)
	OriginalAspectRotated,
	TotalSubFrames,    // 1
	CurrentSubFrame,   // 1
	SwapCount,         // as FrameCount (no sub-frames, no black frames between)
	HDRMode,           // 0: the chain's output is SDR (RetroArch's HDR settings, all off)
	BrightnessNits,    // FrameInfo::paperWhiteNits
	HDRScanlines,      // 0 (RetroArch's "Scanlines")
	SubpixelLayout,    // 0
	ExpandGamut,       // 0
	InverseTonemap,    // 0
	HDR10,             // 0
	Sensor,            // vec3 0: Gyroscope, Accelerometer, AccelerometerRest
	TextureSize,       // vec4 of the picture named by `texture` (SourceSize, OriginalSize, <alias>Size...)
	Parameter,         // float: Preset::parameters[parameter].value
	None,              // a name RetroArch gives no meaning, which the shader declares but never reads: left 0
};

enum class BaseType : uint8_t
{
	Float,
	Int,
	Uint,
	Bool,
};

struct Member
{
	std::string name;
	Semantic semantic = Semantic::Parameter;
	TextureRef texture;     // for TextureSize
	uint32_t parameter = 0; // for Parameter
	bool push = false;      // in the push constants (else the uniform block)
	uint32_t offset = 0;
	uint32_t size = 0;      // bytes the member covers
	BaseType base = BaseType::Float;
	uint8_t columns = 1, rows = 1; // a vec4 is 1 x 4, a mat4 4 x 4
	bool rowMajor = false;
	uint32_t matrixStride = 0;     // bytes between columns (rows when row major)
	uint32_t arrayLength = 0;      // 0: not an array (an array gets its first element set)
	uint32_t arrayStride = 0;
	bool active = true;            // the code names it outside its declaration (in either stage)
};

struct Sampler
{
	std::string name;
	uint32_t binding = 0;
	TextureRef texture;
	bool vertex = false, fragment = false; // which stages declare it
	bool active = true;                    // the code names it outside its declaration
};

struct Parameter
{
	std::string name, description;
	float initial = 0.f;     // the shader's #pragma parameter default
	float presetValue = 0.f; // the preset's value (its own setting, else `initial`); Reset goes back to it
	float minimum = 0.f, maximum = 0.f, step = 0.f;
	float value = 0.f;       // now
};

struct Lut
{
	std::string name, path;
	std::string data; // the file as it is (a PNG): the caller decodes it, and sets width and height
	bool linear = false, mipmap = false;
	Wrap wrap = Wrap::ClampToBorder;
	uint32_t width = 0, height = 0;
};

struct Pass
{
	std::string path;   // the .slang file
	std::string vertex; // GLSL of each stage, for VKShaderCache::CompileShaderToSPV as it is
	std::string fragment;
	std::string alias;  // the preset's aliasN, else the shader's #pragma name ("" when neither)

	std::string formatName = "R8G8B8A8_UNORM"; // libretro's name of the picture's format
	uint32_t vkFormat = 37;                    // its VkFormat (VK_FORMAT_R8G8B8A8_UNORM)

	bool filterLinear = false; // how this pass samples Source (and how others sample the pass before's picture)
	Wrap wrap = Wrap::ClampToBorder;
	bool mipmapInput = false;  // the picture it reads as Source has mipmaps
	uint32_t frameCountMod = 0;
	ScaleType scaleTypeX = ScaleType::Source, scaleTypeY = ScaleType::Source;
	float scaleX = 1.f, scaleY = 1.f;
	bool scaleSet = false;     // the preset gave a scale type (the last pass without one fills the viewport)

	int uboBinding = -1;       // the uniform block's binding in set 0, -1: none
	uint32_t uboSize = 0;      // bytes, rounded up to 16
	uint32_t pushSize = 0;     // bytes of push constants, 0: none
	std::vector<Member> members;
	std::vector<Sampler> samplers;

	bool feedback = false;     // some pass reads this pass's picture of the frame before
};

struct Preset
{
	std::string path;
	std::vector<Pass> passes;
	std::vector<Lut> luts;
	std::vector<Parameter> parameters;
	uint32_t historyFrames = 0;      // OriginalHistoryN: the runner keeps the game's last N pictures
	std::vector<std::string> warnings; // what was let through (a parameter declared twice differently...)

	// The parameters by name: the index, or -1.
	int FindParameter(std::string_view name) const;
	// Sets one, kept within its range; false when there is none of that name.
	bool SetParameter(std::string_view name, float value);
	// Every parameter back to the preset's value.
	void ResetParameters();
};

// Where files come from: by default the file system (fopen). The fuzz test gives its own.
struct FileSource
{
	// The whole file in `out`; false with a reason when it is missing, not a file, or larger than
	// `maxBytes`.
	bool (*read)(void* user, const std::string& path, size_t maxBytes, std::string& out, std::string& error) = nullptr;
	void* user = nullptr;
};

struct LoadOptions
{
	// Folders every file the preset reaches must be inside (after "a/../b" is taken out).
	// Empty: the preset's own folder. libretro presets name "../" a lot inside their collection:
	// give the collection's folder (the title's shaders/slang, the USB drive's folder).
	std::vector<std::string> allowedRoots;
	FileSource files;
	size_t maxFileBytes = 1u << 20;   // a .slangp or .slang file
	size_t maxSourceBytes = 4u << 20; // a pass with its includes put in
	size_t maxLutBytes = 8u << 20;    // a lookup picture's file
	int maxIncludeDepth = 16;
	int maxReferenceDepth = 8;
	int maxParentSteps = 16;          // ".." in one path
	bool readLuts = true;             // false: the lookup pictures are only checked to be there
};

// Reads a preset. False with `error` set (a sentence) when it cannot be run: a file missing or
// outside the roots, a pass the reflection cannot make out, a name a pass reads that nothing
// makes, a limit passed.
bool LoadPreset(const std::string& path, const LoadOptions& options, Preset& out, std::string& error);

// One stage's reflection, as LoadPreset does it (the PC test compares it with glslang's).
struct StageReflection
{
	struct Block
	{
		bool present = false;
		bool push = false;
		int set = 0, binding = -1;
		uint32_t size = 0; // the end of the last member, unrounded (as glslang reports a block's size)
		std::vector<Member> members;
	};
	Block push, ubo;
	std::vector<Sampler> samplers;
};
bool ReflectStage(const std::string& source, bool vertex, StageReflection& out, std::string& error);

// The .slangp files in `folder` and the folders inside it, `maxDepth` deep, sorted.
std::vector<std::string> FindPresets(const std::string& folder, int maxDepth = 4);

// libretro's format names and Vulkan's numbers for them (0 when not known).
uint32_t VkFormatOf(std::string_view libretroName);

// ------------------------------------------------------------------------------------------------
// The plan of one frame.

struct FrameInfo
{
	uint32_t sourceWidth = 0, sourceHeight = 0;     // the game's picture (Original)
	uint32_t viewportWidth = 0, viewportHeight = 0; // the output rectangle
	uint64_t frameCount = 0;
	int32_t frameDirection = 1;
	float originalFps = 60.f;
	float originalAspect = 0.f; // 0: sourceWidth / sourceHeight
	uint32_t rotation = 0;
	float paperWhiteNits = 200.f; // BrightnessNits (RetroArch's default paper white)
	// The sizes of the pictures the runner keeps for OriginalHistory1..N (index 0 is 1 frame
	// ago); missing ones are taken to be the size of this frame's.
	std::vector<uint32_t> historyWidths, historyHeights;
};

struct TextureBind
{
	uint32_t binding = 0;
	TextureRef texture;
	bool linear = false; // the sampler: filter, wrap, and whether to sample its mipmaps
	Wrap wrap = Wrap::ClampToBorder;
	bool mipmap = false;
};

struct PassPlan
{
	uint32_t width = 0, height = 0; // of the picture it draws
	uint32_t mipLevels = 1;         // of that picture (more when the next pass has mipmap_input)
	uint32_t vkFormat = 37;
	bool final = false;             // the last pass: its picture is the chain's output
	std::vector<uint8_t> ubo;       // Pass::uboSize bytes for its uniform block
	std::vector<uint8_t> push;      // Pass::pushSize bytes of push constants
	std::vector<TextureBind> textures;
};

struct ChainPlan
{
	std::vector<PassPlan> passes;
	uint32_t originalMipLevels = 1; // the game's picture needs mipmaps (mipmap_input0)
	uint32_t historyFrames = 0;     // as Preset::historyFrames

	// Works out the frame. The buffers are reused from frame to frame. False when the sizes are
	// zero or the preset has no passes.
	bool Build(const Preset& preset, const FrameInfo& frame, std::string* error = nullptr);
};

// The bytes RetroArch puts in MVP: the quad's corners (0, 0)..(1, 1) to Vulkan's (-1, -1)..(1, 1),
// column major.
inline constexpr float MVP_MATRIX[16] = {2.f, 0.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, -1.f, -1.f, 0.f, 1.f};

// The quad every pass draws (a triangle strip of 4): position (x, y, 0, 1) at location 0, then
// the texture coordinate (u, v) at location 1, 6 floats a vertex.
inline constexpr float QUAD_VERTICES[24] = {
	0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
	1.f, 0.f, 0.f, 1.f, 1.f, 0.f,
	0.f, 1.f, 0.f, 1.f, 0.f, 1.f,
	1.f, 1.f, 0.f, 1.f, 1.f, 1.f,
};
} // namespace PS5Slang

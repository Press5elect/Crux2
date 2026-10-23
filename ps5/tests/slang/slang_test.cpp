// Crux2 - the libretro preset importer (ps5_slang.cpp) on the PC: parsing, the text reflection against glslang's, the plan, a fuzz pass.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   slang_test check <work folder> <root> <preset.slangp>...
//       Each preset is read with <root> as the only allowed folder. Every pass's two stages are
//       written out, compiled by glslangValidator (Vulkan 1.0, as PCSX2's shaderc call targets),
//       optimised by spirv-opt -O (shaderc's "performance" level runs the same passes) and checked
//       by spirv-val; glslang's reflection (-q --reflect-all-block-variables) is compared with
//       PS5Slang::ReflectStage: every member's offset, each block's size and binding, every
//       sampler's binding. The plan is built for 640x448 -> 3840x2160 and 512x448 -> 1920x1080
//       and its buffers checked against the reflection.
//   slang_test fuzz <root> <seed> <iterations> <preset.slangp>...
//       The presets' files, read once, are mutated in memory (bytes flipped, cut, repeated,
//       directives and brackets put in) and read again through LoadOptions::files; whatever
//       loads is planned. Run under the address and undefined behaviour sanitizers.
//   slang_test refuse
//       Presets that must be refused, with the sentence each gets (paths outside the roots,
//       too deep, too many passes, a sampler nothing makes...).
//
// Exit status 0 when all is well.

#include "ps5_slang.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace PS5Slang;

namespace
{
int g_failures = 0;

void Fail(const char* fmt, const std::string& a = {}, const std::string& b = {})
{
	g_failures++;
	std::printf("    FAIL: ");
	std::printf(fmt, a.c_str(), b.c_str());
	std::printf("\n");
}

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

std::string Safe(const std::string& path)
{
	std::string name = path.substr(path.rfind('/') + 1);
	for (char& c : name)
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
			c = '_';
	return name;
}

int Run(const std::string& command)
{
	const int status = std::system(command.c_str());
	return status == -1 ? -1 : WEXITSTATUS(status);
}

// glslang's -q output: "Block.member: offset N, ..." and "Block: ..., size N, ..., binding B" and
// "Sampler: ..., binding B".
struct GlslangReflection
{
	std::map<std::string, std::pair<std::string, long>> members; // "Block.member" -> (block, offset)
	std::map<std::string, std::pair<long, long>> blocks;         // block -> (size, binding)
	std::map<std::string, long> samplers;                         // name -> binding
};

long Field(const std::string& line, const char* name)
{
	const size_t at = line.find(name);
	if (at == std::string::npos)
		return -9999;
	return std::strtol(line.c_str() + at + std::strlen(name), nullptr, 10);
}

bool ParseGlslang(const std::string& text, GlslangReflection& out)
{
	enum
	{
		None,
		Uniforms,
		Blocks
	} section = None;
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(start, end - start);
		start = end + 1;
		if (line == "Uniform reflection:")
			section = Uniforms;
		else if (line == "Uniform block reflection:")
			section = Blocks;
		else if (line.empty() || line.back() == ':')
			section = line.empty() ? section : None;
		else if (section != None)
		{
			const size_t colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			const std::string name = line.substr(0, colon);
			if (section == Blocks)
				out.blocks[name] = {Field(line, "size "), Field(line, "binding ")};
			else if (name.find('.') != std::string::npos)
				out.members[name] = {name.substr(0, name.find('.')), Field(line, "offset ")};
			else
				out.samplers[name] = Field(line, "binding ");
		}
	}
	return true;
}

struct Totals
{
	int presets = 0, passes = 0, stages = 0, members = 0, samplers = 0, plans = 0;
	int activeAgree = 0, activeOver = 0; // members read: as glslang says / thought read, which glslang does not
} g_totals;

// One stage: compiled and compared.
void CheckStage(const std::string& dir, size_t pass, bool vertex, const std::string& source)
{
	const std::string base = dir + "/p" + std::to_string(pass) + (vertex ? ".vert" : ".frag");
	if (!WriteAll(base, source))
	{
		Fail("cannot write %s", base);
		return;
	}
	const std::string log = base + ".log";
	if (Run("glslangValidator -V --target-env vulkan1.0 -l -q --reflect-all-block-variables -o " + base + ".spv " + base + " > " +
			log + " 2>&1") != 0)
	{
		std::string text;
		ReadAll(log, text);
		Fail("%s does not compile:\n%s", base, text.substr(0, 3000));
		return;
	}
	if (Run("spirv-opt -O " + base + ".spv -o " + base + ".opt.spv > " + base + ".opt.log 2>&1") != 0 ||
		Run("spirv-val --target-env vulkan1.0 " + base + ".opt.spv > " + base + ".val.log 2>&1") != 0)
		Fail("%s: spirv-opt -O or spirv-val refuses it (see %s.opt.log / .val.log)", base, base);
	g_totals.stages++;

	std::string text;
	ReadAll(log, text);
	GlslangReflection theirs;
	ParseGlslang(text, theirs);
	// And what glslang finds read (without --reflect-all-block-variables it lists only those).
	GlslangReflection read;
	if (Run("glslangValidator -V --target-env vulkan1.0 -l -q -o " + base + ".spv " + base + " > " + log + ".active 2>&1") == 0)
	{
		std::string activeText;
		ReadAll(log + ".active", activeText);
		ParseGlslang(activeText, read);
	}
	StageReflection ours;
	std::string error;
	if (!ReflectStage(source, vertex, ours, error))
	{
		Fail("%s: the text reflection refuses it: %s", base, error);
		return;
	}
	// Block names by binding: the push constant block has none (-1).
	std::string pushName, uboName;
	for (const auto& [name, sb] : theirs.blocks)
		(sb.second < 0 ? pushName : uboName) = name;
	for (const StageReflection::Block* b : {&ours.push, &ours.ubo})
	{
		const bool isPush = b == &ours.push;
		const std::string& name = isPush ? pushName : uboName;
		if (!b->present)
		{
			if (!name.empty())
				Fail("%s: glslang has a block %s the text reflection does not", base, name);
			continue;
		}
		if (name.empty())
		{
			// glslang leaves out a block the stage never reads; then none of its members may be
			// thought read.
			if (std::any_of(b->members.begin(), b->members.end(), [](const Member& m) { return m.active; }))
				Fail("%s: the text reflection thinks a %s block read that glslang leaves out", base, isPush ? "push constant" : "uniform");
			continue;
		}
		const auto& sb = theirs.blocks[name];
		if (sb.first != static_cast<long>(b->size))
			Fail("%s: block size differs from glslang's (%s)", base, name + ": ours " + std::to_string(b->size) + ", glslang " + std::to_string(sb.first));
		if (!isPush && sb.second != b->binding)
			Fail("%s: uniform block binding differs (%s)", base, name);
		size_t theirCount = 0;
		for (const auto& [full, bo] : theirs.members)
			theirCount += bo.first == name;
		if (theirCount != b->members.size())
			Fail("%s: member count differs from glslang's (%s)", base,
				name + ": ours " + std::to_string(b->members.size()) + ", glslang " + std::to_string(theirCount));
		for (const Member& m : b->members)
		{
			g_totals.members++;
			const auto it = theirs.members.find(name + "." + m.name);
			if (it == theirs.members.end())
				Fail("%s: glslang does not list %s", base, name + "." + m.name);
			else if (it->second.second != static_cast<long>(m.offset))
				Fail("%s: offset differs: %s", base,
					name + "." + m.name + " ours " + std::to_string(m.offset) + ", glslang " + std::to_string(it->second.second));
			const bool glslangReads = read.members.count(name + "." + m.name) != 0;
			if (glslangReads && !m.active)
				Fail("%s: glslang reads %s, the text reflection thinks it unread", base, name + "." + m.name);
			g_totals.activeAgree += glslangReads == m.active;
			g_totals.activeOver += !glslangReads && m.active;
		}
	}
	for (const Sampler& s : ours.samplers)
	{
		g_totals.samplers++;
		const auto it = theirs.samplers.find(s.name);
		// glslang lists only the samplers the stage reads; the text lists all it declares.
		if (it != theirs.samplers.end() && it->second != static_cast<long>(s.binding))
			Fail("%s: sampler binding differs: %s", base, s.name);
		if (it != theirs.samplers.end() && !s.active)
			Fail("%s: glslang reads sampler %s, the text reflection thinks it unread", base, s.name);
	}
	for (const auto& [name, binding] : theirs.samplers)
		if (std::none_of(ours.samplers.begin(), ours.samplers.end(), [&](const Sampler& s) { return s.name == name; }))
			Fail("%s: glslang has a sampler %s the text reflection does not", base, name);
}

// The plan's buffers hold what the reflection says, at the right places.
void CheckPlan(const Preset& preset, uint32_t sw, uint32_t sh, uint32_t vw, uint32_t vh)
{
	ChainPlan plan;
	FrameInfo frame;
	frame.sourceWidth = sw;
	frame.sourceHeight = sh;
	frame.viewportWidth = vw;
	frame.viewportHeight = vh;
	frame.frameCount = 12345;
	std::string error;
	if (!plan.Build(preset, frame, &error))
	{
		Fail("the plan cannot be built: %s", error);
		return;
	}
	g_totals.plans++;
	if (plan.passes.size() != preset.passes.size())
		Fail("the plan has the wrong number of passes");
	for (size_t p = 0; p < plan.passes.size(); p++)
	{
		const PassPlan& pp = plan.passes[p];
		const Pass& pass = preset.passes[p];
		if (pp.ubo.size() != pass.uboSize || pp.push.size() != pass.pushSize || pp.width == 0 || pp.height == 0)
			Fail("pass %s: the plan's sizes are wrong", std::to_string(p));
		if (pp.textures.size() != pass.samplers.size())
			Fail("pass %s: the plan binds the wrong number of textures", std::to_string(p));
		for (const Member& m : pass.members)
		{
			const std::vector<uint8_t>& buf = m.push ? pp.push : pp.ubo;
			if (m.offset + 4 > buf.size())
				continue;
			float f = 0.f;
			std::memcpy(&f, buf.data() + m.offset, 4);
			if (m.semantic == Semantic::OutputSize && (f != static_cast<float>(pp.width)))
				Fail("pass %s: OutputSize is not the pass's width", std::to_string(p));
			if (m.semantic == Semantic::FinalViewportSize && f != static_cast<float>(vw))
				Fail("pass %s: FinalViewportSize is wrong", std::to_string(p));
			if (m.semantic == Semantic::MVP && f != 2.f)
				Fail("pass %s: MVP is wrong", std::to_string(p));
			if (m.semantic == Semantic::Parameter && m.base == BaseType::Float && f != preset.parameters[m.parameter].value)
				Fail("pass %s: parameter %s is not its value", std::to_string(p), m.name);
		}
	}
	if (vw == 3840)
	{
		std::printf("      %ux%u -> %ux%u:", sw, sh, vw, vh);
		for (const PassPlan& pp : plan.passes)
			std::printf(" %ux%u%s", pp.width, pp.height, pp.mipLevels > 1 ? "m" : "");
		std::printf("\n");
	}
}

int Check(int argc, char** argv)
{
	if (argc < 5)
		return 2;
	const std::string work = argv[2];
	LoadOptions options;
	options.allowedRoots.push_back(argv[3]);
	for (int i = 4; i < argc; i++)
	{
		Preset preset;
		std::string error;
		const std::string path = argv[i];
		std::printf("  %s\n", path.c_str());
		if (!LoadPreset(path, options, preset, error))
		{
			Fail("not read: %s", error);
			continue;
		}
		g_totals.presets++;
		size_t feedback = 0;
		for (const Pass& p : preset.passes)
			feedback += p.feedback;
		std::printf("      %zu passes, %zu parameters, %zu lookup pictures, history %u, feedback %zu, %zu warnings\n",
			preset.passes.size(), preset.parameters.size(), preset.luts.size(), preset.historyFrames, feedback,
			preset.warnings.size());
		for (const std::string& w : preset.warnings)
			std::printf("      warning: %s\n", w.c_str());
		const std::string dir = work + "/" + Safe(path);
		mkdir(dir.c_str(), 0755);
		for (size_t p = 0; p < preset.passes.size(); p++)
		{
			g_totals.passes++;
			CheckStage(dir, p, true, preset.passes[p].vertex);
			CheckStage(dir, p, false, preset.passes[p].fragment);
		}
		CheckPlan(preset, 640, 448, 3840, 2160);
		CheckPlan(preset, 512, 448, 1920, 1080);
		// Parameters: set, clamped, reset.
		if (!preset.parameters.empty())
		{
			Parameter& first = preset.parameters[0];
			preset.SetParameter(first.name, 1e9f);
			if (first.value != std::max(first.minimum, first.maximum))
				Fail("SetParameter does not keep %s in its range", first.name);
			preset.ResetParameters();
			if (first.value != first.presetValue)
				Fail("ResetParameters does not put %s back", first.name);
		}
		if (preset.SetParameter("no such parameter", 1.f))
			Fail("SetParameter accepts a name there is none of");
	}
	std::printf("  check: %d presets, %d passes, %d stages compiled, %d members and %d samplers compared with glslang, %d plans\n",
		g_totals.presets, g_totals.passes, g_totals.stages, g_totals.members, g_totals.samplers, g_totals.plans);
	std::printf("         whether a member is read: as glslang for %d, thought read where glslang drops it for %d (harmless: filled anyway)\n",
		g_totals.activeAgree, g_totals.activeOver);
	return g_failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------
// The fuzz pass.

struct Memory
{
	std::map<std::string, std::string> files; // read once from disk
	std::string mutatedPath, mutated;
	std::set<std::string> touched;
};

bool MemoryRead(void* user, const std::string& path, size_t maxBytes, std::string& out, std::string& error)
{
	Memory& m = *static_cast<Memory*>(user);
	m.touched.insert(path);
	if (path == m.mutatedPath)
		out = m.mutated;
	else
	{
		auto it = m.files.find(path);
		if (it == m.files.end())
		{
			std::string text;
			struct stat st;
			if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !ReadAll(path, text))
			{
				error = "missing";
				return false;
			}
			it = m.files.emplace(path, std::move(text)).first;
		}
		out = it->second;
	}
	if (out.size() > maxBytes)
	{
		error = "too large";
		return false;
	}
	return true;
}

void Mutate(std::mt19937& rng, std::string& text)
{
	static const char* const pieces[] = {"#include \"", "#reference \"", "../", "../../../../", "/etc/passwd", "{", "}", "(", ")", "[",
		"]", ";", ",", "\n", "\"", "\\\n", "layout(push_constant) uniform", "layout(std140, set = 0, binding = 15) uniform U {",
		"uniform sampler2D ", "binding = 99", "binding = -1", "#if 1/0\n", "#if defined(\n", "#ifdef X\n", "#else\n", "#endif\n",
		"#define X X X\n", "#define F(a) F(a) a\n", "F(", "#pragma stage vertex\n", "#pragma stage fragment\n",
		"#pragma parameter p \"d\" 1 0 2 0.1\n", "#pragma parameter", "#pragma format R32G32B32A32_SFLOAT\n", "#pragma name A\n",
		"mat4x3 ", "float a[0];", "float b[4097];", "vec3 ", "layout(offset = 3) float o;", "shaders = 33\n", "shaders = 0\n",
		"shader0 = ", "scale_type0 = absolute\nscale0 = 1e30\n", "alias0 = Source\n", "textures = \"A;B;A\"\n", "PassFeedback31",
		"OriginalHistory99", "PassOutput7", "Size", "9999999999999999999999", "-0", "1e-45", "nan", "\xff\xfe", ""};
	const int count = 1 + static_cast<int>(rng() % 6);
	for (int k = 0; k < count; k++)
	{
		const size_t at = text.empty() ? 0 : rng() % (text.size() + 1);
		switch (rng() % 7)
		{
			case 0: // a byte changed
				if (!text.empty())
					text[std::min(at, text.size() - 1)] = static_cast<char>(rng());
				break;
			case 1: // a piece cut out
			{
				const size_t n = std::min<size_t>(rng() % 200, text.size() - std::min(at, text.size()));
				text.erase(std::min(at, text.size()), n);
				break;
			}
			case 2: // a piece repeated
			{
				const size_t n = std::min<size_t>(rng() % 400, text.size() - std::min(at, text.size()));
				const std::string piece = text.substr(std::min(at, text.size()), n);
				text.insert(std::min(at, text.size()), piece);
				break;
			}
			case 3: // a line's digit changed
			{
				const size_t d = text.find_first_of("0123456789", std::min(at, text.size()));
				if (d != std::string::npos)
					text[d] = static_cast<char>('0' + rng() % 10);
				break;
			}
			default: // something a parser must cope with
			{
				const char* piece = pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
				text.insert(std::min(at, text.size()), piece[0] ? piece : std::string(1, '\0'));
				break;
			}
		}
	}
}

int Fuzz(int argc, char** argv)
{
	if (argc < 6)
		return 2;
	const std::string root = argv[2];
	const unsigned seed = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
	const int iterations = std::atoi(argv[4]);
	std::vector<std::string> presets(argv + 5, argv + argc);
	Memory memory;
	LoadOptions options;
	options.allowedRoots.push_back(root);
	options.files.read = MemoryRead;
	options.files.user = &memory;
	// Each preset once as it is, for the files it reaches.
	std::vector<std::vector<std::string>> reached(presets.size());
	for (size_t i = 0; i < presets.size(); i++)
	{
		memory.touched.clear();
		Preset p;
		std::string error;
		if (!LoadPreset(presets[i], options, p, error))
			Fail("fuzz: %s is not read unmutated: %s", presets[i], error);
		for (const std::string& t : memory.touched)
			if (t.size() < 4 || t.compare(t.size() - 4, 4, ".png") != 0)
				reached[i].push_back(t);
	}
	std::mt19937 rng(seed);
	int loaded = 0, refused = 0;
	std::map<std::string, int> reasons;
	for (int n = 0; n < iterations; n++)
	{
		const size_t which = rng() % presets.size();
		if (reached[which].empty())
			continue;
		memory.mutatedPath = reached[which][rng() % reached[which].size()];
		memory.mutated = memory.files[memory.mutatedPath];
		Mutate(rng, memory.mutated);
		Preset p;
		std::string error;
		if (LoadPreset(presets[which], options, p, error))
		{
			loaded++;
			ChainPlan plan;
			FrameInfo frame;
			frame.sourceWidth = 1 + rng() % 1024;
			frame.sourceHeight = 1 + rng() % 1024;
			frame.viewportWidth = 1 + rng() % 4096;
			frame.viewportHeight = 1 + rng() % 4096;
			frame.frameCount = rng();
			if (!plan.Build(p, frame, &error))
				Fail("fuzz: a preset that loads cannot be planned: %s", error);
			for (Parameter& param : p.parameters)
				p.SetParameter(param.name, static_cast<float>(static_cast<int>(rng() % 2001) - 1000));
		}
		else
		{
			refused++;
			std::string key = error.substr(0, 40);
			reasons[key]++;
			if (error.empty())
				Fail("fuzz: a preset was refused without a reason");
		}
	}
	std::printf("  fuzz: %d mutated presets, %d still loaded and were planned, %d refused (%zu different first words)\n", iterations,
		loaded, refused, reasons.size());
	return g_failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------
// What must be refused.

struct Fixture
{
	std::map<std::string, std::string> files;
};

bool FixtureRead(void* user, const std::string& path, size_t maxBytes, std::string& out, std::string& error)
{
	const Fixture& f = *static_cast<const Fixture*>(user);
	const auto it = f.files.find(path);
	if (it == f.files.end())
	{
		error = "cannot find " + path;
		return false;
	}
	if (it->second.size() > maxBytes)
	{
		error = "too large";
		return false;
	}
	out = it->second;
	return true;
}

const char* const STAGE = "#version 450\n"
						  "layout(push_constant) uniform Push { vec4 SourceSize; uint FrameCount; float p; } params;\n"
						  "#pragma parameter p \"P\" 0.5 0.0 1.0 0.1\n"
						  "layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;\n"
						  "#pragma stage vertex\n"
						  "layout(location = 0) in vec4 Position; layout(location = 1) in vec2 TexCoord; layout(location = 0) out vec2 uv;\n"
						  "void main() { gl_Position = global.MVP * Position; uv = TexCoord; }\n"
						  "#pragma stage fragment\n"
						  "layout(location = 0) in vec2 uv; layout(location = 0) out vec4 FragColor;\n"
						  "layout(set = 0, binding = 2) uniform sampler2D Source;\n"
						  "void main() { FragColor = texture(Source, uv) * params.p * float(params.FrameCount) * params.SourceSize.x; }\n";

int Refuse()
{
	struct Case
	{
		const char* what;
		std::map<std::string, std::string> files;
		const char* expect; // a part of the sentence
		bool ok = false;
	};
	auto with = [](std::string sampler) {
		std::string s = STAGE;
		s.replace(s.find("texture(Source"), 14, "texture(" + sampler);
		s.replace(s.find("uniform sampler2D Source"), 24, "uniform sampler2D " + sampler);
		return s;
	};
	std::vector<Case> cases = {
		{"a plain preset", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", STAGE}}, "", true},
		{"a path up out of the root", {{"/r/a.slangp", "shaders = 1\nshader0 = ../s.slang\n"}, {"/s.slang", STAGE}}, "outside"},
		{"an absolute path elsewhere", {{"/r/a.slangp", "shaders = 1\nshader0 = /etc/s.slang\n"}}, "outside"},
		{"33 passes", {{"/r/a.slangp", "shaders = 33\n"}}, "more than 32"},
		{"no passes", {{"/r/a.slangp", "textures = \"\"\n"}}, "names no passes"},
		{"a missing shader", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}}, "cannot find"},
		{"a reference loop", {{"/r/a.slangp", "#reference \"b.slangp\"\n"}, {"/r/b.slangp", "#reference \"a.slangp\"\n"}}, "too deeply"},
		{"an include loop", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", "#version 450\n#include \"s.slang\"\n"}}, "too deep"},
		{"no stages", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", "#version 450\nvoid main() {}\n"}}, "both stages"},
		{"a sampler nothing makes", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", with("Nothing")}}, "nothing in the preset"},
		{"an alias read before it is drawn: the game's picture, as RetroArch",
			{{"/r/a.slangp", "shaders = 2\nshader0 = s.slang\nshader1 = s.slang\nalias1 = Later\n"}, {"/r/s.slang", with("Later")}}, "", true},
		{"a feedback by alias",
			{{"/r/a.slangp", "shaders = 2\nshader0 = s.slang\nshader1 = s.slang\nalias1 = Later\n"}, {"/r/s.slang", with("LaterFeedback")}}, "", true},
		{"history 3", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", with("OriginalHistory3")}}, "", true},
		{"a lookup picture", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\ntextures = L\nL = l.png\nL_linear = true\n"}, {"/r/s.slang", with("L")}, {"/r/l.png", "x"}}, "", true},
		{"a lookup picture without its file", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\ntextures = L\n"}, {"/r/s.slang", with("L")}}, "without its file"},
		{"two passes with one alias", {{"/r/a.slangp", "shaders = 2\nshader0 = s.slang\nshader1 = s.slang\nalias0 = A\nalias1 = A\n"}, {"/r/s.slang", STAGE}}, "two passes"},
		{"an unknown format", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", std::string(STAGE).insert(13, "#pragma format R5G6B5\n")}}, "format"},
		{"push constants over 128 bytes",
			{{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"},
				{"/r/s.slang", std::string(STAGE).replace(std::string(STAGE).find("float p;"), 8, "float p; mat4 a; mat4 b;")}}, "128"},
		{"a member RetroArch gives no meaning, read",
			{{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"},
				{"/r/s.slang", std::string(STAGE).replace(std::string(STAGE).find("params.p *"), 10, "params.nope *").replace(std::string(STAGE).find("float p;"), 8, "float p; float nope;")}},
			"would not fill"},
		{"a binding of 16", {{"/r/a.slangp", "shaders = 1\nshader0 = s.slang\n"}, {"/r/s.slang", std::string(STAGE).replace(std::string(STAGE).find("binding = 2"), 11, "binding = 16")}}, "below 16"},
	};
	int bad = 0;
	for (Case& c : cases)
	{
		Fixture fixture{c.files};
		LoadOptions options;
		options.allowedRoots.push_back("/r");
		options.files.read = FixtureRead;
		options.files.user = &fixture;
		Preset p;
		std::string error;
		const bool ok = LoadPreset("/r/a.slangp", options, p, error);
		const bool right = ok == c.ok && (ok || error.find(c.expect) != std::string::npos);
		std::printf("    %s %s: %s\n", right ? "ok  " : "FAIL", c.what, ok ? "read" : error.c_str());
		bad += !right;
	}
	// The plan of the history / feedback / lookup case.
	{
		Fixture fixture{{{"/r/a.slangp", "shaders = 2\nshader0 = s.slang\nshader1 = t.slang\nalias0 = First\nframe_count_mod1 = 7\n"
										 "filter_linear1 = true\nwrap_mode0 = repeat\nscale_type0 = absolute\nscale0 = 100\nmipmap_input1 = true\n"},
			{"/r/s.slang", with("OriginalHistory2")}, {"/r/t.slang", with("FirstFeedback")}}};
		LoadOptions options;
		options.allowedRoots.push_back("/r");
		options.files.read = FixtureRead;
		options.files.user = &fixture;
		Preset p;
		std::string error;
		ChainPlan plan;
		FrameInfo frame;
		frame.sourceWidth = 640;
		frame.sourceHeight = 448;
		frame.viewportWidth = 3840;
		frame.viewportHeight = 2160;
		frame.frameCount = 100;
		const bool ok = LoadPreset("/r/a.slangp", options, p, error) && plan.Build(p, frame, &error);
		uint32_t frameCount1 = 0;
		const bool right = ok && p.historyFrames == 2 && p.passes[0].feedback && !p.passes[1].feedback && plan.passes[0].width == 100 &&
						   plan.passes[0].height == 100 && plan.passes[0].mipLevels == 7 && plan.passes[1].width == 3840 &&
						   plan.passes[1].textures[0].texture.kind == TextureRef::PassFeedback && !plan.passes[1].textures[0].linear &&
						   plan.passes[1].textures[0].wrap == Wrap::Repeat && plan.passes[0].textures[0].texture.index == 2 &&
						   (std::memcpy(&frameCount1, plan.passes[1].push.data() + 16, 4), frameCount1 == 100 % 7);
		std::printf("    %s the plan of history, feedback, absolute size, mipmaps and frame_count_mod%s%s\n", right ? "ok  " : "FAIL",
			ok ? "" : ": ", error.c_str());
		bad += !right;
	}
	std::printf("  refuse: %zu cases, %d wrong\n", cases.size() + 1, bad);
	return bad == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "";
	int status = 2;
	if (mode == "check")
		status = Check(argc, argv);
	else if (mode == "fuzz")
		status = Fuzz(argc, argv);
	else if (mode == "refuse")
		status = Refuse();
	if (status == 2)
		std::fprintf(stderr, "usage: slang_test check <work> <root> <preset>... | fuzz <root> <seed> <n> <preset>... | refuse\n");
	return status;
}

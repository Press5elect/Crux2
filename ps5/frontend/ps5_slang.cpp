// Crux2 - libretro shader presets (.slangp) read, made ready for PCSX2's Vulkan compiler, and planned frame by frame.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// ps5_slang.h says what this is. The order here: small helpers; paths and files; the preset
// (.slangp) with its #reference chain; a pass's .slang (includes, pragmas, the two stages); the
// text reflection (a preprocessor, a tokenizer, the blocks and their std140 / std430 layout);
// the meaning of each name; the plan of a frame.
//
// Adapted from PSSwanStation (ps5/src/slangimport.cpp, chain.cpp; GPL-3.0-or-later, the same
// author): normalise / joinPath, the preset reader with #reference, the include expansion and
// the texture and size naming. New here: the path roots, the limits, the GLSL reflection and the
// plan, which PSSwanStation did with SPIR-V and inside its Vulkan runner.

#include "ps5_slang.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <sys/stat.h>

namespace PS5Slang
{
namespace
{
// ------------------------------------------------------------------------------------------------
// Small helpers.

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}
bool IsIdentStart(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsIdentChar(char c)
{
	return IsIdentStart(c) || IsDigit(c);
}

std::string_view Trim(std::string_view s)
{
	while (!s.empty() && IsSpace(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && IsSpace(s.back()))
		s.remove_suffix(1);
	return s;
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
bool EndsWith(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Lower(std::string_view s)
{
	std::string out(s);
	for (char& c : out)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return out;
}

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	const int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	if (n < 0)
		return std::string();
	return std::string(buffer, std::min<size_t>(static_cast<size_t>(n), sizeof(buffer) - 1));
}

// A decimal number as libretro writes them ("1.0", "-0.5", "1e-3", "800"), without the C
// library's locale. False when `s` is not one (trailing "f" allowed, as some presets have).
bool ParseNumber(std::string_view s, double& out)
{
	s = Trim(s);
	if (!s.empty() && (s.back() == 'f' || s.back() == 'F'))
		s.remove_suffix(1);
	size_t i = 0;
	bool negative = false;
	if (i < s.size() && (s[i] == '+' || s[i] == '-'))
		negative = s[i++] == '-';
	double value = 0.0;
	int digits = 0, exponent = 0;
	while (i < s.size() && IsDigit(s[i]))
	{
		if (value < 1e300)
			value = value * 10.0 + (s[i] - '0');
		else
			exponent++;
		i++;
		digits++;
	}
	if (i < s.size() && s[i] == '.')
	{
		i++;
		while (i < s.size() && IsDigit(s[i]))
		{
			if (value < 1e300)
			{
				value = value * 10.0 + (s[i] - '0');
				exponent--;
			}
			i++;
			digits++;
		}
	}
	if (digits == 0)
		return false;
	if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
	{
		i++;
		bool expNegative = false;
		if (i < s.size() && (s[i] == '+' || s[i] == '-'))
			expNegative = s[i++] == '-';
		int e = 0, expDigits = 0;
		while (i < s.size() && IsDigit(s[i]))
		{
			e = std::min(e * 10 + (s[i] - '0'), 100000);
			i++;
			expDigits++;
		}
		if (expDigits == 0)
			return false;
		exponent += expNegative ? -e : e;
	}
	if (i != s.size())
		return false;
	exponent = std::clamp(exponent, -400, 400);
	value *= std::pow(10.0, exponent);
	if (!std::isfinite(value))
		return false;
	out = negative ? -value : value;
	return true;
}

bool ParseFloat(std::string_view s, float& out)
{
	double d = 0.0;
	if (!ParseNumber(s, d) || std::fabs(d) > 3.0e38)
		return false;
	out = static_cast<float>(d);
	return true;
}

bool ParseBool(std::string_view s)
{
	const std::string v = Lower(Trim(s));
	return v == "true" || v == "1" || v == "yes" || v == "on";
}

// "Name<digits>" as the number.
bool Numbered(std::string_view text, std::string_view prefix, uint32_t& number)
{
	if (text.size() <= prefix.size() || !StartsWith(text, prefix) || text.size() - prefix.size() > 3)
		return false;
	uint32_t n = 0;
	for (size_t i = prefix.size(); i < text.size(); i++)
	{
		if (!IsDigit(text[i]))
			return false;
		n = n * 10 + static_cast<uint32_t>(text[i] - '0');
	}
	number = n;
	return true;
}

std::vector<std::string_view> SplitLines(std::string_view text)
{
	std::vector<std::string_view> out;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string_view::npos)
			end = text.size();
		std::string_view line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		out.push_back(line);
		if (end == text.size())
			break;
		start = end + 1;
	}
	return out;
}

// ------------------------------------------------------------------------------------------------
// Paths and files.

std::string DirectoryOf(const std::string& path)
{
	const size_t slash = path.rfind('/');
	if (slash == std::string::npos)
		return ".";
	if (slash == 0)
		return "/";
	return path.substr(0, slash);
}

std::string BaseName(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "a/b/../c" as "a/c" (from PSSwanStation). A ".." above an absolute path's top is dropped.
std::string Normalise(const std::string& path)
{
	std::vector<std::string> parts;
	const bool absolute = !path.empty() && path[0] == '/';
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		if (part == "..")
		{
			if (!parts.empty() && parts.back() != "..")
				parts.pop_back();
			else if (!absolute)
				parts.push_back(part);
		}
		else if (!part.empty() && part != ".")
			parts.push_back(part);
		start = end + 1;
	}
	std::string out = absolute ? "/" : "";
	for (size_t i = 0; i < parts.size(); i++)
	{
		if (i != 0)
			out += '/';
		out += parts[i];
	}
	if (out.empty())
		out = ".";
	return out;
}

bool Inside(const std::string& root, const std::string& path)
{
	if (root == "/")
		return !path.empty() && path[0] == '/';
	return path == root || (path.size() > root.size() && StartsWith(path, root) && path[root.size()] == '/');
}

struct Context
{
	const LoadOptions& options;
	std::vector<std::string> roots;
	std::string error;
	std::vector<std::string> warnings;

	bool Fail(std::string message)
	{
		if (error.empty())
			error = std::move(message);
		return false;
	}
};

// `name` as written in `fromFile` (relative to its folder, or absolute) to a path, refused when it
// steps up too many times or lands outside the roots.
bool Resolve(Context& ctx, const std::string& fromFile, std::string_view name, std::string& out)
{
	std::string clean(Trim(name));
	if (clean.empty())
		return ctx.Fail("an empty file name in " + BaseName(fromFile));
	if (clean.size() > 1024)
		return ctx.Fail("a file name in " + BaseName(fromFile) + " is too long");
	for (char& c : clean)
	{
		if (c == '\\')
			c = '/';
		if (c == '\0')
			return ctx.Fail("a file name in " + BaseName(fromFile) + " is not text");
	}
	int steps = 0;
	for (size_t i = 0; i + 1 < clean.size(); i++)
		if (clean[i] == '.' && clean[i + 1] == '.' && (i == 0 || clean[i - 1] == '/') &&
			(i + 2 == clean.size() || clean[i + 2] == '/'))
			steps++;
	if (steps > ctx.options.maxParentSteps)
		return ctx.Fail("\"" + clean + "\" (in " + BaseName(fromFile) + ") goes up too many folders");
	const std::string full = clean[0] == '/' ? Normalise(clean) : Normalise(DirectoryOf(fromFile) + "/" + clean);
	for (const std::string& root : ctx.roots)
		if (Inside(root, full))
		{
			out = full;
			return true;
		}
	return ctx.Fail("\"" + clean + "\" (in " + BaseName(fromFile) + ") is outside the shader folders");
}

bool DefaultRead(void*, const std::string& path, size_t maxBytes, std::string& out, std::string& error)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
	{
		error = "cannot find " + BaseName(path);
		return false;
	}
	if (!S_ISREG(st.st_mode))
	{
		error = BaseName(path) + " is not a file";
		return false;
	}
	if (static_cast<uint64_t>(st.st_size) > maxBytes)
	{
		error = BaseName(path) + " is larger than a shader file may be";
		return false;
	}
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		error = "cannot open " + BaseName(path);
		return false;
	}
	out.assign(static_cast<size_t>(st.st_size), '\0');
	const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	if (got != out.size())
	{
		error = "cannot read " + BaseName(path);
		return false;
	}
	return true;
}

bool ReadFile(Context& ctx, const std::string& path, size_t maxBytes, std::string& out)
{
	std::string why;
	const FileSource& fs = ctx.options.files;
	const bool ok = fs.read ? fs.read(fs.user, path, maxBytes, out, why) : DefaultRead(nullptr, path, maxBytes, out, why);
	if (!ok)
		return ctx.Fail(why.empty() ? "cannot read " + BaseName(path) : why);
	if (out.size() > maxBytes)
		return ctx.Fail(BaseName(path) + " is larger than a shader file may be");
	return true;
}

// ------------------------------------------------------------------------------------------------
// The preset (.slangp): "key = value" lines, '#' comments, #reference to a preset whose values
// come first (each value's paths are from the folder of the file that set it).

struct Value
{
	std::string text, file;
};
using Settings = std::map<std::string, Value, std::less<>>;

bool ReadSettings(Context& ctx, const std::string& path, Settings& settings, int depth)
{
	if (depth > ctx.options.maxReferenceDepth)
		return ctx.Fail("presets refer to each other too deeply (at " + BaseName(path) + ")");
	std::string text;
	if (!ReadFile(ctx, path, ctx.options.maxFileBytes, text))
		return false;
	for (std::string_view raw : SplitLines(text))
	{
		std::string_view line = Trim(raw);
		if (StartsWith(line, "#reference") || StartsWith(line, "#include"))
		{
			std::string_view inner = Trim(line.substr(line[1] == 'r' ? 10 : 8));
			if (inner.size() >= 2 && inner.front() == '"')
			{
				const size_t close = inner.find('"', 1);
				inner = inner.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
			}
			std::string target;
			if (!Resolve(ctx, path, inner, target) || !ReadSettings(ctx, target, settings, depth + 1))
				return false;
			continue;
		}
		if (line.empty() || line[0] == '#' || line[0] == ';' || StartsWith(line, "//"))
			continue;
		const size_t equals = line.find('=');
		if (equals == std::string_view::npos)
			continue;
		const std::string_view key = Trim(line.substr(0, equals));
		std::string_view value = Trim(line.substr(equals + 1));
		if (!value.empty() && value[0] == '"')
		{
			const size_t close = value.find('"', 1);
			value = value.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
		}
		else
		{
			// A comment after the value.
			size_t cut = value.find('#');
			cut = std::min(cut, value.find("//"));
			if (cut != std::string_view::npos)
				value = Trim(value.substr(0, cut));
		}
		bool keyOk = !key.empty() && key.size() <= 128;
		for (const char c : key)
			keyOk = keyOk && (IsIdentChar(c) || c == '-' || c == '.');
		if (keyOk)
			settings[std::string(key)] = Value{std::string(value), path};
	}
	return true;
}

const Value* Find(const Settings& s, const std::string& key)
{
	const auto it = s.find(key);
	return it == s.end() ? nullptr : &it->second;
}

std::string Get(const Settings& s, const std::string& key, std::string_view otherwise = {})
{
	const Value* v = Find(s, key);
	return v ? v->text : std::string(otherwise);
}

bool WrapNamed(std::string_view name, Wrap& out)
{
	const std::string n = Lower(Trim(name));
	if (n == "clamp_to_border" || n.empty())
		out = Wrap::ClampToBorder;
	else if (n == "clamp_to_edge")
		out = Wrap::ClampToEdge;
	else if (n == "repeat")
		out = Wrap::Repeat;
	else if (n == "mirrored_repeat")
		out = Wrap::MirroredRepeat;
	else
		return false;
	return true;
}

bool ScaleNamed(std::string_view name, ScaleType& out)
{
	const std::string n = Lower(Trim(name));
	if (n == "source")
		out = ScaleType::Source;
	else if (n == "viewport")
		out = ScaleType::Viewport;
	else if (n == "absolute")
		out = ScaleType::Absolute;
	else
		return false;
	return true;
}

// "a;b;c" as its parts.
std::vector<std::string> SplitList(std::string_view list)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= list.size())
	{
		size_t end = list.find(';', start);
		if (end == std::string_view::npos)
			end = list.size();
		const std::string_view item = Trim(list.substr(start, end - start));
		if (!item.empty())
			out.emplace_back(item);
		start = end + 1;
	}
	return out;
}

// ------------------------------------------------------------------------------------------------
// A pass's .slang: the includes put in (each where it is named, as RetroArch does; the files keep
// themselves from being read twice with #ifndef), #line marks so the compiler's messages name the
// right file and line, the pragmas taken out, the two stages apart.

struct Source
{
	std::string vertex, fragment, name, format;
	struct Declared
	{
		std::string name, description;
		float initial, minimum, maximum, step;
	};
	std::vector<Declared> parameters;
};

// A file's name as it can stand in a #line string.
std::string LineName(const std::string& path)
{
	std::string out;
	for (const char c : BaseName(path))
		out += (IsIdentChar(c) || c == '.' || c == '-') ? c : '_';
	return out;
}

bool Expand(Context& ctx, const std::string& path, int depth, std::vector<std::string>& out, size_t& total)
{
	if (depth > ctx.options.maxIncludeDepth)
		return ctx.Fail("#include goes too deep (at " + BaseName(path) + ")");
	std::string text;
	if (!ReadFile(ctx, path, ctx.options.maxFileBytes, text))
		return false;
	if (text.find('\0') != std::string::npos)
		return ctx.Fail(BaseName(path) + " is not text");
	const std::vector<std::string_view> lines = SplitLines(text);
	const std::string name = LineName(path);
	for (size_t n = 0; n < lines.size(); n++)
	{
		const std::string_view trimmed = Trim(lines[n]);
		if (StartsWith(trimmed, "#include"))
		{
			const size_t open = trimmed.find('"'), close = trimmed.rfind('"');
			if (open == std::string_view::npos || close <= open)
				return ctx.Fail(Format("an #include in %s (line %zu) is not understood", BaseName(path).c_str(), n + 1));
			std::string inner;
			if (!Resolve(ctx, path, trimmed.substr(open + 1, close - open - 1), inner))
				return false;
			out.push_back(Format("#line 1 \"%s\"", LineName(inner).c_str()));
			if (!Expand(ctx, inner, depth + 1, out, total))
				return false;
			out.push_back(Format("#line %zu \"%s\"", n + 2, name.c_str()));
			continue;
		}
		total += lines[n].size() + 1;
		if (total > ctx.options.maxSourceBytes)
			return ctx.Fail(BaseName(path) + " with its includes is larger than a pass may be");
		out.emplace_back(lines[n]);
	}
	return true;
}

// "#pragma word rest": the rest, when the line is that pragma.
bool Pragma(std::string_view line, std::string_view word, std::string_view& rest)
{
	std::string_view text = Trim(line);
	if (!StartsWith(text, "#"))
		return false;
	text = Trim(text.substr(1));
	if (!StartsWith(text, "pragma"))
		return false;
	text = Trim(text.substr(6));
	if (!StartsWith(text, word) || (text.size() > word.size() && !IsSpace(text[word.size()])))
		return false;
	rest = Trim(text.substr(word.size()));
	return true;
}

bool ParseParameter(std::string_view rest, Source::Declared& p)
{
	// NAME "description" INITIAL MINIMUM MAXIMUM [STEP]
	size_t i = 0;
	while (i < rest.size() && !IsSpace(rest[i]) && rest[i] != '"')
		i++;
	// RetroArch reads the name as any word (%63s): separators in menus are named "row-1" and such.
	p.name = std::string(rest.substr(0, i));
	if (p.name.empty() || p.name.size() > 63)
		return false;
	const size_t open = rest.find('"', i);
	if (open == std::string_view::npos)
		return false;
	const size_t close = rest.find('"', open + 1);
	if (close == std::string_view::npos)
		return false;
	p.description = std::string(rest.substr(open + 1, close - open - 1));
	// The numbers as RetroArch's sscanf("%f %f %f %f") takes them: each the longest number at
	// the start of what is left ("0.0.001" is 0.0, then .001).
	float values[4] = {0.f, 0.f, 0.f, 0.f};
	int count = 0;
	std::string_view numbers = rest.substr(close + 1);
	while (count < 4)
	{
		numbers = Trim(numbers);
		size_t best = 0;
		for (size_t len = 1; len <= std::min<size_t>(numbers.size(), 48); len++)
		{
			float f = 0.f;
			const std::string_view part = numbers.substr(0, len);
			if (part.back() != 'f' && part.back() != 'F' && ParseFloat(part, f))
			{
				best = len;
				values[count] = f;
			}
		}
		if (best == 0)
			break;
		count++;
		numbers = numbers.substr(best);
	}
	if (count < 3)
		return false;
	p.initial = values[0];
	p.minimum = values[1];
	p.maximum = values[2];
	p.step = count >= 4 ? values[3] : 0.1f * (values[2] - values[1]);
	return true;
}

bool LoadSource(Context& ctx, const std::string& path, Source& source)
{
	std::vector<std::string> all;
	size_t total = 0;
	if (!Expand(ctx, path, 0, all, total))
		return false;
	// The #version line comes first; the line directive extension right after it.
	std::string common, vertex, fragment;
	std::string* current = &common;
	bool sawVertex = false, sawFragment = false, sawVersion = false;
	// Where each line came from, for the #line put at the head of each stage's own lines.
	std::string fileName = LineName(path);
	size_t fileLine = 0;
	for (std::string& line : all)
	{
		fileLine++;
		std::string_view rest;
		const std::string_view trimmed = Trim(line);
		if (StartsWith(trimmed, "#line "))
		{
			// One Expand put in: "#line N \"file\"".
			double n = 0;
			const std::string_view after = Trim(trimmed.substr(6));
			const size_t space = after.find(' ');
			if (space != std::string_view::npos && ParseNumber(after.substr(0, space), n) && n >= 1 && n < 1e7)
			{
				fileLine = static_cast<size_t>(n) - 1;
				std::string_view quoted = Trim(after.substr(space));
				if (quoted.size() >= 2 && quoted.front() == '"' && quoted.back() == '"')
					fileName = std::string(quoted.substr(1, quoted.size() - 2));
			}
		}
		if (!sawVersion && StartsWith(trimmed, "#version"))
		{
			sawVersion = true;
			*current += line;
			*current += "\n#extension GL_GOOGLE_cpp_style_line_directive : require\n";
			*current += Format("#line %zu \"%s\"\n", fileLine + 1, fileName.c_str());
			continue;
		}
		if (Pragma(line, "parameter", rest))
		{
			Source::Declared p;
			if (!ParseParameter(rest, p))
				return ctx.Fail(Format("a #pragma parameter in %s is not understood: %.60s", BaseName(path).c_str(),
					std::string(rest).c_str()));
			source.parameters.push_back(std::move(p));
			line.clear();
		}
		else if (Pragma(line, "name", rest))
		{
			source.name = std::string(rest.substr(0, std::min(rest.find_first_of(" \t"), rest.size())));
			line.clear();
		}
		else if (Pragma(line, "format", rest))
		{
			source.format = std::string(rest.substr(0, std::min(rest.find_first_of(" \t"), rest.size())));
			line.clear();
		}
		else if (Pragma(line, "stage", rest))
		{
			if (StartsWith(rest, "vertex"))
			{
				current = &vertex;
				sawVertex = true;
			}
			else if (StartsWith(rest, "fragment"))
			{
				current = &fragment;
				sawFragment = true;
			}
			else
				return ctx.Fail(BaseName(path) + " names a stage other than vertex and fragment");
			line = Format("#line %zu \"%s\"", fileLine + 1, fileName.c_str());
		}
		// What was taken out stays a blank line, so the line numbers hold.
		*current += line;
		*current += '\n';
	}
	if (!sawVersion)
		return ctx.Fail(BaseName(path) + " has no #version line");
	if (!sawVertex || !sawFragment)
		return ctx.Fail(BaseName(path) + " does not have both stages (#pragma stage vertex / fragment)");
	// Each stage carries the lines before the first #pragma stage; the other stage's lines are
	// left out entirely (#line keeps the numbers of what follows right).
	source.vertex = common + vertex;
	source.fragment = common + fragment;
	return true;
}

// ------------------------------------------------------------------------------------------------
// Reflection on the text. First what glslang's preprocessor would leave: comments out, lines
// joined at a backslash, #if / #ifdef / #else / #endif followed, #define'd names put in place.
// Then the declarations at file scope: `layout(...) uniform Name { members } instance;` and
// `layout(...) uniform sampler2D Name;`.

struct Token
{
	enum Kind : uint8_t
	{
		Ident,
		Number,
		Punct,
	};
	Kind kind = Punct;
	std::string text;
};

// Comments to spaces (newlines kept), strings left alone.
std::string StripComments(std::string_view in)
{
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	bool inString = false;
	while (i < in.size())
	{
		const char c = in[i];
		if (inString)
		{
			out += c;
			if (c == '"' || c == '\n')
				inString = false;
			i++;
		}
		else if (c == '"')
		{
			inString = true;
			out += c;
			i++;
		}
		else if (c == '/' && i + 1 < in.size() && in[i + 1] == '/')
		{
			while (i < in.size() && in[i] != '\n')
				i++;
			out += ' ';
		}
		else if (c == '/' && i + 1 < in.size() && in[i + 1] == '*')
		{
			i += 2;
			out += ' ';
			while (i < in.size() && !(in[i] == '*' && i + 1 < in.size() && in[i + 1] == '/'))
			{
				if (in[i] == '\n')
					out += '\n';
				i++;
			}
			i = std::min(i + 2, in.size());
		}
		else
		{
			out += c;
			i++;
		}
	}
	return out;
}

void Tokenize(std::string_view line, std::vector<Token>& out)
{
	size_t i = 0;
	while (i < line.size())
	{
		const char c = line[i];
		if (IsSpace(c))
		{
			i++;
			continue;
		}
		Token t;
		if (IsIdentStart(c))
		{
			size_t j = i + 1;
			while (j < line.size() && IsIdentChar(line[j]))
				j++;
			t.kind = Token::Ident;
			t.text = std::string(line.substr(i, j - i));
			i = j;
		}
		else if (IsDigit(c) || (c == '.' && i + 1 < line.size() && IsDigit(line[i + 1])))
		{
			size_t j = i + 1;
			while (j < line.size() && (IsIdentChar(line[j]) || line[j] == '.' ||
										  ((line[j] == '+' || line[j] == '-') && (line[j - 1] == 'e' || line[j - 1] == 'E') &&
											  !(line[i] == '0' && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X')))))
				j++;
			t.kind = Token::Number;
			t.text = std::string(line.substr(i, j - i));
			i = j;
		}
		else if (c == '"')
		{
			size_t j = line.find('"', i + 1);
			j = j == std::string_view::npos ? line.size() : j + 1;
			t.kind = Token::Punct;
			t.text = std::string(line.substr(i, j - i));
			i = j;
		}
		else
		{
			static const char* const twos[] = {"&&", "||", "==", "!=", "<=", ">=", "<<", ">>", "##", "++", "--", "+=", "-=", "*=", "/="};
			t.kind = Token::Punct;
			t.text = std::string(1, c);
			if (i + 1 < line.size())
				for (const char* two : twos)
					if (two[0] == c && two[1] == line[i + 1])
					{
						t.text = two;
						break;
					}
			i += t.text.size();
		}
		out.push_back(std::move(t));
	}
}

struct Macro
{
	bool function = false;
	std::vector<std::string> params;
	std::vector<Token> body;
};
using Macros = std::map<std::string, Macro, std::less<>>;
const char* const UNCLOSED_CALL = "a macro's arguments are not closed";

// Puts the macros in place (object-like and function-like; # and ## are not done, the shaders
// libretro has do not use them where the reflection looks). `hidden` keeps a macro from
// expanding inside itself.
bool ExpandTokens(const Macros& macros, const std::vector<Token>& in, std::vector<Token>& out, std::set<std::string>& hidden,
	int depth, std::string& error)
{
	if (depth > 64)
	{
		error = "macros expand into each other too deeply";
		return false;
	}
	for (size_t i = 0; i < in.size(); i++)
	{
		const Token& t = in[i];
		if (out.size() > 200000)
		{
			error = "macros expand into too much text";
			return false;
		}
		const auto it = t.kind == Token::Ident && !hidden.count(t.text) ? macros.find(t.text) : macros.end();
		if (it == macros.end())
		{
			out.push_back(t);
			continue;
		}
		const Macro& m = it->second;
		std::vector<Token> replaced;
		if (m.function)
		{
			if (i + 1 >= in.size() || in[i + 1].text != "(")
			{
				out.push_back(t);
				continue;
			}
			std::vector<std::vector<Token>> args(1);
			size_t j = i + 2;
			int level = 0;
			for (; j < in.size(); j++)
			{
				const std::string& s = in[j].text;
				if (s == "(")
					level++;
				else if (s == ")")
				{
					if (level == 0)
						break;
					level--;
				}
				else if (s == "," && level == 0)
				{
					args.emplace_back();
					continue;
				}
				args.back().push_back(in[j]);
			}
			if (j >= in.size())
			{
				error = UNCLOSED_CALL;
				return false;
			}
			if (m.params.empty() && args.size() == 1 && args[0].empty())
				args.clear();
			if (args.size() != m.params.size())
			{
				error = "macro " + t.text + " is given the wrong number of arguments";
				return false;
			}
			for (const Token& b : m.body)
			{
				const auto p = b.kind == Token::Ident ? std::find(m.params.begin(), m.params.end(), b.text) : m.params.end();
				if (p == m.params.end())
					replaced.push_back(b);
				else
				{
					// Arguments are expanded before they are put in (as C does when no # / ## is near).
					std::vector<Token> arg;
					if (!ExpandTokens(macros, args[static_cast<size_t>(p - m.params.begin())], arg, hidden, depth + 1, error))
						return false;
					replaced.insert(replaced.end(), arg.begin(), arg.end());
				}
			}
			i = j;
		}
		else
			replaced = m.body;
		hidden.insert(t.text);
		const bool ok = ExpandTokens(macros, replaced, out, hidden, depth + 1, error);
		hidden.erase(t.text);
		if (!ok)
			return false;
	}
	return true;
}

// #if's expressions, over 64-bit integers, as C's preprocessor has them.
struct ExprParser
{
	const std::vector<Token>& t;
	size_t i = 0;
	bool ok = true;
	int depth = 0;

	const std::string& Peek() const
	{
		static const std::string end;
		return i < t.size() ? t[i].text : end;
	}
	bool Accept(const char* s)
	{
		if (Peek() == s)
		{
			i++;
			return true;
		}
		return false;
	}
	int64_t Primary()
	{
		if (++depth > 200)
		{
			ok = false;
			return 0;
		}
		int64_t v = 0;
		if (i >= t.size())
			ok = false;
		else if (Accept("("))
		{
			v = Conditional();
			if (!Accept(")"))
				ok = false;
		}
		else if (Accept("!"))
			v = !Primary();
		else if (Accept("~"))
			v = ~Primary();
		else if (Accept("-"))
			v = static_cast<int64_t>(0ull - static_cast<uint64_t>(Primary()));
		else if (Accept("+"))
			v = Primary();
		else if (t[i].kind == Token::Number)
		{
			const std::string& s = t[i++].text;
			char* end = nullptr;
			const unsigned long long n = std::strtoull(s.c_str(), &end, 0);
			if (end == s.c_str())
				ok = false;
			while (end && (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L'))
				end++;
			if (end && *end != '\0')
				ok = false; // a float in #if: glslang refuses it too
			v = static_cast<int64_t>(n);
		}
		else if (t[i].kind == Token::Ident)
			i++, v = 0; // a name no macro made is 0
		else
			ok = false;
		depth--;
		return v;
	}
	int64_t Binary(int level)
	{
		// Levels, loosest first: || && | ^ & ==,!= <,>,<=,>= <<,>> +,- *,/,%
		static const std::vector<std::vector<const char*>> ops = {
			{"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
		if (level >= static_cast<int>(ops.size()))
			return Primary();
		int64_t left = Binary(level + 1);
		for (;;)
		{
			const char* op = nullptr;
			for (const char* o : ops[static_cast<size_t>(level)])
				if (Peek() == o)
					op = o;
			if (!op || !ok)
				return left;
			i++;
			const int64_t right = Binary(level + 1);
			const uint64_t ul = static_cast<uint64_t>(left), ur = static_cast<uint64_t>(right);
			const std::string_view o(op);
			if (o == "||") left = left || right;
			else if (o == "&&") left = left && right;
			else if (o == "|") left = left | right;
			else if (o == "^") left = left ^ right;
			else if (o == "&") left = left & right;
			else if (o == "==") left = left == right;
			else if (o == "!=") left = left != right;
			else if (o == "<") left = left < right;
			else if (o == ">") left = left > right;
			else if (o == "<=") left = left <= right;
			else if (o == ">=") left = left >= right;
			else if (o == "<<") left = static_cast<int64_t>(ul << (ur & 63));
			else if (o == ">>") left = left >> (ur & 63);
			else if (o == "+") left = static_cast<int64_t>(ul + ur);
			else if (o == "-") left = static_cast<int64_t>(ul - ur);
			else if (o == "*") left = static_cast<int64_t>(ul * ur);
			else if (right == 0 || (left == INT64_MIN && right == -1)) { ok = false; return 0; }
			else if (o == "/") left = left / right;
			else left = left % right;
		}
	}
	int64_t Conditional()
	{
		const int64_t c = Binary(0);
		if (Accept("?"))
		{
			const int64_t a = Conditional();
			if (!Accept(":"))
				ok = false;
			const int64_t b = Conditional();
			return c ? a : b;
		}
		return c;
	}
};

bool Evaluate(const Macros& macros, const std::vector<Token>& raw, int64_t& value, std::string& error)
{
	// defined NAME / defined(NAME) first, before the macros are put in.
	std::vector<Token> pre;
	for (size_t i = 0; i < raw.size(); i++)
	{
		if (raw[i].kind == Token::Ident && raw[i].text == "defined")
		{
			size_t j = i + 1;
			const bool paren = j < raw.size() && raw[j].text == "(";
			if (paren)
				j++;
			if (j >= raw.size() || raw[j].kind != Token::Ident || (paren && (j + 1 >= raw.size() || raw[j + 1].text != ")")))
			{
				error = "a defined() in #if is not understood";
				return false;
			}
			Token t;
			t.kind = Token::Number;
			t.text = macros.count(raw[j].text) ? "1" : "0";
			pre.push_back(t);
			i = paren ? j + 1 : j;
			continue;
		}
		pre.push_back(raw[i]);
	}
	std::vector<Token> expanded;
	std::set<std::string> hidden;
	if (!ExpandTokens(macros, pre, expanded, hidden, 0, error))
		return false;
	ExprParser p{expanded};
	value = p.Conditional();
	if (!p.ok || p.i != expanded.size())
	{
		error = "an #if expression is not understood";
		return false;
	}
	return true;
}

// The tokens glslang compiles, from a stage's text.
bool Preprocess(const std::string& source, bool vertex, std::vector<Token>& out, std::string& error)
{
	const std::string text = StripComments(source);
	Macros macros;
	auto define = [&](const char* name, const char* value) {
		Macro m;
		if (value)
			Tokenize(value, m.body);
		macros[name] = std::move(m);
	};
	define("VULKAN", "100");
	define("GL_SPIRV", "100");
	define("GL_core_profile", "1");
	define("__VERSION__", "450");
	(void)vertex;
	struct Cond
	{
		bool parentActive, active, taken, sawElse;
	};
	std::vector<Cond> conds;
	auto active = [&] { return conds.empty() || conds.back().active; };
	const std::vector<std::string_view> lines = SplitLines(text);
	// Code lines are expanded as they come, with the macros defined so far (a name #defined
	// after a block does not change the block). A macro call whose arguments go on past the
	// line waits for the lines after it.
	std::vector<Token> pending;
	auto flush = [&](bool force) {
		if (pending.empty())
			return true;
		std::vector<Token> expanded;
		std::set<std::string> hidden;
		std::string why;
		if (ExpandTokens(macros, pending, expanded, hidden, 0, why))
		{
			out.insert(out.end(), expanded.begin(), expanded.end());
			pending.clear();
			return true;
		}
		if (why != UNCLOSED_CALL)
		{
			error = why;
			return false;
		}
		if (force || pending.size() > 4096)
		{
			// Left as written: the reflection does not need what is inside such a call.
			out.insert(out.end(), pending.begin(), pending.end());
			pending.clear();
		}
		return true;
	};
	for (size_t n = 0; n < lines.size(); n++)
	{
		// Lines ending in a backslash go on.
		std::string line(lines[n]);
		while (!line.empty() && line.back() == '\\' && n + 1 < lines.size())
		{
			line.pop_back();
			line += lines[++n];
		}
		const std::string_view trimmed = Trim(line);
		if (!StartsWith(trimmed, "#"))
		{
			if (active())
			{
				Tokenize(line, pending);
				if (!flush(false))
					return false;
			}
			continue;
		}
		if (!flush(true))
			return false;
		if (out.size() > 2000000)
		{
			error = "the shader is too large for the reflection";
			return false;
		}
		std::vector<Token> words;
		Tokenize(trimmed.substr(1), words);
		if (words.empty())
			continue;
		const std::string directive = words[0].text;
		const std::vector<Token> rest(words.begin() + 1, words.end());
		if (directive == "ifdef" || directive == "ifndef")
		{
			if (rest.empty() || rest[0].kind != Token::Ident)
			{
				error = "#" + directive + " without a name";
				return false;
			}
			const bool parent = active();
			const bool test = (macros.count(rest[0].text) != 0) == (directive == "ifdef");
			conds.push_back({parent, parent && test, test, false});
		}
		else if (directive == "if")
		{
			const bool parent = active();
			int64_t v = 0;
			if (parent && !Evaluate(macros, rest, v, error))
				return false;
			conds.push_back({parent, parent && v != 0, v != 0, false});
		}
		else if (directive == "elif")
		{
			if (conds.empty() || conds.back().sawElse)
			{
				error = "#elif without #if";
				return false;
			}
			Cond& c = conds.back();
			if (c.taken || !c.parentActive)
				c.active = false;
			else
			{
				int64_t v = 0;
				if (!Evaluate(macros, rest, v, error))
					return false;
				c.active = v != 0;
				c.taken = c.active;
			}
		}
		else if (directive == "else")
		{
			if (conds.empty() || conds.back().sawElse)
			{
				error = "#else without #if";
				return false;
			}
			Cond& c = conds.back();
			c.active = c.parentActive && !c.taken;
			c.taken = true;
			c.sawElse = true;
		}
		else if (directive == "endif")
		{
			if (conds.empty())
			{
				error = "#endif without #if";
				return false;
			}
			conds.pop_back();
		}
		else if (!active())
			continue;
		else if (directive == "define")
		{
			if (rest.empty() || rest[0].kind != Token::Ident)
			{
				error = "#define without a name";
				return false;
			}
			if (macros.size() > 10000)
			{
				error = "too many #defines";
				return false;
			}
			Macro m;
			size_t body = 1;
			// A function-like macro has its '(' right after the name, with no space.
			const std::string_view after = Trim(trimmed.substr(1));
			const size_t namePos = after.find(rest[0].text, after.find("define") + 6);
			const bool function = namePos != std::string_view::npos && namePos + rest[0].text.size() < after.size() &&
								  after[namePos + rest[0].text.size()] == '(';
			if (function)
			{
				m.function = true;
				size_t j = 2;
				while (j < rest.size() && rest[j].text != ")")
				{
					if (rest[j].kind == Token::Ident)
						m.params.push_back(rest[j].text);
					j++;
				}
				body = j + 1;
			}
			if (body < rest.size())
				m.body.assign(rest.begin() + static_cast<std::ptrdiff_t>(body), rest.end());
			macros[rest[0].text] = std::move(m);
		}
		else if (directive == "undef")
		{
			if (!rest.empty())
				macros.erase(rest[0].text);
		}
		else if (directive == "error")
		{
			error = "the shader stops itself with #error";
			return false;
		}
		// #version, #extension, #line, #pragma: nothing for the reflection.
	}
	if (!conds.empty())
	{
		error = "an #if is not closed with #endif";
		return false;
	}
	return flush(true);
}

// A type a block may hold: scalars, vectors, matrices.
struct TypeInfo
{
	BaseType base = BaseType::Float;
	uint8_t columns = 1, rows = 1;
};

bool TypeNamed(const std::string& name, TypeInfo& t)
{
	static const std::map<std::string, TypeInfo, std::less<>> types = [] {
		std::map<std::string, TypeInfo, std::less<>> m;
		m["float"] = {BaseType::Float, 1, 1};
		m["int"] = {BaseType::Int, 1, 1};
		m["uint"] = {BaseType::Uint, 1, 1};
		m["bool"] = {BaseType::Bool, 1, 1};
		for (uint8_t n = 2; n <= 4; n++)
		{
			const std::string d(1, static_cast<char>('0' + n));
			m["vec" + d] = {BaseType::Float, 1, n};
			m["ivec" + d] = {BaseType::Int, 1, n};
			m["uvec" + d] = {BaseType::Uint, 1, n};
			m["bvec" + d] = {BaseType::Bool, 1, n};
			m["mat" + d] = {BaseType::Float, n, n};
			for (uint8_t r = 2; r <= 4; r++)
				m["mat" + d + "x" + std::string(1, static_cast<char>('0' + r))] = {BaseType::Float, n, r};
		}
		return m;
	}();
	const auto it = types.find(name);
	if (it == types.end())
		return false;
	t = it->second;
	return true;
}

uint32_t RoundUp(uint32_t v, uint32_t a)
{
	return (v + a - 1) / a * a;
}

uint32_t VectorAlign(uint32_t components)
{
	return components == 1 ? 4u : components == 2 ? 8u : 16u;
}

struct Layout
{
	uint32_t size = 0, align = 4, matrixStride = 0, arrayStride = 0;
};

// The std140 / std430 rules (GLSL 4.50, 7.6.2.2), for what libretro's blocks hold.
Layout LayoutOf(const TypeInfo& t, bool rowMajor, uint32_t arrayLength, bool std430)
{
	Layout l;
	if (t.columns == 1)
	{
		l.size = 4u * t.rows;
		l.align = VectorAlign(t.rows);
	}
	else
	{
		// A matrix is an array of its columns (rows when row major).
		const uint32_t vectors = rowMajor ? t.rows : t.columns;
		const uint32_t components = rowMajor ? t.columns : t.rows;
		uint32_t stride = VectorAlign(components);
		if (!std430)
			stride = RoundUp(stride, 16);
		l.matrixStride = stride;
		l.size = stride * vectors;
		l.align = stride;
	}
	if (arrayLength > 0)
	{
		uint32_t elementAlign = l.align;
		if (!std430)
			elementAlign = RoundUp(elementAlign, 16);
		l.arrayStride = RoundUp(l.size, elementAlign);
		l.size = l.arrayStride * arrayLength;
		l.align = elementAlign;
	}
	else if (!std430 && t.columns > 1)
		l.align = RoundUp(l.align, 16);
	return l;
}

// A layout(...) list: names, with values where they have one.
bool ParseLayout(const std::vector<Token>& t, size_t& i, std::map<std::string, int64_t>& out, std::string& error)
{
	// t[i] is "layout"
	i++;
	if (i >= t.size() || t[i].text != "(")
	{
		error = "layout without (";
		return false;
	}
	i++;
	while (i < t.size() && t[i].text != ")")
	{
		if (t[i].kind != Token::Ident)
		{
			error = "a layout qualifier is not understood";
			return false;
		}
		const std::string name = t[i++].text;
		int64_t value = 1;
		if (i < t.size() && t[i].text == "=")
		{
			i++;
			std::vector<Token> expr;
			int level = 0;
			while (i < t.size() && !(level == 0 && (t[i].text == "," || t[i].text == ")")))
			{
				if (t[i].text == "(")
					level++;
				if (t[i].text == ")")
					level--;
				expr.push_back(t[i++]);
			}
			Macros none;
			if (!Evaluate(none, expr, value, error))
			{
				error = "layout(" + name + " = ...) is not a number the reflection can work out";
				return false;
			}
		}
		out[name] = value;
		if (i < t.size() && t[i].text == ",")
			i++;
	}
	if (i >= t.size())
	{
		error = "layout( is not closed";
		return false;
	}
	i++;
	return true;
}

bool IsQualifier(const std::string& s)
{
	static const std::set<std::string, std::less<>> q = {"highp", "mediump", "lowp", "precise", "invariant", "flat",
		"smooth", "noperspective", "const", "readonly", "writeonly", "coherent", "volatile", "restrict"};
	return q.count(s) != 0;
}

// The members between a block's braces. `t[i]` is just after '{'; on return just after '}'.
bool ParseMembers(const std::vector<Token>& t, size_t& i, bool std430, bool blockRowMajor, std::vector<Member>& members,
	uint32_t& end, std::string& error)
{
	uint32_t offset = 0;
	end = 0;
	while (i < t.size() && t[i].text != "}")
	{
		std::map<std::string, int64_t> layout;
		bool rowMajor = blockRowMajor;
		// Qualifiers.
		for (;;)
		{
			if (i < t.size() && t[i].text == "layout")
			{
				if (!ParseLayout(t, i, layout, error))
					return false;
				if (layout.count("row_major"))
					rowMajor = true;
				if (layout.count("column_major"))
					rowMajor = false;
			}
			else if (i < t.size() && IsQualifier(t[i].text))
				i++;
			else
				break;
		}
		if (i >= t.size())
			break;
		TypeInfo type;
		if (t[i].kind != Token::Ident || !TypeNamed(t[i].text, type))
		{
			error = "a block member of type " + t[i].text.substr(0, 40) + " is not handled";
			return false;
		}
		i++;
		bool first = true;
		for (;;)
		{
			if (i >= t.size() || t[i].kind != Token::Ident)
			{
				error = "a block member has no name";
				return false;
			}
			Member m;
			m.name = t[i++].text;
			uint32_t length = 0;
			if (i < t.size() && t[i].text == "[")
			{
				i++;
				std::vector<Token> expr;
				while (i < t.size() && t[i].text != "]")
					expr.push_back(t[i++]);
				if (i >= t.size())
				{
					error = "an array in a block is not closed";
					return false;
				}
				i++;
				int64_t n = 0;
				Macros none;
				if (!Evaluate(none, expr, n, error) || n < 1 || n > 4096)
				{
					error = "the length of " + m.name + "[] is not a number the reflection can work out";
					return false;
				}
				length = static_cast<uint32_t>(n);
				if (i < t.size() && t[i].text == "[")
				{
					error = m.name + " is an array of arrays, which is not handled";
					return false;
				}
			}
			const Layout l = LayoutOf(type, rowMajor, length, std430);
			if (first && layout.count("offset"))
			{
				const int64_t o = layout["offset"];
				if (o < 0 || o > 65536 || static_cast<uint32_t>(o) % l.align != 0 || static_cast<uint32_t>(o) < offset)
				{
					error = "the offset given to " + m.name + " is not one glslang takes";
					return false;
				}
				offset = static_cast<uint32_t>(o);
			}
			else
				offset = RoundUp(offset, l.align);
			m.offset = offset;
			m.size = l.size;
			m.base = type.base;
			m.columns = type.columns;
			m.rows = type.rows;
			m.rowMajor = type.columns > 1 && rowMajor;
			m.matrixStride = l.matrixStride;
			m.arrayLength = length;
			m.arrayStride = l.arrayStride;
			offset += l.size;
			end = std::max(end, offset);
			if (offset > 65536)
			{
				error = "a block is too large";
				return false;
			}
			if (members.size() >= 1024)
			{
				error = "a block has too many members";
				return false;
			}
			members.push_back(std::move(m));
			first = false;
			if (i < t.size() && t[i].text == ",")
			{
				i++;
				continue;
			}
			break;
		}
		if (i >= t.size() || t[i].text != ";")
		{
			error = "a block member is not ended with ;";
			return false;
		}
		i++;
	}
	if (i >= t.size())
	{
		error = "a block is not closed";
		return false;
	}
	i++;
	return true;
}

bool IsSamplerType(const std::string& s)
{
	return s == "sampler2D" || s == "usampler2D" || s == "isampler2D";
}

} // namespace

bool ReflectStage(const std::string& source, bool vertex, StageReflection& out, std::string& error)
{
	out = StageReflection();
	std::vector<Token> t;
	if (!Preprocess(source, vertex, t, error))
		return false;
	size_t i = 0;
	std::vector<Token> statement;
	std::string instance[2]; // the push constant block's and the uniform block's instance names
	// Function bodies, by name, for what main() reaches: glslang leaves out what it does not.
	std::vector<char> live(t.size(), 1);
	std::map<std::string, std::vector<std::pair<size_t, size_t>>, std::less<>> functions;
	while (i < t.size())
	{
		const std::string& s = t[i].text;
		if (s == "{")
		{
			const bool uniform = std::any_of(statement.begin(), statement.end(), [](const Token& x) { return x.text == "uniform"; });
			if (!uniform)
			{
				// A function's body, a struct: skipped whole, and not read until main() reaches it.
				const size_t start = i;
				int level = 0;
				for (; i < t.size(); i++)
				{
					if (t[i].text == "{")
						level++;
					else if (t[i].text == "}" && --level == 0)
						break;
				}
				const size_t stop = std::min(i + 1, t.size());
				std::fill(live.begin() + static_cast<std::ptrdiff_t>(start), live.begin() + static_cast<std::ptrdiff_t>(stop), 0);
				const auto paren = std::find_if(statement.begin(), statement.end(), [](const Token& x) { return x.text == "("; });
				const bool isStruct = std::any_of(statement.begin(), statement.end(), [](const Token& x) { return x.text == "struct"; });
				if (!isStruct && paren != statement.begin() && paren != statement.end() && (paren - 1)->kind == Token::Ident)
					functions[(paren - 1)->text].push_back({start, stop});
				i = stop;
				statement.clear();
				continue;
			}
			// layout(...) uniform Name { members } [instance [array]] ;
			std::map<std::string, int64_t> layout;
			for (size_t j = 0; j < statement.size(); j++)
				if (statement[j].text == "layout")
				{
					std::vector<Token> copy(statement.begin() + static_cast<std::ptrdiff_t>(j), statement.end());
					size_t k = 0;
					if (!ParseLayout(copy, k, layout, error))
						return false;
					j += k - 1;
				}
			const bool push = layout.count("push_constant") != 0;
			const bool std430 = layout.count("std430") ? true : layout.count("std140") ? false : push;
			StageReflection::Block& block = push ? out.push : out.ubo;
			if (block.present)
			{
				error = push ? "a stage has two push constant blocks" : "a stage has two uniform blocks (libretro passes have one)";
				return false;
			}
			block.present = true;
			block.push = push;
			block.set = layout.count("set") ? static_cast<int>(layout["set"]) : 0;
			block.binding = layout.count("binding") ? static_cast<int>(layout["binding"]) : -1;
			if (!push && (block.set != 0 || block.binding < 0 || block.binding >= MAX_BINDINGS))
			{
				error = "the uniform block is not at set 0 with a binding below 16";
				return false;
			}
			i++;
			uint32_t end = 0;
			if (!ParseMembers(t, i, std430, layout.count("row_major") != 0, block.members, end, error))
				return false;
			block.size = end;
			for (Member& m : block.members)
				m.push = push;
			if (i < t.size() && t[i].kind == Token::Ident)
				instance[push ? 0 : 1] = t[i].text;
			while (i < t.size() && t[i].text != ";")
				i++;
			i++;
			statement.clear();
			continue;
		}
		if (s == ";")
		{
			// layout(...) uniform sampler2D Name;
			bool uniform = false;
			std::map<std::string, int64_t> layout;
			std::string type, name;
			for (size_t j = 0; j < statement.size(); j++)
			{
				const std::string& w = statement[j].text;
				if (w == "layout")
				{
					std::vector<Token> copy(statement.begin() + static_cast<std::ptrdiff_t>(j), statement.end());
					size_t k = 0;
					if (!ParseLayout(copy, k, layout, error))
						return false;
					j += k - 1;
				}
				else if (w == "uniform")
					uniform = true;
				else if (uniform && type.empty() && statement[j].kind == Token::Ident && !IsQualifier(w))
					type = w;
				else if (uniform && !type.empty() && name.empty() && statement[j].kind == Token::Ident)
					name = w;
			}
			if (uniform)
			{
				if (!IsSamplerType(type))
				{
					error = "a uniform outside a block (" + name.substr(0, 40) + ", " + type.substr(0, 40) +
							") is not something a libretro pass has";
					return false;
				}
				const int set = layout.count("set") ? static_cast<int>(layout["set"]) : 0;
				const int64_t binding = layout.count("binding") ? layout["binding"] : -1;
				if (set != 0 || binding < 0 || binding >= MAX_BINDINGS || name.empty())
				{
					error = "the sampler " + name.substr(0, 40) + " is not at set 0 with a binding below 16";
					return false;
				}
				Sampler sm;
				sm.name = name;
				sm.binding = static_cast<uint32_t>(binding);
				(vertex ? sm.vertex : sm.fragment) = true;
				out.samplers.push_back(std::move(sm));
			}
			statement.clear();
			i++;
			continue;
		}
		if (s == "}")
		{
			i++;
			statement.clear();
			continue;
		}
		if (statement.size() > 4096)
		{
			error = "a declaration is too long";
			return false;
		}
		statement.push_back(t[i]);
		i++;
	}
	// What the code reads: a member of a block with an instance name as "instance.member" (a
	// local variable of the same name does not count, as shaders have), else a name seen again
	// past its declaration (over-counted when a local has the name: the check is only stricter).
	std::set<std::string> reached;
	std::vector<std::string> queue = {"main"};
	while (!queue.empty())
	{
		const std::string name = queue.back();
		queue.pop_back();
		const auto f = functions.find(name);
		if (f == functions.end() || !reached.insert(name).second)
			continue;
		for (const auto& [start, stop] : f->second)
			for (size_t k = start; k < stop; k++)
			{
				live[k] = 1;
				if (t[k].kind == Token::Ident && k + 1 < stop && t[k + 1].text == "(" && functions.count(t[k].text) &&
					!reached.count(t[k].text))
					queue.push_back(t[k].text);
			}
	}
	std::map<std::string, int, std::less<>> seen, dotted[2];
	for (size_t k = 0; k < t.size(); k++)
	{
		if (t[k].kind != Token::Ident || !live[k])
			continue;
		seen[t[k].text]++;
		if (k >= 2 && t[k - 1].text == "." && live[k - 2])
			for (int b = 0; b < 2; b++)
				if (!instance[b].empty() && t[k - 2].text == instance[b])
					dotted[b][t[k].text]++;
	}
	auto count = [](const std::map<std::string, int, std::less<>>& m, const std::string& n) {
		const auto it = m.find(n);
		return it == m.end() ? 0 : it->second;
	};
	for (int b = 0; b < 2; b++)
		for (Member& m : (b == 0 ? out.push : out.ubo).members)
			m.active = instance[b].empty() ? count(seen, m.name) > 1 : count(dotted[b], m.name) > 0;
	for (Sampler& s : out.samplers)
		s.active = count(seen, s.name) > 1;
	return true;
}

namespace
{
// ------------------------------------------------------------------------------------------------
// What each name means, once every pass is read (aliases name later passes too).

struct Names
{
	const Preset& preset;
	size_t pass;

	// An alias of a pass: its index (any pass), or -1.
	int Alias(std::string_view name) const
	{
		if (name.empty())
			return -1;
		for (size_t i = 0; i < preset.passes.size(); i++)
			if (preset.passes[i].alias == name)
				return static_cast<int>(i);
		return -1;
	}
	int LutNamed(std::string_view name) const
	{
		for (size_t i = 0; i < preset.luts.size(); i++)
			if (preset.luts[i].name == name)
				return static_cast<int>(i);
		return -1;
	}

	// A texture's name to what it is. `why` says what is wrong with a name it knows but refuses.
	bool Texture(std::string_view name, TextureRef& ref, std::string& why) const
	{
		uint32_t n = 0;
		int which = -1;
		if (name == "Original")
			ref = {TextureRef::History, 0};
		else if (name == "Source")
			ref = pass == 0 ? TextureRef{TextureRef::History, 0} : TextureRef{TextureRef::PassOutput, static_cast<uint32_t>(pass - 1)};
		else if (Numbered(name, "OriginalHistory", n))
		{
			if (n > 31)
			{
				why = "it asks for more than 31 frames of history";
				return false;
			}
			ref = {TextureRef::History, n};
		}
		else if (Numbered(name, "PassOutput", n))
		{
			if (n >= preset.passes.size())
			{
				why = "the preset has no such pass";
				return false;
			}
			// A pass not drawn yet this frame: RetroArch gives the game's picture.
			ref = n < pass ? TextureRef{TextureRef::PassOutput, n} : TextureRef{TextureRef::History, 0};
		}
		else if (Numbered(name, "PassFeedback", n))
		{
			if (n >= preset.passes.size())
			{
				why = "the preset has no such pass";
				return false;
			}
			ref = {TextureRef::PassFeedback, n};
		}
		else if ((which = Alias(name)) >= 0)
			ref = static_cast<size_t>(which) < pass ? TextureRef{TextureRef::PassOutput, static_cast<uint32_t>(which)}
													: TextureRef{TextureRef::History, 0};
		else if (EndsWith(name, "Feedback") && (which = Alias(name.substr(0, name.size() - 8))) >= 0)
			ref = {TextureRef::PassFeedback, static_cast<uint32_t>(which)};
		else if ((which = LutNamed(name)) >= 0)
			ref = {TextureRef::Lut, static_cast<uint32_t>(which)};
		else
		{
			why = "nothing in the preset makes it";
			return false;
		}
		return true;
	}

	// A size's name (the texture's name and "Size"; OriginalHistorySizeN, PassOutputSizeN and
	// PassFeedbackSizeN put the number last).
	bool Size(std::string_view name, TextureRef& ref, std::string& why) const
	{
		uint32_t n = 0;
		if (Numbered(name, "OriginalHistorySize", n))
			return Texture("OriginalHistory" + std::to_string(n), ref, why);
		if (Numbered(name, "PassOutputSize", n))
			return Texture("PassOutput" + std::to_string(n), ref, why);
		if (Numbered(name, "PassFeedbackSize", n))
			return Texture("PassFeedback" + std::to_string(n), ref, why);
		if (EndsWith(name, "Size") && name.size() > 4)
			return Texture(name.substr(0, name.size() - 4), ref, why);
		why = "it is not a name RetroArch gives a meaning";
		return false;
	}
};

bool Builtin(std::string_view name, Semantic& s)
{
	static const std::pair<const char*, Semantic> table[] = {
		{"MVP", Semantic::MVP},
		{"OutputSize", Semantic::OutputSize},
		{"FinalViewportSize", Semantic::FinalViewportSize},
		{"FrameCount", Semantic::FrameCount},
		{"FrameDirection", Semantic::FrameDirection},
		{"FrameTimeDelta", Semantic::FrameTimeDelta},
		{"OriginalFPS", Semantic::OriginalFPS},
		{"Rotation", Semantic::Rotation},
		{"OriginalAspect", Semantic::OriginalAspect},
		{"OriginalAspectRotated", Semantic::OriginalAspectRotated},
		{"TotalSubFrames", Semantic::TotalSubFrames},
		{"CurrentSubFrame", Semantic::CurrentSubFrame},
		{"HDRMode", Semantic::HDRMode},
		{"BrightnessNits", Semantic::BrightnessNits},
		{"Scanlines", Semantic::HDRScanlines},
		{"SubpixelLayout", Semantic::SubpixelLayout},
		{"ExpandGamut", Semantic::ExpandGamut},
		{"InverseTonemap", Semantic::InverseTonemap},
		{"HDR10", Semantic::HDR10},
		{"Gyroscope", Semantic::Sensor},
		{"Accelerometer", Semantic::Sensor},
		{"AccelerometerRest", Semantic::Sensor},
		{"SwapCount", Semantic::SwapCount},
	};
	for (const auto& [n, sem] : table)
		if (name == n)
		{
			s = sem;
			return true;
		}
	return false;
}

bool ResolveNames(Context& ctx, Preset& preset)
{
	uint32_t history = 0;
	for (size_t p = 0; p < preset.passes.size(); p++)
	{
		Pass& pass = preset.passes[p];
		const Names names{preset, p};
		const std::string where = Format("pass %zu (%s)", p, BaseName(pass.path).c_str());
		for (Member& m : pass.members)
		{
			std::string why;
			int param = -1;
			if (Builtin(m.name, m.semantic))
			{
			}
			else if ((param = preset.FindParameter(m.name)) >= 0)
			{
				m.semantic = Semantic::Parameter;
				m.parameter = static_cast<uint32_t>(param);
			}
			else if (names.Size(m.name, m.texture, why))
				m.semantic = Semantic::TextureSize;
			else if (!m.active)
			{
				// RetroArch only asks about what the code reads.
				m.semantic = Semantic::None;
				ctx.warnings.push_back(where + " declares " + m.name + ", which nothing fills; it is not read");
				continue;
			}
			else
				return ctx.Fail(where + " has a uniform " + m.name + " RetroArch would not fill: " + why);
			if (!m.active && m.semantic != Semantic::TextureSize)
				continue; // filled, but its type does not matter
			if (!m.active && m.semantic == Semantic::TextureSize && !(m.columns == 1 && m.rows == 4))
			{
				m.semantic = Semantic::None;
				continue;
			}
			const bool isVec4 = m.columns == 1 && m.rows == 4 && m.base == BaseType::Float;
			const bool isScalar = m.columns == 1 && m.rows == 1;
			const bool isVec3 = m.columns == 1 && m.rows == 3 && m.base == BaseType::Float;
			const bool ok = m.semantic == Semantic::MVP ? (m.columns == 4 && m.rows == 4)
							: (m.semantic == Semantic::OutputSize || m.semantic == Semantic::FinalViewportSize ||
								  m.semantic == Semantic::TextureSize)
								? isVec4
							: m.semantic == Semantic::Sensor ? isVec3
															 : isScalar && m.arrayLength == 0;
			if (!ok)
				return ctx.Fail(where + ": " + m.name + " is not of the type RetroArch gives it");
			if (m.semantic == Semantic::TextureSize && m.texture.kind == TextureRef::History)
				history = std::max(history, m.texture.index);
		}
		for (size_t k = 0; k < pass.samplers.size();)
		{
			Sampler& s = pass.samplers[k];
			std::string why;
			if (!names.Texture(s.name, s.texture, why))
			{
				if (s.active)
					return ctx.Fail(where + " reads " + s.name + ", which cannot be: " + why);
				// Declared, never read: no picture is bound to it.
				ctx.warnings.push_back(where + " declares the sampler " + s.name + " (" + why + "); it is not read");
				pass.samplers.erase(pass.samplers.begin() + static_cast<std::ptrdiff_t>(k));
				continue;
			}
			k++;
			if (s.texture.kind == TextureRef::PassFeedback)
				preset.passes[s.texture.index].feedback = true;
			if (s.texture.kind == TextureRef::History)
				history = std::max(history, s.texture.index);
		}
	}
	preset.historyFrames = history;
	return true;
}

// The two stages' reflections as one: a name both have must agree.
bool MergeStages(Context& ctx, const std::string& where, const StageReflection (&stages)[2], Pass& pass)
{
	std::map<std::string, const Member*> seen; // by block ("p" / "u") and name: a name may be in both
	for (const StageReflection& r : stages)
	{
		for (const StageReflection::Block* b : {&r.push, &r.ubo})
		{
			if (!b->present)
				continue;
			if (b->push)
				pass.pushSize = std::max(pass.pushSize, b->size);
			else
			{
				if (pass.uboBinding >= 0 && pass.uboBinding != b->binding)
					return ctx.Fail(where + ": the two stages put the uniform block at different bindings");
				pass.uboBinding = b->binding;
				pass.uboSize = std::max(pass.uboSize, RoundUp(b->size, 16));
			}
			for (const Member& m : b->members)
			{
				const std::string key = (m.push ? "p" : "u") + m.name;
				const auto it = seen.find(key);
				if (it != seen.end())
				{
					if (it->second->push != m.push || it->second->offset != m.offset || it->second->size != m.size)
						return ctx.Fail(where + ": " + m.name + " is not in the same place in both stages");
					for (Member& have : pass.members)
						if (have.name == m.name && have.push == m.push)
							have.active |= m.active;
					continue;
				}
				pass.members.push_back(m);
				seen[key] = &m;
			}
		}
		for (const Sampler& s : r.samplers)
		{
			auto it = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const Sampler& x) { return x.name == s.name; });
			if (it == pass.samplers.end())
				pass.samplers.push_back(s);
			else if (it->binding != s.binding)
				return ctx.Fail(where + ": the sampler " + s.name + " has different bindings in the two stages");
			else
			{
				it->vertex |= s.vertex;
				it->fragment |= s.fragment;
				it->active |= s.active;
			}
		}
	}
	if (pass.pushSize > MAX_PUSH_CONSTANT_BYTES)
		return ctx.Fail(Format("%s: its push constants are %u bytes, more than %zu", where.c_str(), pass.pushSize, MAX_PUSH_CONSTANT_BYTES));
	if (pass.uboSize > MAX_UBO_BYTES)
		return ctx.Fail(where + ": its uniform block is too large");
	std::set<uint32_t> bindings;
	if (pass.uboBinding >= 0)
		bindings.insert(static_cast<uint32_t>(pass.uboBinding));
	for (const Sampler& s : pass.samplers)
		if (!bindings.insert(s.binding).second)
			return ctx.Fail(Format("%s: binding %u is used twice", where.c_str(), s.binding));
	return true;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// The preset.

uint32_t VkFormatOf(std::string_view name)
{
	static const std::pair<const char*, uint32_t> formats[] = {
		{"R8_UNORM", 9}, {"R8_UINT", 13}, {"R8_SINT", 14},
		{"R8G8_UNORM", 16}, {"R8G8_UINT", 20}, {"R8G8_SINT", 21},
		{"R8G8B8A8_UNORM", 37}, {"R8G8B8A8_UINT", 41}, {"R8G8B8A8_SINT", 42}, {"R8G8B8A8_SRGB", 43},
		{"A2B10G10R10_UNORM_PACK32", 64}, {"A2B10G10R10_UINT_PACK32", 68},
		{"R16_UINT", 74}, {"R16_SINT", 75}, {"R16_SFLOAT", 76},
		{"R16G16_UINT", 81}, {"R16G16_SINT", 82}, {"R16G16_SFLOAT", 83},
		{"R16G16B16A16_UINT", 95}, {"R16G16B16A16_SINT", 96}, {"R16G16B16A16_SFLOAT", 97},
		{"R32_UINT", 98}, {"R32_SINT", 99}, {"R32_SFLOAT", 100},
		{"R32G32_UINT", 101}, {"R32G32_SINT", 102}, {"R32G32_SFLOAT", 103},
		{"R32G32B32A32_UINT", 107}, {"R32G32B32A32_SINT", 108}, {"R32G32B32A32_SFLOAT", 109},
	};
	for (const auto& [n, v] : formats)
		if (name == n)
			return v;
	return 0;
}

int Preset::FindParameter(std::string_view name) const
{
	for (size_t i = 0; i < parameters.size(); i++)
		if (parameters[i].name == name)
			return static_cast<int>(i);
	return -1;
}

bool Preset::SetParameter(std::string_view name, float value)
{
	const int i = FindParameter(name);
	if (i < 0 || !std::isfinite(value))
		return false;
	Parameter& p = parameters[static_cast<size_t>(i)];
	p.value = std::clamp(value, std::min(p.minimum, p.maximum), std::max(p.minimum, p.maximum));
	return true;
}

void Preset::ResetParameters()
{
	for (Parameter& p : parameters)
		p.value = p.presetValue;
}

bool LoadPreset(const std::string& rawPath, const LoadOptions& options, Preset& out, std::string& error)
{
	out = Preset();
	error.clear();
	Context ctx{options, {}, {}, {}};
	std::string path(rawPath);
	for (char& c : path)
		if (c == '\\')
			c = '/';
	path = Normalise(path);
	for (const std::string& root : options.allowedRoots)
	{
		std::string r(root);
		for (char& c : r)
			if (c == '\\')
				c = '/';
		ctx.roots.push_back(Normalise(r));
	}
	if (ctx.roots.empty())
		ctx.roots.push_back(DirectoryOf(path));
	if (std::none_of(ctx.roots.begin(), ctx.roots.end(), [&](const std::string& r) { return Inside(r, path); }))
	{
		error = BaseName(path) + " is outside the shader folders";
		return false;
	}
	out.path = path;

	Settings settings;
	if (!ReadSettings(ctx, path, settings, 0))
	{
		error = ctx.error;
		return false;
	}
	double count = 0;
	if (!ParseNumber(Get(settings, "shaders"), count) || count < 1 || count != std::floor(count))
	{
		error = BaseName(path) + " names no passes (shaders = N)";
		return false;
	}
	if (count > MAX_PASSES)
	{
		error = Format("%s has %d passes, more than %d", BaseName(path).c_str(), static_cast<int>(std::min(count, 1e6)), MAX_PASSES);
		return false;
	}
	const size_t passes = static_cast<size_t>(count);

	// The lookup pictures, first: a pass may name one.
	for (const std::string& name : SplitList(Get(settings, "textures")))
	{
		if (out.luts.size() >= MAX_LUTS)
		{
			error = Format("%s has more than %d lookup pictures", BaseName(path).c_str(), MAX_LUTS);
			return false;
		}
		const Value* file = Find(settings, name);
		if (!file)
		{
			error = "the preset names a lookup picture (" + name + ") without its file";
			return false;
		}
		Lut lut;
		lut.name = name;
		if (!Resolve(ctx, file->file, file->text, lut.path))
		{
			error = ctx.error;
			return false;
		}
		lut.linear = ParseBool(Get(settings, name + "_linear", "false"));
		lut.mipmap = ParseBool(Get(settings, name + "_mipmap", "false"));
		if (!WrapNamed(Get(settings, name + "_wrap_mode"), lut.wrap))
			ctx.warnings.push_back("the preset gives " + name + " a wrap mode that is not known: clamp_to_border instead");
		std::string data;
		if (!ReadFile(ctx, lut.path, options.maxLutBytes, data))
		{
			error = ctx.error;
			return false;
		}
		if (options.readLuts)
			lut.data = std::move(data);
		out.luts.push_back(std::move(lut));
	}

	std::map<std::string, size_t> parameterIndex;
	for (size_t n = 0; n < passes; n++)
	{
		const Value* shader = Find(settings, Format("shader%zu", n));
		if (!shader)
		{
			error = Format("the preset has no shader%zu", n);
			return false;
		}
		Pass pass;
		if (!Resolve(ctx, shader->file, shader->text, pass.path))
		{
			error = ctx.error;
			return false;
		}
		const std::string where = Format("pass %zu (%s)", n, BaseName(pass.path).c_str());
		Source source;
		if (!LoadSource(ctx, pass.path, source))
		{
			error = ctx.error;
			return false;
		}
		for (Source::Declared& d : source.parameters)
		{
			const auto it = parameterIndex.find(d.name);
			if (it != parameterIndex.end())
			{
				const Parameter& p = out.parameters[it->second];
				if (p.initial != d.initial || p.minimum != d.minimum || p.maximum != d.maximum)
					ctx.warnings.push_back(where + " declares " + d.name + " again with other values: the first is kept");
				continue;
			}
			if (out.parameters.size() >= MAX_PARAMETERS)
			{
				error = "the preset has too many parameters";
				return false;
			}
			Parameter p;
			p.name = d.name;
			p.description = d.description;
			p.initial = d.initial;
			p.minimum = d.minimum;
			p.maximum = d.maximum;
			p.step = d.step;
			p.presetValue = p.value = d.initial;
			parameterIndex[d.name] = out.parameters.size();
			out.parameters.push_back(std::move(p));
		}

		// The pass's settings.
		const std::string i = std::to_string(n);
		pass.filterLinear = ParseBool(Get(settings, "filter_linear" + i, "false"));
		if (!WrapNamed(Get(settings, "wrap_mode" + i), pass.wrap))
			ctx.warnings.push_back(where + ": its wrap mode is not known: clamp_to_border instead (as RetroArch)");
		pass.mipmapInput = ParseBool(Get(settings, "mipmap_input" + i, "false"));
		double mod = 0;
		if (Find(settings, "frame_count_mod" + i) &&
			(!ParseNumber(Get(settings, "frame_count_mod" + i), mod) || mod < 0 || mod > 4294967295.0))
		{
			error = where + ": its frame_count_mod is not a number";
			return false;
		}
		pass.frameCountMod = static_cast<uint32_t>(mod);
		pass.alias = Get(settings, "alias" + i);
		if (pass.alias.empty())
			pass.alias = source.name;
		for (const char c : pass.alias)
			if (!IsIdentChar(c))
			{
				error = where + ": its alias " + pass.alias.substr(0, 40) + " is not a name a shader can use";
				return false;
			}
		// Scale: scale_typeN for both axes, scale_type_xN / _yN for one; scaleN, scale_xN, scale_yN.
		const Value* both = Find(settings, "scale_type" + i);
		const Value* typeX = Find(settings, "scale_type_x" + i);
		const Value* typeY = Find(settings, "scale_type_y" + i);
		pass.scaleSet = both || typeX || typeY;
		bool ok = true;
		if (both)
		{
			ok = ok && ScaleNamed(both->text, pass.scaleTypeX);
			pass.scaleTypeY = pass.scaleTypeX;
		}
		if (typeX)
			ok = ok && ScaleNamed(typeX->text, pass.scaleTypeX);
		if (typeY)
			ok = ok && ScaleNamed(typeY->text, pass.scaleTypeY);
		if (!ok)
		{
			error = where + ": its scale type is not source, viewport or absolute";
			return false;
		}
		// scaleN for both axes; scale_xN / scale_yN only where there is no scaleN (as RetroArch
		// reads them). An absolute size is a whole number of pixels.
		for (int axis = 0; axis < 2; axis++)
		{
			const Value* v = Find(settings, "scale" + i);
			if (!v)
				v = Find(settings, std::string(axis == 0 ? "scale_x" : "scale_y") + i);
			float f = 1.f;
			if (!v)
				continue;
			if (!ParseFloat(v->text, f) || !(f > 0.f) || f > 16384.f)
			{
				error = where + ": its scale is not a number above 0";
				return false;
			}
			if ((axis == 0 ? pass.scaleTypeX : pass.scaleTypeY) == ScaleType::Absolute)
				f = std::max(1.f, std::round(f));
			(axis == 0 ? pass.scaleX : pass.scaleY) = f;
		}
		if (!pass.scaleSet)
		{
			// The last pass fills the output; any other is the size of what it reads.
			const bool last = n + 1 == passes;
			pass.scaleTypeX = pass.scaleTypeY = last ? ScaleType::Viewport : ScaleType::Source;
			pass.scaleX = pass.scaleY = 1.f;
		}
		// The picture's format: the shader's #pragma format, unless the preset asks for an sRGB,
		// float or 10-bit picture (in that order, as RetroArch looks), which it only does for a
		// pass with a scale.
		if (!source.format.empty())
			pass.formatName = source.format;
		if (pass.scaleSet && ParseBool(Get(settings, "srgb_framebuffer" + i, "false")))
			pass.formatName = "R8G8B8A8_SRGB";
		else if (pass.scaleSet && ParseBool(Get(settings, "float_framebuffer" + i, "false")))
			pass.formatName = "R16G16B16A16_SFLOAT";
		else if (pass.scaleSet && ParseBool(Get(settings, "rgb10_framebuffer" + i, "false")))
			pass.formatName = "A2B10G10R10_UNORM_PACK32";
		pass.vkFormat = VkFormatOf(pass.formatName);
		if (pass.vkFormat == 0)
		{
			error = where + " asks for a format that is not known (" + pass.formatName.substr(0, 40) + ")";
			return false;
		}

		// The reflection of both stages.
		StageReflection stages[2];
		for (int s = 0; s < 2; s++)
		{
			std::string why;
			if (!ReflectStage(s == 0 ? source.vertex : source.fragment, s == 0, stages[s], why))
			{
				error = where + Format(", %s stage: ", s == 0 ? "vertex" : "fragment") + why;
				return false;
			}
		}
		if (!MergeStages(ctx, where, stages, pass))
		{
			error = ctx.error;
			return false;
		}
		pass.vertex = std::move(source.vertex);
		pass.fragment = std::move(source.fragment);
		out.passes.push_back(std::move(pass));
	}

	// Aliases are names: two passes may not share one, nor a pass and a lookup picture.
	for (size_t a = 0; a < out.passes.size(); a++)
	{
		if (out.passes[a].alias.empty())
			continue;
		for (size_t b = a + 1; b < out.passes.size(); b++)
			if (out.passes[a].alias == out.passes[b].alias)
			{
				error = "two passes have the alias " + out.passes[a].alias;
				return false;
			}
		for (const Lut& l : out.luts)
			if (l.name == out.passes[a].alias)
			{
				error = "a pass and a lookup picture are both named " + l.name;
				return false;
			}
	}

	// The preset's own values for the parameters: those it lists in "parameters", and (as newer
	// RetroArch writes them, after a #reference) any other key named as a parameter.
	for (Parameter& p : out.parameters)
	{
		const Value* v = Find(settings, p.name);
		float f = 0.f;
		if (v && ParseFloat(v->text, f))
			p.presetValue = p.value = std::clamp(f, std::min(p.minimum, p.maximum), std::max(p.minimum, p.maximum));
	}

	if (!ResolveNames(ctx, out))
	{
		error = ctx.error;
		return false;
	}
	out.warnings = std::move(ctx.warnings);
	return true;
}

std::vector<std::string> FindPresets(const std::string& folder, int maxDepth)
{
	std::vector<std::string> out, folders;
	if (maxDepth < 0)
		return out;
	DIR* dir = opendir(folder.c_str());
	if (!dir)
		return out;
	while (const dirent* entry = readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name.empty() || name[0] == '.')
			continue;
		const std::string full = folder + "/" + name;
		struct stat st;
		if (stat(full.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			folders.push_back(full);
		else if (S_ISREG(st.st_mode) && EndsWith(Lower(name), ".slangp"))
			out.push_back(full);
		if (out.size() + folders.size() > 10000)
			break;
	}
	closedir(dir);
	for (const std::string& inner : folders)
	{
		std::vector<std::string> more = FindPresets(inner, maxDepth - 1);
		out.insert(out.end(), more.begin(), more.end());
	}
	std::sort(out.begin(), out.end());
	return out;
}

// ------------------------------------------------------------------------------------------------
// The plan of a frame.

namespace
{
uint32_t Scaled(ScaleType type, float factor, uint32_t source, uint32_t viewport)
{
	const double base = type == ScaleType::Source ? source : type == ScaleType::Viewport ? viewport : 1.0;
	const double v = std::round(base * static_cast<double>(factor));
	return static_cast<uint32_t>(std::clamp(v, 1.0, 16384.0));
}

uint32_t MipLevels(uint32_t w, uint32_t h)
{
	uint32_t levels = 1;
	for (uint32_t s = std::max(w, h); s > 1; s >>= 1)
		levels++;
	return levels;
}

void PutFloats(uint8_t* at, const float* v, size_t n)
{
	std::memcpy(at, v, n * sizeof(float));
}

// One value into a member of whatever scalar type it was declared.
void PutScalar(uint8_t* at, const Member& m, double v)
{
	switch (m.base)
	{
		case BaseType::Float:
		{
			const float f = static_cast<float>(v);
			std::memcpy(at, &f, 4);
			break;
		}
		case BaseType::Int:
		{
			const int32_t x = static_cast<int32_t>(std::clamp(v, -2147483648.0, 2147483647.0));
			std::memcpy(at, &x, 4);
			break;
		}
		case BaseType::Uint:
		{
			const uint32_t x = static_cast<uint32_t>(std::clamp(v, 0.0, 4294967295.0));
			std::memcpy(at, &x, 4);
			break;
		}
		case BaseType::Bool:
		{
			const uint32_t x = v != 0.0 ? 1u : 0u;
			std::memcpy(at, &x, 4);
			break;
		}
	}
}
} // namespace

bool ChainPlan::Build(const Preset& preset, const FrameInfo& frame, std::string* error)
{
	const size_t count = preset.passes.size();
	if (count == 0 || frame.sourceWidth == 0 || frame.sourceHeight == 0 || frame.viewportWidth == 0 || frame.viewportHeight == 0)
	{
		if (error)
			*error = "the chain has no passes, or a size is zero";
		return false;
	}
	passes.resize(count);
	historyFrames = preset.historyFrames;

	// Sizes, from the game's picture on.
	for (size_t p = 0; p < count; p++)
	{
		const Pass& pass = preset.passes[p];
		const uint32_t sw = p == 0 ? frame.sourceWidth : passes[p - 1].width;
		const uint32_t sh = p == 0 ? frame.sourceHeight : passes[p - 1].height;
		PassPlan& plan = passes[p];
		plan.width = Scaled(pass.scaleTypeX, pass.scaleX, sw, frame.viewportWidth);
		plan.height = Scaled(pass.scaleTypeY, pass.scaleY, sh, frame.viewportHeight);
		plan.vkFormat = pass.vkFormat;
		plan.final = p + 1 == count;
	}
	// Mipmaps: a picture has them when the pass after it asks for its input with them.
	originalMipLevels = preset.passes[0].mipmapInput ? MipLevels(frame.sourceWidth, frame.sourceHeight) : 1;
	for (size_t p = 0; p < count; p++)
		passes[p].mipLevels = p + 1 < count && preset.passes[p + 1].mipmapInput ? MipLevels(passes[p].width, passes[p].height) : 1;

	auto sizeOf = [&](const TextureRef& ref, uint32_t& w, uint32_t& h) {
		w = h = 0;
		switch (ref.kind)
		{
			case TextureRef::History:
				w = frame.sourceWidth;
				h = frame.sourceHeight;
				if (ref.index > 0 && ref.index - 1 < frame.historyWidths.size() && ref.index - 1 < frame.historyHeights.size() &&
					frame.historyWidths[ref.index - 1] > 0 && frame.historyHeights[ref.index - 1] > 0)
				{
					w = frame.historyWidths[ref.index - 1];
					h = frame.historyHeights[ref.index - 1];
				}
				break;
			case TextureRef::PassOutput:
			case TextureRef::PassFeedback:
				// A feedback picture is kept at the pass's size (the runner makes it anew, cleared,
				// when the size changes).
				if (ref.index < count)
				{
					w = passes[ref.index].width;
					h = passes[ref.index].height;
				}
				break;
			case TextureRef::Lut:
				if (ref.index < preset.luts.size())
				{
					w = preset.luts[ref.index].width;
					h = preset.luts[ref.index].height;
				}
				break;
		}
	};
	auto size4 = [](uint32_t w, uint32_t h, float v[4]) {
		v[0] = static_cast<float>(w);
		v[1] = static_cast<float>(h);
		v[2] = w ? 1.f / static_cast<float>(w) : 0.f;
		v[3] = h ? 1.f / static_cast<float>(h) : 0.f;
	};
	const float aspect = frame.originalAspect > 0.f ? frame.originalAspect
													 : static_cast<float>(frame.sourceWidth) / static_cast<float>(frame.sourceHeight);
	const double fps = frame.originalFps > 1.f ? frame.originalFps : 60.0;

	for (size_t p = 0; p < count; p++)
	{
		const Pass& pass = preset.passes[p];
		PassPlan& plan = passes[p];
		plan.ubo.assign(pass.uboSize, 0);
		plan.push.assign(pass.pushSize, 0);
		for (const Member& m : pass.members)
		{
			std::vector<uint8_t>& buffer = m.push ? plan.push : plan.ubo;
			const uint32_t one = m.columns == 1 ? 4u * m.rows : m.size;
			if (static_cast<size_t>(m.offset) + std::min(one, m.size) > buffer.size())
				continue;
			uint8_t* at = buffer.data() + m.offset;
			float v[4] = {};
			switch (m.semantic)
			{
				case Semantic::MVP:
					// Column c of the matrix at c * matrixStride; row major turns it over.
					for (int c = 0; c < 4; c++)
						for (int r = 0; r < 4; r++)
						{
							const float x = MVP_MATRIX[c * 4 + r];
							const size_t o = m.rowMajor ? static_cast<size_t>(r) * m.matrixStride + static_cast<size_t>(c) * 4
														: static_cast<size_t>(c) * m.matrixStride + static_cast<size_t>(r) * 4;
							if (m.offset + o + 4 <= buffer.size())
								std::memcpy(at + o, &x, 4);
						}
					continue;
				case Semantic::OutputSize:
					size4(plan.width, plan.height, v);
					PutFloats(at, v, 4);
					continue;
				case Semantic::FinalViewportSize:
					size4(frame.viewportWidth, frame.viewportHeight, v);
					PutFloats(at, v, 4);
					continue;
				case Semantic::TextureSize:
				{
					uint32_t w = 0, h = 0;
					sizeOf(m.texture, w, h);
					size4(w, h, v);
					PutFloats(at, v, 4);
					continue;
				}
				case Semantic::FrameCount:
				{
					const uint64_t f = pass.frameCountMod > 0 ? frame.frameCount % pass.frameCountMod : frame.frameCount;
					// Wraps at 2^32 as RetroArch's uint does; a float FrameCount gets the same number.
					PutScalar(at, m, static_cast<double>(static_cast<uint32_t>(f)));
					continue;
				}
				case Semantic::FrameDirection: PutScalar(at, m, frame.frameDirection); continue;
				case Semantic::FrameTimeDelta: PutScalar(at, m, std::round(1e6 / fps)); continue;
				case Semantic::OriginalFPS: PutScalar(at, m, fps); continue;
				case Semantic::Rotation: PutScalar(at, m, frame.rotation); continue;
				case Semantic::OriginalAspect: PutScalar(at, m, aspect); continue;
				case Semantic::OriginalAspectRotated:
					PutScalar(at, m, (frame.rotation & 1) ? 1.0 / aspect : aspect);
					continue;
				case Semantic::TotalSubFrames:
				case Semantic::CurrentSubFrame: PutScalar(at, m, 1.0); continue;
				case Semantic::SwapCount: PutScalar(at, m, static_cast<double>(static_cast<uint32_t>(frame.frameCount))); continue;
				case Semantic::BrightnessNits: PutScalar(at, m, frame.paperWhiteNits); continue;
				case Semantic::HDRMode:
				case Semantic::HDRScanlines:
				case Semantic::SubpixelLayout:
				case Semantic::ExpandGamut:
				case Semantic::InverseTonemap:
				case Semantic::HDR10:
				case Semantic::Sensor:
					continue; // 0: the picture goes out as SDR, and there are no sensors
				case Semantic::Parameter:
					if (m.parameter < preset.parameters.size())
						PutScalar(at, m, preset.parameters[m.parameter].value);
					continue;
				case Semantic::None:
					continue;
			}
		}

		// The pictures, each sampled as the pass after its maker asks (the game's picture as
		// pass 0 asks, a lookup picture as the preset says).
		plan.textures.resize(pass.samplers.size());
		for (size_t s = 0; s < pass.samplers.size(); s++)
		{
			const Sampler& sampler = pass.samplers[s];
			TextureBind& bind = plan.textures[s];
			bind.binding = sampler.binding;
			bind.texture = sampler.texture;
			switch (sampler.texture.kind)
			{
				case TextureRef::History:
					bind.linear = preset.passes[0].filterLinear;
					bind.wrap = preset.passes[0].wrap;
					bind.mipmap = sampler.texture.index == 0 && originalMipLevels > 1;
					break;
				case TextureRef::PassOutput:
				case TextureRef::PassFeedback:
				{
					// A pass's picture of this frame as the pass after it reads it; its picture
					// of the frame before as the pass itself reads its input (RetroArch's
					// shader_vulkan.c does both so).
					const size_t maker = sampler.texture.index;
					const Pass& taker = sampler.texture.kind == TextureRef::PassOutput ? preset.passes[std::min(maker + 1, count - 1)]
																					   : preset.passes[std::min(maker, count - 1)];
					bind.linear = taker.filterLinear;
					bind.wrap = taker.wrap;
					bind.mipmap = maker < count && passes[maker].mipLevels > 1;
					break;
				}
				case TextureRef::Lut:
				{
					const Lut& lut = preset.luts[sampler.texture.index];
					bind.linear = lut.linear;
					bind.wrap = lut.wrap;
					bind.mipmap = lut.mipmap;
					break;
				}
			}
		}
	}
	return true;
}
} // namespace PS5Slang

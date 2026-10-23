// Crux2 - the HD texture packs' names and unpacking, on a PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_texture_packs_extract.h"
#include "ps5_texture_packs_logic.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0, g_failed = 0;
void Expect(bool ok, const std::string& what)
{
	g_checks++;
	if (!ok)
	{
		g_failed++;
		std::fprintf(stderr, "FAILED: %s\n", what.c_str());
	}
}

std::vector<char> ReadAll(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

// The archive handed over in pieces of an odd size, as a download does.
PS5TexturePacksExtract::Read Stream(const std::vector<char>& data, size_t piece, size_t* at)
{
	return [&data, piece, at](void* buffer, size_t size) -> long {
		const size_t n = std::min({piece, size, data.size() - *at});
		std::memcpy(buffer, data.data() + *at, n);
		*at += n;
		return static_cast<long>(n);
	};
}

size_t CountFiles(const std::string& folder, bool* partial)
{
	size_t n = 0;
	for (const auto& e : std::filesystem::recursive_directory_iterator(folder))
	{
		if (e.is_regular_file())
		{
			n++;
			if (e.path().extension() == ".partial")
				*partial = true;
		}
	}
	return n;
}
} // namespace

int main(int argc, char** argv)
{
	using namespace PS5TexturePacksLogic;
	if (argc != 3)
		return 2;
	const std::string archives = argv[1], out = argv[2];

	// Names.
	const auto one = SerialsIn("Black (USA) [SLUS-21376] HD Remaster.rar");
	Expect(one.size() == 1 && one[0] == "SLUS-21376", "a serial in a pack's name");
	const auto two = SerialsIn("Game (USA) (Disc 1, 2) [SLUS-20946, SLUS-20973] HD Remaster Definitive Edition.rar");
	Expect(two.size() == 2 && two[1] == "SLUS-20973", "two serials in a pack's name");
	Expect(SerialsIn("SLUS_213.76").size() == 1 && SerialsIn("SLUS_213.76")[0] == "SLUS-21376", "a disc's own file name");
	Expect(SerialsIn("XSLUS-213761 no").empty() && SerialsIn("ABCD-1234").empty(), "not a serial");
	Expect(IsPack("Black (USA) [SLUS-21376] HD Remaster.rar"), "a pack");
	Expect(!IsPack("PlayStation 2 BIOS (USA) [SCPH-39001] HD Remaster.rar"), "the BIOS is never a pack");
	Expect(!IsPack("Cover.png") && !IsPack("__ia_thumb.jpg"), "the collection's pictures are not packs");
	Expect(TitleOf("Black (USA) [SLUS-21376] HD Remaster.rar") == "Black (USA) [SLUS-21376] HD Remaster", "a pack's title");
	Expect(EncodeForURL("Black (USA) [SLUS-21376].rar") == "Black%20%28USA%29%20%5BSLUS-21376%5D.rar", "a name in an address");
	Expect(SizeText(1932735283ull) == "1.8 GB" && SizeText(50ull << 20) == "50 MB", "sizes");

	// Where each file goes.
	const std::vector<std::string> s = {"SLUS-21376"};
	Expect(EntryTarget("SLUS-21376/replacements/a.png", s) == "SLUS-21376/replacements/a.png", "a pack's own layout");
	Expect(EntryTarget("Black HD/slus-21376/replacements/a.png", s) == "SLUS-21376/replacements/a.png", "under a folder of its own");
	Expect(EntryTarget("replacements/a.png", s) == "SLUS-21376/replacements/a.png", "no serial folder");
	Expect(EntryTarget("SLUS-21376/", s).empty() && EntryTarget("SLUS-21376/../../etc/passwd", s).empty(), "folders and climbing out");
	Expect(EntryTarget("/etc/passwd", s).empty() && EntryTarget("C:\\x.png", s).empty(), "absolute paths");
	Expect(EntryTarget("__MACOSX/SLUS-21376/._a.png", s).empty(), "a Mac's resource forks");
	Expect(EntryTarget("Pack\\SLUS-21376\\replacements\\b.png", s) == "SLUS-21376/replacements/b.png", "backslashes");

	// Unpacking libarchive's own RAR and RAR5 archives as a stream.
	std::filesystem::remove_all(out);
	int n = 0;
	for (const char* name : {"test_read_format_rar.rar", "test_read_format_rar_compress_normal.rar", "test_read_format_rar_multi_lzss_blocks.rar",
			 "test_read_format_rar5_multiple_files_solid.rar", "test_read_format_rar5_stored_manyfiles.rar", "test_read_format_rar5_compressed.rar"})
	{
		const std::vector<char> data = ReadAll(archives + "/" + name);
		Expect(!data.empty(), std::string("read ") + name);
		const std::string root = out + "/" + std::to_string(n++);
		size_t at = 0;
		const auto result = PS5TexturePacksExtract::Extract(Stream(data, 4093, &at), root, s, [] { return false; });
		Expect(result.ok && result.files > 0, std::string("unpacked as a stream: ") + name + " (" + result.error + ")");
		bool partial = false;
		Expect(CountFiles(root, &partial) == static_cast<size_t>(result.files) && !partial, std::string("every file in place: ") + name);
		Expect(std::filesystem::is_directory(root + "/SLUS-21376"), std::string("under the serial's folder: ") + name);
	}
	{
		// Stopped half way: no half file under its name.
		const std::vector<char> data = ReadAll(archives + "/test_read_format_rar5_stored_manyfiles.rar");
		size_t at = 0;
		int asked = 0;
		const auto result = PS5TexturePacksExtract::Extract(Stream(data, 512, &at), out + "/stopped", s, [&asked] { return ++asked > 3; });
		bool partial = false;
		Expect(!result.ok && result.error == "stopped", "a stop is said");
		if (std::filesystem::exists(out + "/stopped"))
			CountFiles(out + "/stopped", &partial);
		Expect(!partial, "a stop leaves no partial file");
	}
	{
		// What is not an archive.
		const std::vector<char> data(100000, 'x');
		size_t at = 0;
		const auto result = PS5TexturePacksExtract::Extract(Stream(data, 1000, &at), out + "/bad", s, [] { return false; });
		Expect(!result.ok && !result.error.empty(), "not an archive: refused with a reason");
	}
	{
		// The download failing half way.
		const std::vector<char> data = ReadAll(archives + "/test_read_format_rar5_compressed.rar");
		size_t at = 0;
		PS5TexturePacksExtract::Read failing = [&](void* buffer, size_t size) -> long {
			if (at > data.size() / 2)
				return -1;
			const size_t k = std::min<size_t>({300, size, data.size() - at});
			std::memcpy(buffer, data.data() + at, k);
			at += k;
			return static_cast<long>(k);
		};
		const auto result = PS5TexturePacksExtract::Extract(failing, out + "/failing", s, [] { return false; });
		Expect(!result.ok, "a download that fails half way is not taken for a whole pack");
	}

	if (g_failed)
	{
		std::fprintf(stderr, "texturepacks: %d of %d checks FAILED\n", g_failed, g_checks);
		return 1;
	}
	std::printf("texturepacks: %d checks passed\n", g_checks);
	return 0;
}

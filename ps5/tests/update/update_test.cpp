// Crux2 - the updater (frontend/ps5_update.cpp), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   update_test <work folder>
//
// A folder of the work folder plays the title folder, with an older version's files and a user's
// files in it. GitHub is played by the test: its releases page is JSON written here, its ZIPs are
// made here with libzip, and both are served through the same HttpGet and HttpDownload the title
// gives the updater. Each step is held against what must be on the disk afterwards: the program's
// files replaced, the user's never touched, and after a failure or an interrupted install the
// folder exactly as it was.

#include "ps5_update.h"

#include <zip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace PS5Update;

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static bool Contains(const std::string& text, const char* part)
{
	return text.find(part) != std::string::npos;
}

// ------------------------------------------------------------------------------------- files

static void Run(const std::string& command)
{
	if (std::system(command.c_str()) != 0)
		std::fprintf(stderr, "command failed: %s\n", command.c_str());
}

static void MakeDirs(const std::string& path)
{
	for (size_t at = 1; at <= path.size(); at++)
		if (at == path.size() || path[at] == '/')
			mkdir(path.substr(0, at).c_str(), 0777);
}

static void Put(const std::string& path, const std::string& data)
{
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos)
		MakeDirs(path.substr(0, slash));
	FILE* file = std::fopen(path.c_str(), "wb");
	if (!file)
	{
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
		std::exit(2);
	}
	std::fwrite(data.data(), 1, data.size(), file);
	std::fclose(file);
}

static std::string Get(const std::string& path)
{
	std::string data;
	FILE* file = std::fopen(path.c_str(), "rb");
	if (!file)
		return "<missing>";
	char buffer[65536];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		data.append(buffer, got);
	std::fclose(file);
	return data;
}

static bool Exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}

// Every file and folder below `dir`, with each file's bytes ("/" for a folder). .update/ left out
// when asked.
static void Walk(const std::string& dir, const std::string& relative, std::map<std::string, std::string>& out,
	bool skip_update)
{
	DIR* list = opendir((dir + relative).c_str());
	if (!list)
		return;
	std::vector<std::string> names;
	while (const dirent* entry = readdir(list))
		if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
			names.push_back(entry->d_name);
	closedir(list);
	for (const std::string& name : names)
	{
		const std::string path = relative + name;
		if (skip_update && path == ".update")
			continue;
		struct stat st;
		if (lstat((dir + path).c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			out[path + "/"] = "/";
			Walk(dir, path + "/", out, skip_update);
		}
		else
			out[path] = Get(dir + path);
	}
}

static std::map<std::string, std::string> Snapshot(const std::string& dir, bool skip_update = false)
{
	std::map<std::string, std::string> out;
	Walk(dir, "", out, skip_update);
	return out;
}

static std::string Filler(const std::string& seed, size_t size)
{
	std::string out;
	unsigned value = 2166136261u;
	for (char c : seed)
		value = (value ^ static_cast<unsigned char>(c)) * 16777619u;
	while (out.size() < size)
	{
		value = value * 1103515245u + 12345u;
		out += static_cast<char>('a' + (value >> 16) % 26);
	}
	return out;
}

// ---------------------------------------------------------------------------------- the ZIPs

struct Item
{
	std::string name;
	std::string data;
	bool folder = false;
	uint32_t mode = 0100777; // what make-release-zip.py writes; 0: no Unix mode at all
};

// A ZIP of these entries. Names that libzip would not write as they are (".." and the like) are
// given as a stand-in of the same length and patched into the bytes afterwards: `patches` maps
// stand-in to the real name.
static std::string MakeZip(const std::string& work, const std::vector<Item>& items,
	const std::vector<std::pair<std::string, std::string>>& patches = {})
{
	const std::string path = work + "/made.zip";
	unlink(path.c_str());
	int error = 0;
	zip_t* zip = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
	if (!zip)
	{
		std::fprintf(stderr, "zip_open: %d\n", error);
		std::exit(2);
	}
	for (const Item& item : items)
	{
		zip_int64_t index;
		if (item.folder)
		{
			std::string name = item.name;
			if (!name.empty() && name.back() == '/')
				name.pop_back();
			index = zip_dir_add(zip, name.c_str(), ZIP_FL_ENC_UTF_8);
		}
		else
		{
			zip_source_t* source = zip_source_buffer(zip, item.data.data(), item.data.size(), 0);
			index = zip_file_add(zip, item.name.c_str(), source, ZIP_FL_ENC_UTF_8);
			if (index < 0)
				zip_source_free(source);
		}
		if (index < 0)
		{
			std::fprintf(stderr, "zip: cannot add %s: %s\n", item.name.c_str(), zip_strerror(zip));
			std::exit(2);
		}
		if (item.mode != 0)
			zip_file_set_external_attributes(zip, static_cast<zip_uint64_t>(index), 0, ZIP_OPSYS_UNIX,
				(item.folder ? 040777u : item.mode) << 16);
		else
			zip_file_set_external_attributes(zip, static_cast<zip_uint64_t>(index), 0, ZIP_OPSYS_DOS, 0);
	}
	if (zip_close(zip) != 0)
	{
		std::fprintf(stderr, "zip_close: %s\n", zip_strerror(zip));
		std::exit(2);
	}
	std::string bytes = Get(path);
	for (const auto& [from, to] : patches)
	{
		if (from.size() != to.size())
			std::exit(3);
		for (size_t at = bytes.find(from); at != std::string::npos; at = bytes.find(from, at + 1))
			bytes.replace(at, from.size(), to);
	}
	return bytes;
}

// The release package of a version, laid out as make-release-zip.py lays it out: everything below
// PPSA99249/, with folder entries. It carries some of the user's places too (bios/README.txt,
// games/README.txt as the real package does, and, to be sure they are passed over, an
// inis/PCSX2.ini and a resources/patches.zip).
static std::vector<Item> Package(const std::string& version, size_t eboot_size = 300000)
{
	const std::string t = "PPSA99249/";
	std::vector<Item> items;
	const auto folder = [&](const std::string& name) { items.push_back({t + name, "", true}); };
	const auto file = [&](const std::string& name, const std::string& data) { items.push_back({t + name, data}); };
	items.push_back({t, "", true});
	file("LEGAL.txt", "legal " + version);
	file("README.md", "# Crux2 " + version);
	folder("bios/");
	file("bios/README.txt", "put a BIOS here (" + version + ")");
	// A text that compresses well, as an eboot.bin does in part, and a part that does not.
	std::string eboot = "EBOOT " + version + "\n";
	while (eboot.size() < eboot_size / 2)
		eboot += "the same line again and again ";
	eboot += Filler(version, eboot_size - eboot.size());
	file("eboot.bin", eboot);
	folder("games/");
	file("games/README.txt", "put games here (" + version + ")");
	folder("inis/");
	file("inis/PCSX2.ini", "[a release must never carry this] " + version);
	folder("licenses/");
	file("licenses/GPL-3.0.txt", "GPL " + version);
	folder("resources/");
	file("resources/GameIndex.yaml", "games " + version);
	file("resources/patches.zip", "the release's patches " + version);
	folder("resources/fonts/");
	file("resources/fonts/Roboto-Bold.ttf", "font " + version);
	folder("resources/new-in-" + version + "/");
	file("resources/new-in-" + version + "/thing.txt", "new " + version);
	file("sandbox-elevator.elf", "elevator " + version);
	folder("sce_module/");
	file("sce_module/libc.prx", "libc " + version);
	folder("sce_sys/");
	file("sce_sys/icon0.png", "icon " + version);
	file("sce_sys/param.json", "{\"titleId\": \"PPSA99249\", \"contentVersion\": \"" + version + "\"}");
	file("symbols.txt", "symbols " + version);
	return items;
}

// The title folder of an older version, with a user's files in it.
static void OldFolder(const std::string& app)
{
	Run("rm -rf '" + app + "'");
	MakeDirs(app);
	Put(app + "/eboot.bin", "EBOOT old");
	Put(app + "/LEGAL.txt", "legal old");
	Put(app + "/README.md", "readme old");
	Put(app + "/symbols.txt", "symbols old");
	Put(app + "/sandbox-elevator.elf", "elevator old");
	Put(app + "/sce_sys/param.json", "{\"titleId\": \"PPSA99249\", \"contentVersion\": \"old\"}");
	Put(app + "/sce_sys/icon0.png", "icon old");
	Put(app + "/sce_module/libc.prx", "libc old");
	Put(app + "/sce_module/extra.prx", "a module the new release does not have");
	Put(app + "/resources/GameIndex.yaml", "games old");
	Put(app + "/resources/fonts/Roboto-Bold.ttf", "font old");
	Put(app + "/licenses/GPL-3.0.txt", "GPL old");
	Put(app + "/licenses/old-only.txt", "a notice the new release does not have");
	// The user's.
	Put(app + "/resources/patches.zip", "the user's patches");
	Put(app + "/inis/PCSX2.ini", "[UI]\nmine=1\n");
	Put(app + "/memcards/Mcd001.ps2", Filler("card", 4096));
	Put(app + "/bios/scph39001.bin", Filler("bios", 8192));
	Put(app + "/bios/README.txt", "put a BIOS here (old)");
	Put(app + "/sstates/SLUS-20312 (0A1B2C3D).01.p2s", "state");
	Put(app + "/covers/SLUS-20312.png", "cover");
	Put(app + "/network.cfg", "share=smb://192.0.2.10/games\n");
	Put(app + "/startup.wav", "my sound");
	Put(app + "/crux2-boot.log", "log");
	Put(app + "/games/README.txt", "put games here (old)");
}

// ------------------------------------------------------------------------------ the releases

struct Asset
{
	std::string name, url;
	uint64_t size = 0;
	std::string digest; // "sha256:..." or empty
};

struct Rel
{
	std::string tag, name, body;
	bool draft = false, prerelease = true;
	std::vector<Asset> assets;
};

static std::string Json(const std::string& text)
{
	std::string out = "\"";
	for (const unsigned char c : text)
	{
		if (c == '"' || c == '\\')
			out += '\\', out += static_cast<char>(c);
		else if (c == '\n')
			out += "\\n";
		else if (c < 0x20)
		{
			char buffer[8];
			std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
			out += buffer;
		}
		else
			out += static_cast<char>(c);
	}
	return out + "\"";
}

static std::string ReleasesJson(const std::vector<Rel>& releases)
{
	std::string out = "[";
	for (size_t i = 0; i < releases.size(); i++)
	{
		const Rel& r = releases[i];
		out += i ? ",\n{" : "{";
		out += "\"url\": \"https://api.github.com/x\", \"id\": " + std::to_string(1000 + i) + ", \"tag_name\": " +
		       Json(r.tag) + ", \"name\": " + Json(r.name) + ", \"draft\": " + (r.draft ? "true" : "false") +
		       ", \"prerelease\": " + (r.prerelease ? "true" : "false") +
		       ", \"published_at\": \"2026-10-1" + std::to_string(i % 10) + "T12:00:00Z\", \"author\": {\"login\": \"x\"}, \"assets\": [";
		for (size_t j = 0; j < r.assets.size(); j++)
		{
			const Asset& a = r.assets[j];
			out += j ? ", {" : "{";
			out += "\"name\": " + Json(a.name) + ", \"browser_download_url\": " + Json(a.url) +
			       ", \"size\": " + std::to_string(a.size) + ", \"content_type\": \"application/zip\"";
			if (!a.digest.empty())
				out += ", \"digest\": " + Json(a.digest);
			out += "}";
		}
		out += "], \"body\": " + Json(r.body) + "}";
	}
	return out + "]";
}

// -------------------------------------------------------------------------------- the server

static const std::string API = "https://api.github.com/repos/Press5elect/Crux2/releases?per_page=20";
static const std::string DOWNLOADS = "https://github.com/Press5elect/Crux2/releases/download/";

struct Server
{
	std::mutex mutex;
	std::map<std::string, std::pair<int, std::string>> pages; // url -> code, body
	std::vector<std::string> asked;
	std::atomic<int> delay_ms{0};
	size_t chunk = 65536;

	void Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		pages.clear();
		asked.clear();
		delay_ms = 0;
	}
	void Serve(const std::string& url, const std::string& body, int code = 200)
	{
		std::lock_guard<std::mutex> lock(mutex);
		pages[url] = {code, body};
	}
	bool Find(const std::string& url, int& code, std::string& body)
	{
		std::lock_guard<std::mutex> lock(mutex);
		asked.push_back(url);
		const auto found = pages.find(url);
		if (found == pages.end())
		{
			code = 404;
			body = "{\"message\": \"Not Found\"}";
			return false;
		}
		code = found->second.first;
		body = found->second.second;
		return true;
	}
	bool Asked(const std::string& url)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const std::string& one : asked)
			if (one == url)
				return true;
		return false;
	}
};
static Server s_server;

static std::mutex s_log_mutex;
static std::vector<std::string> s_log;

static Setup MakeSetup(const std::string& app, const std::string& version)
{
	Setup setup;
	setup.app_dir = app;
	setup.version = version;
	setup.http_get = [](const std::string& url, std::vector<uint8_t>& out, unsigned) {
		int code;
		std::string body;
		s_server.Find(url, code, body);
		if (code < 0)
			return code;
		out.assign(body.begin(), body.end());
		return code;
	};
	setup.http_download = [](const std::string& url, const std::string& path, uint64_t limit,
							  const std::function<bool(uint64_t, uint64_t)>& progress) {
		int code;
		std::string body;
		s_server.Find(url, code, body);
		if (code != 200)
			return code;
		if (body.size() > limit)
			return -1;
		const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			return -1;
		size_t done = 0;
		while (done < body.size())
		{
			const size_t take = std::min(s_server.chunk, body.size() - done);
			if (write(fd, body.data() + done, take) != static_cast<ssize_t>(take))
			{
				close(fd);
				return -1;
			}
			done += take;
			if (s_server.delay_ms > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(s_server.delay_ms.load()));
			if (!progress(done, body.size()))
			{
				close(fd);
				return -1;
			}
		}
		close(fd);
		return 200;
	};
	setup.log = [](const char* line) {
		std::lock_guard<std::mutex> lock(s_log_mutex);
		s_log.push_back(line);
	};
	return setup;
}

static Status Wait()
{
	for (int i = 0; i < 20000; i++)
	{
		const Status status = GetStatus();
		if (status.state != State::Checking && status.state != State::Downloading && status.state != State::Verifying)
			return status;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	std::fprintf(stderr, "the updater did not finish in time\n");
	std::exit(2);
}

// A release of `version` served with its ZIP (whose bytes are `zip`), and, as asked, a
// SHA256SUMS, a .sha256 file and GitHub's digest; their checksums can be made wrong.
struct Serving
{
	bool sums = true, single = false, digest = true;
	std::string sums_hash, single_hash, digest_hash; // empty: the right one
	bool zip_asset = true;
	int64_t size = -1; // -1: the right one
};

static Rel ServeRelease(const std::string& version, const std::string& zip, const Serving& how = {})
{
	const std::string tag = "v" + version;
	const std::string name = "Crux2-" + tag + ".zip";
	const std::string hash = Sha256Hex(zip.data(), zip.size());
	Rel rel;
	rel.tag = tag;
	rel.name = "Crux2 " + tag;
	rel.body = "## What's new in " + tag + "\n\n- Faster in " + tag;
	if (how.zip_asset)
	{
		Asset asset{name, DOWNLOADS + tag + "/" + name, how.size < 0 ? zip.size() : static_cast<uint64_t>(how.size), ""};
		if (how.digest)
			asset.digest = "sha256:" + (how.digest_hash.empty() ? hash : how.digest_hash);
		rel.assets.push_back(asset);
		s_server.Serve(asset.url, zip);
	}
	rel.assets.push_back({"Crux2-" + tag + "-source.zip", DOWNLOADS + tag + "/Crux2-" + tag + "-source.zip", 10, ""});
	if (how.sums)
	{
		const std::string url = DOWNLOADS + tag + "/SHA256SUMS";
		rel.assets.push_back({"SHA256SUMS", url, 100, ""});
		s_server.Serve(url, (how.sums_hash.empty() ? hash : how.sums_hash) + "  " + name + "\n" +
								std::string(64, 'c') + "  Crux2-" + tag + "-source.zip\n");
	}
	if (how.single)
	{
		const std::string url = DOWNLOADS + tag + "/" + name + ".sha256";
		rel.assets.push_back({name + ".sha256", url, 80, ""});
		s_server.Serve(url, (how.single_hash.empty() ? hash : how.single_hash) + "  " + name + "\n");
	}
	return rel;
}

// From a fresh start up to Ready (or whatever came of it): an old folder, one release, Check(),
// Download().
static Status ToReady(const std::string& work, const std::string& app, const std::string& zip,
	const Serving& how = {}, const std::string& running = "1.0.0")
{
	s_server.Clear();
	OldFolder(app);
	const Rel rel = ServeRelease("1.2.0", zip, how);
	s_server.Serve(API, ReleasesJson({rel}));
	Init(MakeSetup(app, running));
	Check();
	const Status checked = Wait();
	if (checked.state != State::Available)
		return checked;
	Download();
	(void)work;
	return Wait();
}

// Whether the title folder is just as `before` and .update/ is gone.
static bool Unchanged(const std::string& app, const std::map<std::string, std::string>& before)
{
	const auto now = Snapshot(app);
	if (now != before)
	{
		for (const auto& [path, data] : now)
			if (!before.count(path) || before.at(path) != data)
				std::fprintf(stderr, "  changed or new: %s\n", path.c_str());
		for (const auto& [path, data] : before)
			if (!now.count(path))
				std::fprintf(stderr, "  missing: %s\n", path.c_str());
		return false;
	}
	return true;
}

// ------------------------------------------------------------------------------------- tests

static void TestHelpers()
{
	std::fprintf(stderr, "versions, names, text, SHA-256\n");
	Version v = ParseVersion("1.0.0");
	CHECK(v.valid && v.major == 1 && v.minor == 0 && v.patch == 0);
	v = ParseVersion("v1.2.3");
	CHECK(v.valid && v.major == 1 && v.minor == 2 && v.patch == 3);
	v = ParseVersion("  V10.20.30-test");
	CHECK(v.valid && v.major == 10 && v.minor == 20 && v.patch == 30);
	v = ParseVersion("1.2");
	CHECK(v.valid && v.major == 1 && v.minor == 2 && v.patch == 0);
	CHECK(ToString(ParseVersion("v0.9.0-rc1")) == "0.9.0");
	CHECK(!ParseVersion("").valid);
	CHECK(!ParseVersion("abc").valid);
	CHECK(!ParseVersion("7").valid);
	CHECK(!ParseVersion("vv1.0.0").valid);
	CHECK(!ParseVersion("1234567890.0.0").valid);
	CHECK(CompareVersions(ParseVersion("1.0.0"), ParseVersion("1.0.1")) < 0);
	CHECK(CompareVersions(ParseVersion("1.0.1"), ParseVersion("1.1.0")) < 0);
	CHECK(CompareVersions(ParseVersion("1.1.0"), ParseVersion("2.0.0")) < 0);
	CHECK(CompareVersions(ParseVersion("1.10.0"), ParseVersion("1.9.9")) > 0);
	CHECK(CompareVersions(ParseVersion("v1.0.0"), ParseVersion("1.0.0-test")) == 0);
	CHECK(CompareVersions(ParseVersion("1.0"), ParseVersion("1.0.0")) == 0);
	CHECK(CompareVersions(ParseVersion("x"), ParseVersion("0.0.1")) < 0);
	CHECK(CompareVersions(ParseVersion("x"), ParseVersion("y")) == 0);
	CHECK(ToString(VersionIn("Crux2-v1.0.0.zip")) == "1.0.0");
	CHECK(ToString(VersionIn("Crux2-v1.2.3-part1of3.zip")) == "1.2.3");
	CHECK(ToString(VersionIn("v0.9.0")) == "0.9.0");
	CHECK(ToString(VersionIn("Crux2 v2.10.4 (test)")) == "2.10.4");
	CHECK(ToString(VersionIn("release-1.4.0")) == "1.4.0");
	CHECK(!VersionIn("Crux2").valid);
	CHECK(!VersionIn("Crux2-source.zip").valid);
	CHECK(!VersionIn("PPSA99249").valid);

	CHECK(IsManaged("eboot.bin"));
	CHECK(IsManaged("sce_sys/param.json"));
	CHECK(IsManaged("sce_module/libc.prx"));
	CHECK(IsManaged("resources/fonts/Roboto-Bold.ttf"));
	CHECK(IsManaged("resources/GameIndex.yaml"));
	CHECK(IsManaged("licenses/GPL-3.0.txt"));
	CHECK(IsManaged("LEGAL.txt"));
	CHECK(IsManaged("README.md"));
	CHECK(IsManaged("symbols.txt"));
	CHECK(IsManaged("sandbox-elevator.elf"));
	CHECK(!IsManaged("resources/patches.zip"));
	CHECK(!IsManaged("resources/Patches.ZIP"));
	CHECK(!IsManaged("inis/PCSX2.ini"));
	CHECK(!IsManaged("memcards/Mcd001.ps2"));
	CHECK(!IsManaged("bios/README.txt"));
	CHECK(!IsManaged("bios/scph39001.bin"));
	CHECK(!IsManaged("games/README.txt"));
	CHECK(!IsManaged("network.cfg"));
	CHECK(!IsManaged("startup.wav"));
	CHECK(!IsManaged("background.png"));
	CHECK(!IsManaged("crux2-boot.log"));
	CHECK(!IsManaged("radv-shader-cache/x"));
	CHECK(!IsManaged(".update/stage/eboot.bin"));
	CHECK(!IsManaged("sce_sys"));
	CHECK(!IsManaged("sce_sys/"));
	CHECK(!IsManaged("resources/../inis/PCSX2.ini"));
	CHECK(!IsManaged("/eboot.bin"));
	CHECK(!IsManaged("resources\\x"));
	CHECK(!IsManaged("EBOOT.BIN"));
	CHECK(!IsManaged("something-new.txt"));

	const std::string plain = PlainText(
		"## What's new\n\n> [!NOTE]\n> Read **this** first.\n\n* A `fast` path, see [the notes](https://example.org/n).\n"
		"![picture](https://example.org/p.png)\n\n\n\n### Fixed\n---\n- Item [one]\n```\ncode **kept**\n```\n");
	CHECK(plain == "What's new\n\nRead this first.\n\n- A fast path, see the notes.\npicture\n\nFixed\n\n- Item [one]\n"
	               "code **kept**");
	if (plain != "What's new\n\nRead this first.\n\n- A fast path, see the notes.\npicture\n\nFixed\n\n- Item [one]\ncode **kept**")
		std::fprintf(stderr, "  plain text was:\n%s\n", plain.c_str());

	CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char* long_one = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(Sha256Hex(long_one, std::strlen(long_one)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	const std::string million(1000000, 'a');
	CHECK(Sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void TestCheck(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "asking for releases\n");
	OldFolder(app);
	const std::string zip12 = MakeZip(work, Package("1.2.0"));
	const std::string zip11 = MakeZip(work, Package("1.1.0"));

	const auto releases = [&](bool shuffled) {
		s_server.Clear();
		Rel draft = ServeRelease("2.0.0", zip12);
		draft.draft = true;
		draft.body = "Draft notes nobody may see";
		Rel r12 = ServeRelease("1.2.0", zip12);
		r12.body = "# Crux2 v1.2.0\n\n> [!NOTE]\n> Pre-release.\n\n- **Faster** shaders, see [the README](https://example.org)\n- `eboot.bin` smaller";
		Rel r11 = ServeRelease("1.1.0", zip11);
		r11.name = "Second";
		r11.body = "### Fixed\n* Cards";
		Rel r10;
		r10.tag = "v1.0.0";
		r10.name = "Crux2 v1.0.0";
		r10.body = "first notes";
		Rel r09;
		r09.tag = "v0.9.0";
		r09.name = "";
		r09.body = "older notes";
		Rel junk;
		junk.tag = "nightly";
		junk.name = "a release with no version";
		junk.body = "junk";
		std::vector<Rel> list = {draft, r12, r11, r10, r09, junk};
		if (shuffled)
			list = {r10, r11, junk, draft, r09, r12};
		s_server.Serve(API, ReleasesJson(list));
	};

	for (bool shuffled : {false, true})
	{
		releases(shuffled);
		Init(MakeSetup(app, "1.0.0"));
		Check();
		const Status status = Wait();
		CHECK(status.state == State::Available);
		CHECK(status.version == "1.2.0");
		CHECK(status.tag == "v1.2.0");
		CHECK(status.name == "Crux2 v1.2.0");
		CHECK(status.newer.size() == 2);
		if (status.newer.size() == 2)
		{
			CHECK(status.newer[0].version == "1.2.0" && status.newer[1].version == "1.1.0");
			CHECK(status.newer[0].prerelease);
			CHECK(status.newer[1].name == "Second");
			CHECK(status.newer[1].text == "Fixed\n- Cards");
			CHECK(status.newer[0].date.size() == 10 && status.newer[0].date.compare(0, 8, "2026-10-") == 0);
		}
		CHECK(Contains(status.notes, "Faster shaders, see the README"));
		CHECK(Contains(status.notes, "eboot.bin smaller"));
		CHECK(Contains(status.notes, "Pre-release."));
		CHECK(Contains(status.notes, "v1.1.0 - Second"));
		CHECK(status.notes.find("Crux2 v1.2.0") < status.notes.find("v1.1.0 - Second"));
		CHECK(!Contains(status.notes, "**") && !Contains(status.notes, "`") && !Contains(status.notes, "]("));
		CHECK(!Contains(status.notes, "[!NOTE]") && !Contains(status.notes, "# "));
		CHECK(!Contains(status.notes, "first notes") && !Contains(status.notes, "older notes"));
		CHECK(!Contains(status.notes, "Draft") && !Contains(status.notes, "junk"));
		CHECK(!status.reinstall);
		CHECK(status.error.empty());
	}
	// What was asked: the one releases page, and nothing that says who is asking.
	{
		std::lock_guard<std::mutex> lock(s_server.mutex);
		CHECK(s_server.asked.size() == 1 && s_server.asked[0] == API);
	}

	// Up to date, and newer than the newest.
	for (const char* running : {"1.2.0", "v1.2.0", "1.3.0-test"})
	{
		releases(false);
		Init(MakeSetup(app, running));
		Check();
		const Status status = Wait();
		CHECK(status.state == State::UpToDate);
		CHECK(status.version == "1.2.0");
		CHECK(status.reinstall);
		CHECK(status.newer.empty() && status.notes.empty());
	}
	// A running version that cannot be read: every release is newer.
	releases(false);
	Init(MakeSetup(app, "unknown"));
	Check();
	{
		const Status status = Wait();
		CHECK(status.state == State::Available && status.newer.size() == 4);
		CHECK(Contains(status.notes, "older notes") && Contains(status.notes, "v0.9.0"));
	}
	// An older one in between: only those newer than it.
	releases(false);
	Init(MakeSetup(app, "1.1.0"));
	Check();
	{
		const Status status = Wait();
		CHECK(status.state == State::Available && status.newer.size() == 1);
	}

	// The newest has no ZIP (yet).
	s_server.Clear();
	{
		Rel r13 = ServeRelease("1.3.0", zip12, Serving{true, false, true, "", "", "", false});
		s_server.Serve(API, ReleasesJson({r13, ServeRelease("1.2.0", zip12)}));
	}
	Init(MakeSetup(app, "1.0.0"));
	Check();
	{
		const Status status = Wait();
		CHECK(status.state == State::NoRelease);
		CHECK(Contains(status.error, "Crux2-v1.3.0.zip"));
		CHECK(status.version == "1.3.0");
		Download(); // nothing to download: nothing starts
		CHECK(GetStatus().state == State::NoRelease);
	}
	// Up to date with a newest release that has no ZIP: nothing to install again.
	Init(MakeSetup(app, "1.3.0"));
	Check();
	{
		const Status status = Wait();
		CHECK(status.state == State::UpToDate && !status.reinstall);
		Reinstall();
		CHECK(GetStatus().state == State::UpToDate);
	}

	// Answers that are not a list of releases.
	const struct
	{
		int code;
		const char* body;
		State state;
		const char* says;
	} answers[] = {
		{200, "[]", State::NoRelease, "no release"},
		{200, "[{\"tag_name\": \"nightly\", \"draft\": false}]", State::NoRelease, "no release"},
		{404, "{}", State::NoRelease, "not found"},
		{403, "{\"message\": \"API rate limit exceeded\"}", State::Failed, "Try again later"},
		{500, "", State::Failed, "500"},
		{-1, "", State::Failed, "online"},
		{200, "[{\"tag_name\": \"v1.2.0\", ", State::Failed, "could not be read"},
		{200, "{\"tag_name\": \"v1.2.0\"}", State::Failed, "could not be read"},
		{200, "not json at all", State::Failed, "could not be read"},
	};
	for (const auto& answer : answers)
	{
		s_server.Clear();
		s_server.Serve(API, answer.body, answer.code);
		Init(MakeSetup(app, "1.0.0"));
		Check();
		const Status status = Wait();
		CHECK(status.state == answer.state);
		CHECK(Contains(status.error, answer.says));
		if (!Contains(status.error, answer.says))
			std::fprintf(stderr, "  said: %s\n", status.error.c_str());
	}
	// A deeply nested answer is read without recursion (rapidjson's iterative parser).
	s_server.Clear();
	s_server.Serve(API, std::string(100000, '[') + std::string(100000, ']'));
	Init(MakeSetup(app, "1.0.0"));
	Check();
	CHECK(Wait().state == State::NoRelease);
	// A repository that is no "owner/name".
	for (const char* repository : {"evil/../x", "", "noslash", "a/b/c", "a/b?c"})
	{
		Setup setup = MakeSetup(app, "1.0.0");
		setup.repository = repository;
		s_server.Clear();
		Init(setup);
		Check();
		CHECK(Wait().state == State::Failed);
		std::lock_guard<std::mutex> lock(s_server.mutex);
		CHECK(s_server.asked.empty());
	}
	// Not initialised for a moment: Check() before Init() of another set-up does no harm.
	Init(MakeSetup(app, "1.0.0"));
	CHECK(GetStatus().state == State::Idle);
}

static void TestInstall(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "download, check, unpack, install\n");
	const std::string zip = MakeZip(work, Package("1.2.0"));
	const Status ready = ToReady(work, app, zip);
	CHECK(ready.state == State::Ready);
	CHECK(ready.done == zip.size() && ready.total == zip.size());
	const auto before = Snapshot(app, true);
	// Unpacked beside the title: the program's files only, eboot.bin last in the plan.
	CHECK(Get(app + "/.update/stage/eboot.bin").compare(0, 12, "EBOOT 1.2.0\n") == 0);
	CHECK(Exists(app + "/.update/stage/sce_sys/param.json"));
	CHECK(Exists(app + "/.update/stage/resources/new-in-1.2.0/thing.txt"));
	CHECK(!Exists(app + "/.update/stage/inis"));
	CHECK(!Exists(app + "/.update/stage/bios"));
	CHECK(!Exists(app + "/.update/stage/games"));
	CHECK(!Exists(app + "/.update/stage/resources/patches.zip"));
	{
		const std::string plan = Get(app + "/.update/plan");
		CHECK(plan.size() > 10 && plan.compare(plan.size() - 10, 10, "eboot.bin\n") == 0);
		CHECK(!Contains(plan, "inis/") && !Contains(plan, "patches.zip") && !Contains(plan, "bios/"));
	}
	struct stat st;
	CHECK(stat((app + "/.update/stage/eboot.bin").c_str(), &st) == 0 && (st.st_mode & 0777) == 0777);
	// Nothing of the title changed yet.
	OldFolder(work + "/reference");
	CHECK(Snapshot(work + "/reference") == before);

	// Waiting: a check now would only lose it.
	Check();
	CHECK(GetStatus().state == State::Ready);
	// Recover() must not take away what is waiting.
	CHECK(!Recover(app));
	CHECK(Exists(app + "/.update/plan"));

	CHECK(Install());
	const Status installed = GetStatus();
	CHECK(installed.state == State::Installed);
	CHECK(!Exists(app + "/.update"));
	// The program's files are the release's.
	for (const Item& item : Package("1.2.0"))
	{
		if (item.folder)
			continue;
		const std::string path = item.name.substr(10);
		if (IsManaged(path))
		{
			CHECK(Get(app + "/" + path) == item.data);
			CHECK(stat((app + "/" + path).c_str(), &st) == 0 && (st.st_mode & 0777) == 0777);
		}
	}
	// The user's are as they were, also those the release carries a file of.
	CHECK(Get(app + "/inis/PCSX2.ini") == "[UI]\nmine=1\n");
	CHECK(Get(app + "/memcards/Mcd001.ps2") == Filler("card", 4096));
	CHECK(Get(app + "/bios/scph39001.bin") == Filler("bios", 8192));
	CHECK(Get(app + "/bios/README.txt") == "put a BIOS here (old)");
	CHECK(Get(app + "/games/README.txt") == "put games here (old)");
	CHECK(Get(app + "/resources/patches.zip") == "the user's patches");
	CHECK(Get(app + "/network.cfg") == "share=smb://192.0.2.10/games\n");
	CHECK(Get(app + "/startup.wav") == "my sound");
	CHECK(Get(app + "/crux2-boot.log") == "log");
	CHECK(Get(app + "/sstates/SLUS-20312 (0A1B2C3D).01.p2s") == "state");
	CHECK(Get(app + "/covers/SLUS-20312.png") == "cover");
	// Files the release does not have are left.
	CHECK(Get(app + "/sce_module/extra.prx") == "a module the new release does not have");
	CHECK(Get(app + "/licenses/old-only.txt") == "a notice the new release does not have");
	// Every file of the folder now: the release's managed files, and the rest exactly as before.
	{
		const auto after = Snapshot(app);
		for (const auto& [path, data] : before)
			if (!IsManaged(path) && path.back() != '/')
				CHECK(after.count(path) && after.at(path) == data);
	}
	// Once Installed, nothing more is started.
	Check();
	CHECK(GetStatus().state == State::Installed);
	CHECK(!Install());

	// Install again: the folder is v1.2.0 now, and up to date.
	std::fprintf(stderr, "install again\n");
	{
		const auto installed_files = Snapshot(app);
		Put(app + "/eboot.bin", "damaged");
		s_server.Clear();
		const Rel rel = ServeRelease("1.2.0", zip);
		s_server.Serve(API, ReleasesJson({rel}));
		Init(MakeSetup(app, "1.2.0"));
		Check();
		CHECK(Wait().state == State::UpToDate);
		CHECK(GetStatus().reinstall);
		Reinstall();
		const Status status = Wait();
		CHECK(status.state == State::Ready);
		CHECK(status.version == "1.2.0");
		CHECK(Install());
		CHECK(Snapshot(app) == installed_files);
	}

	// A second release on top of it: the folders the first one added stay, the new one is added.
	std::fprintf(stderr, "a release after that\n");
	{
		const std::string zip13 = MakeZip(work, Package("1.3.0"));
		s_server.Clear();
		const Rel rel = ServeRelease("1.3.0", zip13);
		s_server.Serve(API, ReleasesJson({rel}));
		Init(MakeSetup(app, "1.2.0"));
		Check();
		CHECK(Wait().state == State::Available);
		Download();
		CHECK(Wait().state == State::Ready);
		CHECK(Install());
		CHECK(Get(app + "/eboot.bin").compare(0, 12, "EBOOT 1.3.0\n") == 0);
		CHECK(Exists(app + "/resources/new-in-1.2.0/thing.txt"));
		CHECK(Exists(app + "/resources/new-in-1.3.0/thing.txt"));
		CHECK(Get(app + "/inis/PCSX2.ini") == "[UI]\nmine=1\n");
	}
}

static void TestChecksums(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "checksums and sizes\n");
	const std::string zip = MakeZip(work, Package("1.2.0"));
	const std::string wrong = std::string(64, 'a');
	const struct
	{
		Serving how;
		bool ready;
		const char* says;
	} cases[] = {
		{Serving{true, false, true, wrong, "", "", true, -1}, false, "checksum"},       // SHA256SUMS wrong
		{Serving{true, false, true, "", "", wrong, true, -1}, false, "checksum"},       // GitHub's digest wrong
		{Serving{false, false, true, "", "", wrong, true, -1}, false, "checksum"},      // the digest alone, wrong
		{Serving{false, true, false, "", wrong, "", true, -1}, false, "checksum"},      // the .sha256 file wrong
		{Serving{true, true, false, "", wrong, "", true, -1}, false, "differ"},         // two that disagree
		{Serving{true, false, true, "", "", "", true, static_cast<int64_t>(zip.size()) + 1}, false, "bytes"},
		{Serving{false, true, false, "", "", "", true, -1}, true, ""},                  // the .sha256 file alone
		{Serving{true, true, true, "", "", "", true, -1}, true, ""},                    // all three
		{Serving{false, false, false, "", "", "", true, -1}, true, ""},                 // none: the ZIP's own CRCs
	};
	for (const auto& one : cases)
	{
		const Status status = ToReady(work, app, zip, one.how);
		if (one.ready)
		{
			CHECK(status.state == State::Ready);
			continue;
		}
		CHECK(status.state == State::Failed);
		CHECK(Contains(status.error, one.says));
		if (!Contains(status.error, one.says))
			std::fprintf(stderr, "  said: %s\n", status.error.c_str());
		OldFolder(work + "/reference");
		CHECK(Unchanged(app, Snapshot(work + "/reference")));
		CHECK(!Install());
	}
	// SHA256SUMS without a line for the ZIP: it says nothing of it.
	s_server.Clear();
	OldFolder(app);
	{
		Rel rel = ServeRelease("1.2.0", zip, Serving{false, false, false, "", "", "", true, -1});
		rel.assets.push_back({"SHA256SUMS", DOWNLOADS + "v1.2.0/SHA256SUMS", 70, ""});
		s_server.Serve(DOWNLOADS + "v1.2.0/SHA256SUMS", std::string(64, 'd') + "  something-else.zip\n");
		s_server.Serve(API, ReleasesJson({rel}));
	}
	Init(MakeSetup(app, "1.0.0"));
	Check();
	Wait();
	Download();
	CHECK(Wait().state == State::Ready);
	// A checksum file that cannot be fetched.
	s_server.Clear();
	OldFolder(app);
	{
		Rel rel = ServeRelease("1.2.0", zip, Serving{false, true, false, "", "", "", true, -1});
		s_server.Serve(API, ReleasesJson({rel}));
		s_server.Serve(DOWNLOADS + "v1.2.0/Crux2-v1.2.0.zip.sha256", "", 500);
	}
	Init(MakeSetup(app, "1.0.0"));
	Check();
	Wait();
	Download();
	{
		const Status status = Wait();
		CHECK(status.state == State::Failed && Contains(status.error, "checksum could not be read"));
	}
	// The ZIP itself not served.
	s_server.Clear();
	OldFolder(app);
	{
		Rel rel = ServeRelease("1.2.0", zip, Serving{false, false, false, "", "", "", true, -1});
		s_server.Serve(API, ReleasesJson({rel}));
		s_server.Serve(DOWNLOADS + "v1.2.0/Crux2-v1.2.0.zip", "", 404);
	}
	Init(MakeSetup(app, "1.0.0"));
	Check();
	Wait();
	Download();
	{
		const Status status = Wait();
		CHECK(status.state == State::Failed && Contains(status.error, "404"));
		CHECK(!Exists(app + "/.update"));
	}
	// Bytes that are no ZIP, with no checksum to catch it.
	{
		const Status status = ToReady(work, app, "PK\x03\x04 this is not a zip", Serving{false, false, false, "", "", "", true, -1});
		CHECK(status.state == State::Failed && Contains(status.error, "not a ZIP"));
		CHECK(!Exists(app + "/.update"));
	}
	// A ZIP damaged after it was made (a byte of eboot.bin's data changed): the CRC catches it.
	{
		// eboot.bin's data is most of the archive: a byte a third of the way in is in it.
		std::string damaged = MakeZip(work, Package("1.2.0"));
		damaged[damaged.size() / 3] ^= 0x55;
		const Status status = ToReady(work, app, damaged, Serving{false, false, false, "", "", "", true, -1});
		CHECK(status.state == State::Failed);
		OldFolder(work + "/reference");
		CHECK(Unchanged(app, Snapshot(work + "/reference")));
	}
}

static void TestRefusedZips(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "ZIPs that are refused\n");
	const std::string t = "PPSA99249/";
	struct Case
	{
		const char* what;
		std::vector<Item> items;
		std::vector<std::pair<std::string, std::string>> patches;
		const char* says;
	};
	std::vector<Case> cases;
	{
		auto items = Package("1.2.0");
		items.push_back({t + "zz/x", "escape"});
		cases.push_back({"a \"..\" entry", items, {{t + "zz/x", t + "../x"}}, "\"..\""});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({"xetc/passwd", "escape"});
		cases.push_back({"an absolute path", items, {{"xetc/passwd", "/etc/passwd"}}, "absolute"});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({t + "resources/aXb", "x"});
		cases.push_back({"a backslash", items, {{t + "resources/aXb", t + "resources/a\\b"}}, "backslash"});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({t + "resources/link", "/etc/passwd", false, 0120777});
		cases.push_back({"a symbolic link", items, {}, "symbolic link"});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({t + "resources/fifo", "", false, 010777});
		cases.push_back({"a special file", items, {}, "not a plain file"});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({"OTHER/eboot.bin", "x"});
		cases.push_back({"an entry outside the title's folder", items, {}, "outside"});
	}
	{
		auto items = Package("1.2.0");
		items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.name == t + "eboot.bin"; }),
			items.end());
		cases.push_back({"no eboot.bin", items, {}, "no eboot.bin"});
	}
	{
		auto items = Package("1.2.0");
		items.erase(std::remove_if(items.begin(), items.end(),
						[&](const Item& i) { return i.name == t + "sce_sys/param.json"; }),
			items.end());
		cases.push_back({"no param.json", items, {}, "no sce_sys/param.json"});
	}
	{
		auto items = Package("1.2.0");
		for (Item& item : items)
			if (item.name == t + "sce_sys/param.json")
				item.data = "{\"titleId\": \"PPSA99248\"}";
		cases.push_back({"another title's param.json", items, {}, "param.json is not"});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({t + "readme.md", "x"});
		cases.push_back({"two names that differ only in case", items, {}, "twice"});
	}
	{
		auto items = Package("1.2.0");
		items.push_back({t + "resources/GameIndex.yaml/inner", "x"});
		cases.push_back({"a file below a file", items, {}, "below something"});
	}
	{
		// The same name twice (written as two names and patched into one).
		auto items = Package("1.2.0");
		items.push_back({t + "LEGAL.tx2", "again"});
		cases.push_back({"one name twice", items, {{t + "LEGAL.tx2", t + "LEGAL.txt"}}, "not a ZIP that can be read"});
	}
	for (const Case& one : cases)
	{
		std::fprintf(stderr, "  %s\n", one.what);
		const std::string zip = MakeZip(work, one.items, one.patches);
		const Status status = ToReady(work, app, zip);
		CHECK(status.state == State::Failed);
		// (libzip itself refuses an archive with one name twice, before the updater looks at it.)
		CHECK(Contains(status.error, "refused") || Contains(status.error, "not a ZIP"));
		CHECK(Contains(status.error, one.says));
		if (!Contains(status.error, one.says))
			std::fprintf(stderr, "  said: %s\n", status.error.c_str());
		OldFolder(work + "/reference");
		CHECK(Unchanged(app, Snapshot(work + "/reference")));
		CHECK(!Exists(work + "/x") && !Exists(app + "/../x"));
	}
	// Too large, and too many entries (the limits made small for the test).
	{
		const std::string zip = MakeZip(work, Package("1.2.0"));
		Test::max_unpacked = 100000;
		Status status = ToReady(work, app, zip);
		Test::max_unpacked = 0;
		CHECK(status.state == State::Failed && Contains(status.error, "unpacks to more than"));
		Test::max_entries = 5;
		status = ToReady(work, app, zip);
		Test::max_entries = 0;
		CHECK(status.state == State::Failed && Contains(status.error, "too many"));
		OldFolder(work + "/reference");
		CHECK(Unchanged(app, Snapshot(work + "/reference")));
	}
	// Entries without a Unix mode (a ZIP made elsewhere) are taken as plain files and folders.
	{
		auto items = Package("1.2.0");
		for (Item& item : items)
			item.mode = 0;
		const Status status = ToReady(work, app, MakeZip(work, items));
		CHECK(status.state == State::Ready);
	}
}

static void TestInterrupted(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "installs interrupted, renames that fail or say they worked when they did not\n");
	const std::string zip = MakeZip(work, Package("1.2.0"));
	OldFolder(work + "/reference");
	const auto original = Snapshot(work + "/reference");

	// How many renames a whole install makes.
	CHECK(ToReady(work, app, zip).state == State::Ready);
	std::string plan = Get(app + "/.update/plan");
	int files = 0;
	for (char c : plan)
		files += c == '\n';
	CHECK(files == 12);
	int existing = 0;
	for (size_t start = 0; start < plan.size();)
	{
		const size_t end = plan.find('\n', start);
		existing += Exists(app + "/" + plan.substr(start, end - start));
		start = end + 1;
	}
	const int renames = files + existing;
	std::fprintf(stderr, "  %d files, %d renames\n", files, renames);

	int cuts = 0, undos = 0;
	for (int k = 0; k <= renames; k++)
	{
		CHECK(ToReady(work, app, zip).state == State::Ready);
		Test::stop_after = k;
		const bool done = Install();
		Test::stop_after = -1;
		CHECK(!done);
		CHECK(Exists(app + "/.update/journal"));
		// A few undos are themselves interrupted, at each of their steps.
		const bool nested = k == 1 || k == renames / 2 || k == renames;
		for (int j = 0; nested && j < 6 && Exists(app + "/.update/journal"); j++)
		{
			Test::undo_stop_after = j;
			CHECK(Recover(app));
			Test::undo_stop_after = -1;
			undos++;
		}
		// An undo that was let run to its end left no journal; one that was cut short did.
		const bool pending = Exists(app + "/.update/journal");
		CHECK(pending || nested);
		CHECK(Recover(app) == pending);
		CHECK(Unchanged(app, original));
		CHECK(!Recover(app)); // nothing left to do
		cuts++;
	}
	std::fprintf(stderr, "  %d power cuts and %d interrupted undos, each put back exactly\n", cuts, undos);

	for (int k = 0; k < renames; k++)
	{
		CHECK(ToReady(work, app, zip).state == State::Ready);
		Test::fail_at = k;
		CHECK(!Install());
		Test::fail_at = -1;
		const Status status = GetStatus();
		CHECK(status.state == State::Failed && Contains(status.error, "could not be"));
		CHECK(Unchanged(app, original));
	}
	for (int k = 0; k < renames; k++)
	{
		CHECK(ToReady(work, app, zip).state == State::Ready);
		Test::lie_at = k;
		CHECK(!Install());
		Test::lie_at = -1;
		CHECK(GetStatus().state == State::Failed);
		CHECK(Unchanged(app, original));
	}

	// What stands where a program file goes is a folder: nothing is begun.
	CHECK(ToReady(work, app, zip).state == State::Ready);
	unlink((app + "/symbols.txt").c_str());
	MakeDirs(app + "/symbols.txt");
	Put(app + "/symbols.txt/mine", "x");
	{
		const auto before = Snapshot(app, true);
		CHECK(!Install());
		CHECK(Contains(GetStatus().error, "not a file"));
		CHECK(Unchanged(app, before));
	}
	// The stage lost a file between Download() and Install().
	CHECK(ToReady(work, app, zip).state == State::Ready);
	unlink((app + "/.update/stage/licenses/GPL-3.0.txt").c_str());
	CHECK(!Install());
	CHECK(Contains(GetStatus().error, "incomplete"));
	CHECK(Unchanged(app, original));

	// What an earlier update left without a journal is removed at the start; nothing else moves.
	OldFolder(app);
	Put(app + "/.update/release.zip", "half");
	Put(app + "/.update/stage/eboot.bin", "new");
	CHECK(!Recover(app));
	CHECK(Unchanged(app, original));
	CHECK(!Recover(app));
	CHECK(!Recover(""));
	// A journal that names what is not the program's: those lines are passed over.
	OldFolder(app);
	Put(app + "/.update/journal", "inis/PCSX2.ini\n../outside\neboot.bin\n");
	Put(app + "/.update/backup/inis/PCSX2.ini", "an attacker's");
	CHECK(Recover(app));
	CHECK(Unchanged(app, original));
}

static void TestCancel(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "a download stopped half-way\n");
	const std::string zip = MakeZip(work, Package("1.2.0", 3000000));
	s_server.Clear();
	OldFolder(app);
	const auto before = Snapshot(app);
	const Rel rel = ServeRelease("1.2.0", zip);
	s_server.Serve(API, ReleasesJson({rel}));
	s_server.chunk = 16384;
	s_server.delay_ms = 3;
	Init(MakeSetup(app, "1.0.0"));
	Check();
	CHECK(Wait().state == State::Available);
	Download();
	Download(); // a second press while it runs: nothing more
	bool seen = false;
	for (int i = 0; i < 5000 && !seen; i++)
	{
		const Status status = GetStatus();
		seen = status.state == State::Downloading && status.done > 0 && status.done < status.total;
		if (!seen)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(seen);
	Check(); // ignored while downloading
	Cancel();
	const Status status = Wait();
	CHECK(status.state == State::Available);
	CHECK(status.done == 0);
	CHECK(!Exists(app + "/.update"));
	CHECK(Unchanged(app, before));
	// And then the whole of it.
	s_server.delay_ms = 0;
	Download();
	CHECK(Wait().state == State::Ready);
	CHECK(Install());
	s_server.chunk = 65536;
	(void)work;
}

static void TestFreeSpace(const std::string& work, const std::string& app)
{
	std::fprintf(stderr, "free space\n");
	const std::string zip = MakeZip(work, Package("1.2.0", 4000000));
	OldFolder(work + "/reference");
	const auto original = Snapshot(work + "/reference");
	for (int which = 0; which < 2; which++)
	{
		s_server.Clear();
		OldFolder(app);
		const Rel rel = ServeRelease("1.2.0", zip);
		s_server.Serve(API, ReleasesJson({rel}));
		Setup setup = MakeSetup(app, "1.0.0");
		// First too little for the download; then enough for it but not for what it unpacks to.
		const int64_t free = which == 0 ? (1 << 20) : static_cast<int64_t>(zip.size() + (32 << 20) + 1000);
		setup.free_space = [free](const std::string&) { return free; };
		Init(setup);
		Check();
		CHECK(Wait().state == State::Available);
		Download();
		const Status status = Wait();
		CHECK(status.state == State::Failed && Contains(status.error, "not enough free space"));
		CHECK(s_server.Asked(DOWNLOADS + "v1.2.0/Crux2-v1.2.0.zip") == (which == 1));
		CHECK(Unchanged(app, original));
	}
	// statvfs() on the real folder: plenty.
	s_server.Clear();
	CHECK(ToReady(work, app, zip).state == State::Ready);
}

// A real release ZIP of Crux2 (made by make-release-zip.py), installed over the older folder.
static void TestRealZip(const std::string& work, const std::string& app, const std::string& path)
{
	std::fprintf(stderr, "a real release: %s\n", path.c_str());
	const std::string zip = Get(path);
	CHECK(zip.size() > 1000000);
	int error = 0;
	zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
	CHECK(archive != nullptr);
	if (!archive)
		return;
	std::string eboot;
	zip_stat_t st;
	zip_stat_init(&st);
	if (zip_stat(archive, "PPSA99249/eboot.bin", 0, &st) == 0)
	{
		eboot.resize(static_cast<size_t>(st.size));
		zip_file_t* file = zip_fopen(archive, "PPSA99249/eboot.bin", 0);
		CHECK(file && zip_fread(file, eboot.data(), st.size) == static_cast<zip_int64_t>(st.size));
		if (file)
			zip_fclose(file);
	}
	zip_discard(archive);
	CHECK(!eboot.empty());

	const Status status = ToReady(work, app, zip);
	CHECK(status.state == State::Ready);
	if (status.state != State::Ready)
	{
		std::fprintf(stderr, "  said: %s\n", status.error.c_str());
		return;
	}
	OldFolder(work + "/reference");
	const auto original = Snapshot(work + "/reference");
	CHECK(Install());
	CHECK(Get(app + "/eboot.bin") == eboot);
	CHECK(Exists(app + "/sce_module/libc.prx") && Exists(app + "/resources/GameIndex.yaml"));
	CHECK(Exists(app + "/licenses/components.json") && Exists(app + "/sandbox-elevator.elf"));
	const auto after = Snapshot(app);
	for (const auto& [name, data] : original)
		if (!IsManaged(name) && name.back() != '/')
			CHECK(after.count(name) && after.at(name) == data);
	CHECK(!Exists(app + "/.update"));
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: update_test <work folder> [a release ZIP]\n");
		return 2;
	}
	const std::string work = argv[1];
	MakeDirs(work);
	const std::string app = work + "/PPSA99249";

	TestHelpers();
	TestCheck(work, app);
	TestInstall(work, app);
	TestChecksums(work, app);
	TestRefusedZips(work, app);
	TestInterrupted(work, app);
	TestCancel(work, app);
	TestFreeSpace(work, app);
	if (argc >= 3)
		TestRealZip(work, app, argv[2]);

	// The log: one line each, "update: ...", and no address of any kind in it.
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		CHECK(!s_log.empty());
		bool shaped = true, quiet = true;
		for (const std::string& line : s_log)
		{
			shaped = shaped && line.compare(0, 8, "update: ") == 0 && line.find('\n') == std::string::npos;
			quiet = quiet && !Contains(line, "http") && !Contains(line, "192.0.2.");
		}
		CHECK(shaped);
		CHECK(quiet);
	}

	if (s_failures != 0)
	{
		std::printf("update: %d checks, %d FAILED\n", s_checks, s_failures);
		return 1;
	}
	std::printf("update: %d checks passed\n", s_checks);
	return 0;
}

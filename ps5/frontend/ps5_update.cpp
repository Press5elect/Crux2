// Crux2 - the updater: a newer release, downloaded and put in place from inside the title.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Adapted from PSSwanStation's updater (ps5/src/update.cpp), by the same author; the way of working
// (download beside the title, check, unpack into a stage, rename with a journal, undo at the next
// start) is that one's, which was proven on a console.
//
// .update/ in the title folder holds, while it exists:
//
//     release.zip   the download
//     stage/        the new files, unpacked
//     plan          their paths, a line each, eboot.bin last
//     new           what Install() adds to the title folder: the files that had no older one, and
//                   the folders made for them (those end in '/')
//     journal       the plan again: present from the first rename to the last
//     backup/       the old files while the journal is there
//     displaced/    (only after a rare undo) a file that stood where an old one had to return
//
// Only the program's own files are ever unpacked or replaced (IsManaged). The ZIP's other entries
// are passed over, and a file on the disk that the release does not have is left where it is,
// also inside the program's folders.
//
// exFAT. The title folder can be on an exFAT USB drive, and there a rename onto a file that exists
// was measured to report success yet keep the old file. So no rename here is ever made onto
// anything: the old file is renamed away first, then the new one in, and afterwards the drive is
// asked whether the source is gone and the destination has the size the source had. fchmod() may
// be refused there too; that is not an error.
//
// Everything here is written without exceptions and without std::filesystem (the console's libc++
// has none): POSIX calls only.

#include "ps5_update.h"
#include "ps5_priority.h"

#include "rapidjson/document.h"

#include <zip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace PS5Update
{

#if defined(CRUX2_UPDATE_TEST)
namespace Test
{
int stop_after = -1;
int undo_stop_after = -1;
int fail_at = -1;
int lie_at = -1;
uint64_t max_unpacked = 0;
size_t max_entries = 0;
} // namespace Test
#endif

namespace
{

constexpr uint64_t MAX_ZIP_BYTES = 1ull << 30;
constexpr uint64_t MAX_UNPACKED_BYTES = 2ull << 30;
constexpr size_t MAX_ENTRIES = 20000;
constexpr size_t MAX_ANSWER_BYTES = 8u << 20;   // the releases page's JSON, 20 releases with their notes
constexpr size_t MAX_CHECKSUM_BYTES = 64u << 10; // a .sha256 file or SHA256SUMS
constexpr size_t MAX_NOTES_ONE = 6000;          // characters of one release's notes
constexpr size_t MAX_NOTES_ALL = 20000;         // characters of them all together
constexpr size_t MAX_PARAM_JSON = 1u << 20;
constexpr size_t MAX_COMPONENT = 255;
constexpr int MAX_TREE_DEPTH = 32;
constexpr uint64_t FREE_MARGIN = 32ull << 20;   // room left over on the drive after each step
// The console writes large blocks many times faster than small ones.
constexpr size_t WRITE_BLOCK = 1u << 20;
constexpr unsigned ANSWER_SECONDS = 20;
constexpr int RELEASES_ASKED = 20;

uint64_t MaxUnpacked()
{
#if defined(CRUX2_UPDATE_TEST)
	if (Test::max_unpacked != 0)
		return Test::max_unpacked;
#endif
	return MAX_UNPACKED_BYTES;
}

size_t MaxEntries()
{
#if defined(CRUX2_UPDATE_TEST)
	if (Test::max_entries != 0)
		return Test::max_entries;
#endif
	return MAX_ENTRIES;
}

// The program's files: these folders whole, and these files of the title folder itself. Nothing
// else is unpacked, replaced or put back. They are what ps5/tools/link-title.sh puts into the
// title folder: eboot.bin, sce_sys/ (param.json and the pictures), sce_module/ (libc.prx),
// resources/ (PCSX2's and the port's), sandbox-elevator.elf (the USB helper), symbols.txt (the
// profiler's names, which belong to one eboot.bin), README.md, LEGAL.txt and licenses/. A README
// or a CHANGELOG in another form is allowed for, should a later package carry one.
constexpr const char* MANAGED_FOLDERS[] = {"sce_sys/", "sce_module/", "resources/", "licenses/"};
constexpr const char* MANAGED_FILES[] = {"eboot.bin", "sandbox-elevator.elf", "symbols.txt", "LEGAL.txt", "README.md",
	"README.txt", "CHANGELOG.md", "CHANGELOG.txt"};
// Inside those folders, but the user's: PCSX2's patch collection, which the title fetches on
// request (Settings > Interface > PlayStation 5 > Download PCSX2's patches) or the user copies in;
// the package never carries it. Compared without regard to case, as exFAT names are.
constexpr const char* USER_FILES_INSIDE[] = {"resources/patches.zip"};

// ------------------------------------------------------------------------------------- text

std::string Lowercase(std::string text)
{
	for (char& c : text)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

std::string Trim(const std::string& text)
{
	size_t start = 0, end = text.size();
	while (start < end && std::isspace(static_cast<unsigned char>(text[start])))
		start++;
	while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
		end--;
	return text.substr(start, end - start);
}

bool StartsWith(const std::string& text, const char* start)
{
	return text.compare(0, std::strlen(start), start) == 0;
}

bool EndsWith(const std::string& text, const char* ending)
{
	const size_t length = std::strlen(ending);
	return text.size() >= length && text.compare(text.size() - length, length, ending) == 0;
}

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

void Say(const Log& log, const char* format, ...) __attribute__((format(printf, 2, 3)));
void Say(const Log& log, const char* format, ...)
{
	if (!log)
		return;
	char buffer[768];
	std::memcpy(buffer, "update: ", 8);
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer + 8, sizeof(buffer) - 8, format, args);
	va_end(args);
	// One line: no line breaks from a name or an error inside it.
	for (char* c = buffer; *c; c++)
		if (*c == '\n' || *c == '\r')
			*c = ' ';
	log(buffer);
}

std::string MiB(uint64_t bytes)
{
	return Format("%.1f MiB", static_cast<double>(bytes) / 1048576.0);
}

// At most `most` characters of UTF-8 text (not bytes): a cut never lands inside a character.
std::string CutCharacters(const std::string& text, size_t most, const char* ending)
{
	size_t characters = 0, keep = 0;
	const size_t room = most - std::min(most, std::strlen(ending));
	for (size_t at = 0; at < text.size(); at++)
	{
		if ((static_cast<unsigned char>(text[at]) & 0xc0) == 0x80)
			continue;
		if (characters == room)
			keep = at;
		if (characters == most)
			return text.substr(0, keep) + ending;
		characters++;
	}
	return text;
}

// One line for the screen out of a release's name.
std::string OneLine(const std::string& text)
{
	std::string out;
	for (const unsigned char c : text)
		out += c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
	return CutCharacters(Trim(out), 100, "...");
}

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// ------------------------------------------------------------------------------------ paths

// What is wrong with a path that is to be added to a folder, or nullptr: it must stay below that
// folder (not absolute, no empty, "." or ".." part), and have no backslash, no control character
// and no part longer than a file name may be. A folder's entry in a ZIP ends in '/', which
// `folder` allows.
const char* PathFault(const std::string& path, bool folder = false)
{
	if (path.empty())
		return "has no name";
	if (path[0] == '/')
		return "is an absolute path";
	for (const unsigned char c : path)
		if (c < 0x20 || c == 0x7f || c == '\\')
			return "has a backslash or a control character in its name";
	if (path.size() >= 2 && path[1] == ':')
		return "is an absolute path";
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		const bool last = end == path.size();
		if (part.empty())
		{
			// Only as what follows a folder's closing '/'.
			if (!(folder && last && start > 0))
				return "has an empty part in its path";
		}
		else if (part == "." || part == "..")
			return "has \"..\" or \".\" in its path";
		else if (part.size() > MAX_COMPONENT)
			return "has a name that is too long";
		start = end + 1;
	}
	return nullptr;
}

std::string WithSlash(std::string dir)
{
	if (!dir.empty() && dir.back() != '/')
		dir += '/';
	return dir;
}

std::string ParentOf(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? "" : path.substr(0, slash);
}

// Whether the path is a link. open() with O_NOFOLLOW refuses a link (ELOOP, or EMLINK as FreeBSD
// has it) whatever lstat() does; lstat() is asked only when open() cannot tell. (On the console,
// PSSwanStation's updater that asked lstat() alone found files it had just written "not there".)
bool IsLink(const std::string& path)
{
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0)
	{
		close(fd);
		return false;
	}
	if (errno == ELOOP || errno == EMLINK)
		return true;
	struct stat own;
	return lstat(path.c_str(), &own) == 0 && S_ISLNK(own.st_mode);
}

// What is at a path, a link itself and not what it points to.
bool LookAt(const std::string& path, struct stat& st)
{
	if (IsLink(path))
	{
		std::memset(&st, 0, sizeof(st));
		st.st_mode = S_IFLNK | 0777;
		return true;
	}
	return stat(path.c_str(), &st) == 0;
}

bool PathExists(const std::string& path)
{
	struct stat st;
	return LookAt(path, st);
}

bool IsRegularFile(const std::string& path)
{
	struct stat st;
	return LookAt(path, st) && S_ISREG(st.st_mode);
}

bool DirExists(const std::string& path)
{
	struct stat st;
	return LookAt(path, st) && S_ISDIR(st.st_mode);
}

// A folder and those above it.
void MakeDirs(const std::string& path)
{
	if (path.empty())
		return;
	std::string at;
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		at = path.substr(0, end);
		if (!at.empty() && !DirExists(at))
		{
			if (mkdir(at.c_str(), 0777) == 0)
				chmod(at.c_str(), 0777); // may be refused on exFAT: nothing is lost by it
		}
		start = end + 1;
	}
}

// A folder and all in it. Links are removed, never followed.
void RemoveTree(std::string path, int depth = 0)
{
	while (path.size() > 1 && path.back() == '/')
		path.pop_back();
	struct stat st;
	if (path.empty() || !LookAt(path, st))
		return;
	if (!S_ISDIR(st.st_mode))
	{
		unlink(path.c_str());
		return;
	}
	if (depth < MAX_TREE_DEPTH)
	{
		std::vector<std::string> names;
		if (DIR* list = opendir(path.c_str()))
		{
			while (const dirent* entry = readdir(list))
			{
				const std::string name = entry->d_name;
				if (name != "." && name != "..")
					names.push_back(name);
			}
			closedir(list);
		}
		for (const std::string& name : names)
			RemoveTree(path + "/" + name, depth + 1);
	}
	rmdir(path.c_str());
}

bool WriteAll(int fd, const void* data, size_t bytes)
{
	const char* at = static_cast<const char*>(data);
	while (bytes > 0)
	{
		const ssize_t written = write(fd, at, bytes);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		at += written;
		bytes -= static_cast<size_t>(written);
	}
	return true;
}

bool ReadFile(const std::string& path, std::string& data, size_t most)
{
	data.clear();
	const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	char buffer[16384];
	bool ok = true;
	for (;;)
	{
		const ssize_t got = read(fd, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0 || data.size() + static_cast<size_t>(std::max<ssize_t>(got, 0)) > most)
			ok = false;
		if (got <= 0 || !ok)
			break;
		data.append(buffer, static_cast<size_t>(got));
	}
	close(fd);
	return ok;
}

// Asks for a folder's entries (a rename, a new file, a removed one) to be on the disk before what
// follows is done. Where a folder cannot be opened or synchronised nothing is lost by it: the
// order of the steps stays the same.
void SyncDir(std::string path)
{
	while (path.size() > 1 && path.back() == '/')
		path.pop_back();
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return;
	fsync(fd);
	close(fd);
}

// A small file that is whole on the disk, or not there: written beside its place, synchronised,
// then renamed (onto nothing: whatever had the name is removed first, see exFAT above).
bool WriteDurable(const std::string& path, const std::string& text)
{
	const std::string temporary = path + ".tmp";
	unlink(temporary.c_str());
	const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0)
		return false;
	const bool written = WriteAll(fd, text.data(), text.size());
	const bool synced = fsync(fd) == 0;
	const bool closed = close(fd) == 0;
	if (written && synced && closed)
	{
		unlink(path.c_str());
		if (!PathExists(path) && rename(temporary.c_str(), path.c_str()) == 0 && !PathExists(temporary))
		{
			struct stat st;
			if (stat(path.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) == text.size())
			{
				chmod(path.c_str(), 0666);
				return true;
			}
		}
	}
	unlink(temporary.c_str());
	return false;
}

// The lines of a list file (plan, journal, new), empty ones left out.
bool ReadLines(const std::string& path, std::vector<std::string>& lines)
{
	lines.clear();
	std::string data;
	if (!IsRegularFile(path) || !ReadFile(path, data, 16u << 20))
		return false;
	size_t start = 0;
	while (start < data.size())
	{
		size_t end = data.find('\n', start);
		if (end == std::string::npos)
			end = data.size();
		std::string line = data.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty())
			lines.push_back(line);
		start = end + 1;
	}
	return true;
}

int64_t FreeBytes(const FreeSpace& asked, const std::string& folder)
{
	if (asked)
		return asked(folder);
	struct statvfs vfs;
	if (statvfs(folder.c_str(), &vfs) != 0)
		return -1;
	const unsigned long long bytes = static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
	return bytes > static_cast<unsigned long long>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(bytes);
}

// ---------------------------------------------------------------------------------- SHA-256

class Sha256
{
public:
	void Add(const uint8_t* data, size_t bytes)
	{
		m_total += bytes;
		while (bytes > 0)
		{
			const size_t take = std::min(bytes, sizeof(m_block) - m_filled);
			std::memcpy(m_block + m_filled, data, take);
			m_filled += take;
			data += take;
			bytes -= take;
			if (m_filled == sizeof(m_block))
			{
				Transform();
				m_filled = 0;
			}
		}
	}

	// The digest as 64 lower-case hex digits. Once only.
	std::string Finish()
	{
		const uint64_t bits = m_total * 8;
		const uint8_t one = 0x80, zero = 0;
		Add(&one, 1);
		while (m_filled != 56)
			Add(&zero, 1);
		uint8_t length[8];
		for (int i = 0; i < 8; i++)
			length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
		Add(length, 8);
		static const char hex[] = "0123456789abcdef";
		std::string out;
		for (const uint32_t word : m_state)
			for (int shift = 28; shift >= 0; shift -= 4)
				out += hex[(word >> shift) & 15];
		return out;
	}

private:
	static uint32_t Rotate(uint32_t value, int by) { return (value >> by) | (value << (32 - by)); }

	void Transform()
	{
		static const uint32_t K[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = static_cast<uint32_t>(m_block[i * 4]) << 24 | static_cast<uint32_t>(m_block[i * 4 + 1]) << 16 |
			       static_cast<uint32_t>(m_block[i * 4 + 2]) << 8 | static_cast<uint32_t>(m_block[i * 4 + 3]);
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
		uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
			const uint32_t choose = (e & f) ^ (~e & g);
			const uint32_t t1 = h + s1 + choose + K[i] + w[i];
			const uint32_t s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
			const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = s0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		m_state[0] += a;
		m_state[1] += b;
		m_state[2] += c;
		m_state[3] += d;
		m_state[4] += e;
		m_state[5] += f;
		m_state[6] += g;
		m_state[7] += h;
	}

	uint32_t m_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t m_block[64] = {};
	size_t m_filled = 0;
	uint64_t m_total = 0;
};

bool IsHex64(const std::string& text, size_t at)
{
	if (text.size() < at + 64)
		return false;
	for (size_t i = at; i < at + 64; i++)
		if (!std::isxdigit(static_cast<unsigned char>(text[i])))
			return false;
	return true;
}

// A .sha256 file: 64 hex digits, alone or followed by white space and the file's name (what
// sha256sum writes). Or SHA256SUMS: such lines for several files, of which `name`'s is wanted.
// Empty when there is none that fits.
std::string ChecksumFor(const std::vector<uint8_t>& data, const std::string& name, bool single)
{
	std::string text(data.begin(), data.end());
	if (StartsWith(text, "\xef\xbb\xbf"))
		text.erase(0, 3);
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = Trim(text.substr(start, end - start));
		start = end + 1;
		if (!IsHex64(line, 0) || (line.size() > 64 && !std::isspace(static_cast<unsigned char>(line[64]))))
			continue;
		std::string named = Trim(line.substr(64));
		if (!named.empty() && named[0] == '*') // sha256sum's binary mode
			named.erase(0, 1);
		if (named == name || (single && named.empty()))
			return Lowercase(line.substr(0, 64));
	}
	return "";
}

// ------------------------------------------------------------------------------------- JSON

std::string JsonString(const rapidjson::Value& object, const char* key)
{
	if (!object.IsObject())
		return "";
	const auto member = object.FindMember(key);
	if (member == object.MemberEnd() || !member->value.IsString())
		return "";
	return std::string(member->value.GetString(), member->value.GetStringLength());
}

bool JsonBool(const rapidjson::Value& object, const char* key)
{
	if (!object.IsObject())
		return false;
	const auto member = object.FindMember(key);
	return member != object.MemberEnd() && member->value.IsBool() && member->value.GetBool();
}

bool JsonSize(const rapidjson::Value& object, const char* key, uint64_t& out)
{
	if (!object.IsObject())
		return false;
	const auto member = object.FindMember(key);
	if (member == object.MemberEnd() || !member->value.IsUint64())
		return false;
	out = member->value.GetUint64();
	return true;
}

// ----------------------------------------------------------------------------- the releases

struct Release
{
	Version version;
	std::string tag, name, date, body;
	bool prerelease = false;
	std::string zip_name, zip_url;
	std::string checksum_url; // "<ZIP>.sha256", or empty
	std::string sums_url;     // SHA256SUMS, or empty
	std::string digest;       // the SHA-256 GitHub itself computed, or empty
	uint64_t zip_size = 0;
	bool has_size = false;

	bool HasZip() const { return !zip_url.empty(); }
};

bool IsHttps(const std::string& url)
{
	return StartsWith(url, "https://") && url.size() > 8;
}

// The title's ZIP among a release's assets: "<prefix><tag>.zip" first, else a "<prefix>...zip"
// whose name has the release's version and is not a part or the source.
void FindAssets(const rapidjson::Value& assets, const Setup& use, Release& release)
{
	if (!assets.IsArray())
		return;
	const std::string exact = use.asset_prefix + release.tag + ".zip";
	const std::string prefix = Lowercase(use.asset_prefix);
	const rapidjson::Value* chosen = nullptr;
	for (const rapidjson::Value& asset : assets.GetArray())
	{
		const std::string name = JsonString(asset, "name");
		const std::string lower = Lowercase(name);
		if (!IsHttps(JsonString(asset, "browser_download_url")))
			continue;
		if (name == exact)
		{
			chosen = &asset;
			break;
		}
		if (chosen == nullptr && StartsWith(lower, prefix.c_str()) && EndsWith(lower, ".zip") &&
			lower.find("-source") == std::string::npos && lower.find("-part") == std::string::npos &&
			CompareVersions(VersionIn(name.substr(prefix.size())), release.version) == 0)
			chosen = &asset;
	}
	if (chosen == nullptr)
		return;
	release.zip_name = JsonString(*chosen, "name");
	release.zip_url = JsonString(*chosen, "browser_download_url");
	release.has_size = JsonSize(*chosen, "size", release.zip_size);
	// "sha256:<64 hex digits>": GitHub's own, on files uploaded since 2025.
	const std::string digest = Lowercase(JsonString(*chosen, "digest"));
	if (digest.size() == 71 && StartsWith(digest, "sha256:") && IsHex64(digest, 7))
		release.digest = digest.substr(7);
	for (const rapidjson::Value& asset : assets.GetArray())
	{
		const std::string name = JsonString(asset, "name");
		const std::string url = JsonString(asset, "browser_download_url");
		if (!IsHttps(url))
			continue;
		if (name == release.zip_name + ".sha256")
			release.checksum_url = url;
		else if (name == "SHA256SUMS" || name == "SHA256SUMS.txt")
			release.sums_url = url;
	}
}

// ------------------------------------------------------------------------------------ state

// g_state_mutex: what GetStatus() copies, and the set-up. Held for moments only.
std::mutex g_state_mutex;
Status g_current;
Setup g_setup;
Release g_found;
bool g_initialised = false;

// g_call_mutex: one of Init(), Check(), Download(), Install() and Recover() at a time. A state in
// which the worker runs (Checking, Downloading, Verifying) is entered only with it held, and left
// only by the worker.
std::mutex g_call_mutex;
std::atomic<bool> g_stop{false};

// The one worker thread, for a check or a download. It is joined before the next one starts and
// when the program ends. Declared after everything it uses, so that it ends before any of that.
struct Worker
{
	std::thread thread;
	void Join()
	{
		if (thread.joinable())
			thread.join();
	}
	~Worker()
	{
		g_stop = true;
		Join();
	}
};
Worker g_worker;

void Finish(const Log& log, State state, const std::string& error = "")
{
	if (state == State::Failed)
		Say(log, "failed: %s", error.c_str());
	std::lock_guard<std::mutex> lock(g_state_mutex);
	g_current.state = state;
	g_current.error = error;
	g_current.speed = 0;
	if (state != State::Ready && state != State::Installed)
		g_current.done = g_current.total = 0;
}

template <typename Function>
void StartWorker(Function&& function)
{
	g_worker.Join();
	g_worker.thread = std::thread([function = std::forward<Function>(function)]() mutable {
		PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
		function();
	});
}

// ------------------------------------------------------------------------------------ check

bool ValidRepository(const std::string& repository)
{
	const size_t slash = repository.find('/');
	if (slash == std::string::npos || slash == 0 || slash + 1 >= repository.size() ||
		repository.find('/', slash + 1) != std::string::npos || repository.size() > 200)
		return false;
	for (const char c : repository)
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/'))
			return false;
	return repository.find("..") == std::string::npos;
}

std::string NotesHeader(const Release& release)
{
	const std::string version = ToString(release.version);
	std::string header = OneLine(release.name);
	if (header.empty())
		header = "v" + version;
	else if (header.find(version) == std::string::npos)
		header = "v" + version + " - " + header;
	if (!release.date.empty())
		header += " (" + release.date + ")";
	return header;
}

void RunCheck(const Setup& use)
{
	const Log& log = use.log;
	if (!ValidRepository(use.repository))
	{
		Finish(log, State::Failed, "The repository to ask for updates is not set.");
		return;
	}
	const std::string url = use.api_base + "/repos/" + use.repository + "/releases?per_page=" +
	                        std::to_string(RELEASES_ASKED);
	Say(log, "asking GitHub for %s's releases", use.repository.c_str());
	std::vector<uint8_t> answer;
	const int code = use.http_get ? use.http_get(url, answer, ANSWER_SECONDS) : -1;
	if (code == 404)
	{
		Say(log, "the repository was not found");
		Finish(log, State::NoRelease, "The repository " + use.repository + " was not found on GitHub.");
		return;
	}
	if (code == 403 || code == 429)
	{
		Finish(log, State::Failed,
			"GitHub refused the question: it answers only so many an hour from one network. Try again later.");
		return;
	}
	if (code != 200)
	{
		Finish(log, State::Failed,
			code < 0 ? "GitHub could not be reached. Is the console online?" : Format("GitHub answered %d.", code));
		return;
	}
	if (answer.size() > MAX_ANSWER_BYTES)
	{
		Finish(log, State::Failed, "GitHub's answer is too long.");
		return;
	}
	rapidjson::Document page;
	page.Parse<rapidjson::kParseIterativeFlag>(reinterpret_cast<const char*>(answer.data()), answer.size());
	if (page.HasParseError() || !page.IsArray())
	{
		Finish(log, State::Failed, "GitHub's answer could not be read.");
		return;
	}

	// Every release that is not a draft and has a version in its tag (or, failing that, its name).
	// Pre-releases count: Crux2's releases are pre-releases.
	std::vector<Release> releases;
	for (const rapidjson::Value& item : page.GetArray())
	{
		if (!item.IsObject() || JsonBool(item, "draft"))
			continue;
		Release release;
		release.tag = JsonString(item, "tag_name");
		release.name = JsonString(item, "name");
		release.version = VersionIn(release.tag);
		if (!release.version.valid)
			release.version = VersionIn(release.name);
		if (!release.version.valid)
			continue;
		release.body = JsonString(item, "body");
		release.prerelease = JsonBool(item, "prerelease");
		const std::string published = JsonString(item, "published_at");
		if (published.size() >= 10 && published[4] == '-' && published[7] == '-')
			release.date = published.substr(0, 10);
		const auto assets = item.FindMember("assets");
		if (assets != item.MemberEnd())
			FindAssets(assets->value, use, release);
		releases.push_back(std::move(release));
	}
	if (releases.empty())
	{
		Say(log, "the repository has no release yet");
		Finish(log, State::NoRelease, "There is no release of Crux2 yet.");
		return;
	}
	// The newest first; GitHub's own order (newest first) between equals.
	std::stable_sort(releases.begin(), releases.end(),
		[](const Release& a, const Release& b) { return CompareVersions(a.version, b.version) > 0; });
	const Release& newest = releases.front();
	const Version running = ParseVersion(use.version);
	if (!running.valid)
		Say(log, "the running version \"%s\" cannot be read; every release counts as newer", use.version.c_str());

	Status status;
	status.version = ToString(newest.version);
	status.tag = newest.tag;
	status.name = OneLine(newest.name);
	if (status.name.empty())
		status.name = "Crux2 v" + status.version;
	for (const Release& release : releases)
	{
		if (CompareVersions(release.version, running) <= 0)
			break;
		ReleaseNotes notes;
		notes.version = ToString(release.version);
		notes.tag = release.tag;
		notes.name = OneLine(release.name);
		notes.date = release.date;
		notes.prerelease = release.prerelease;
		notes.text = CutCharacters(PlainText(release.body), MAX_NOTES_ONE, "...");
		if (!status.notes.empty())
			status.notes += "\n\n\n";
		status.notes += NotesHeader(release);
		if (!notes.text.empty())
			status.notes += "\n\n" + notes.text;
		status.newer.push_back(std::move(notes));
	}
	status.notes = CutCharacters(status.notes, MAX_NOTES_ALL, "\n...");

	if (CompareVersions(newest.version, running) <= 0)
	{
		Say(log, "v%s is running, the newest release is v%s", ToString(running).c_str(), status.version.c_str());
		status.state = State::UpToDate;
		// It can be put in place again (Install again): a damaged folder mended.
		status.reinstall = newest.HasZip();
	}
	else if (!newest.HasZip())
	{
		// A release whose files are not there (yet).
		const std::string expected = use.asset_prefix + newest.tag + ".zip";
		Say(log, "v%s has no %s", status.version.c_str(), expected.c_str());
		status.state = State::NoRelease;
		status.error = "Version " + status.version + " is out, but its release has no " + expected +
		               " (yet). Try again later.";
	}
	else
	{
		status.state = State::Available;
		Say(log, "v%s is available (%s, %llu bytes%s; %zu releases newer than v%s)", status.version.c_str(),
			newest.zip_name.c_str(), static_cast<unsigned long long>(newest.zip_size),
			newest.checksum_url.empty() && newest.sums_url.empty() && newest.digest.empty() ? "" : ", with a checksum",
			status.newer.size(), ToString(running).c_str());
	}
	std::lock_guard<std::mutex> lock(g_state_mutex);
	g_found = newest;
	g_current = std::move(status);
}

// ---------------------------------------------------------------------------------- the ZIP

struct Entry
{
	zip_uint64_t index;
	std::string path; // below the title folder
	uint64_t size;
};

// Why a ZIP is refused, for the screen and the log.
std::string Refusal(const std::string& name, const char* why)
{
	std::string shown;
	for (const unsigned char c : name)
		shown += c < 0x20 || c == 0x7f ? '?' : static_cast<char>(c);
	return "The release's ZIP was refused: \"" + CutCharacters(shown, 80, "...") + "\" " + why + ".";
}

// Looks at every entry before anything is unpacked. All of them must be below the one folder
// named after the title; none may be a link or a special file, be encrypted, or have a name that
// leads elsewhere (or that, without regard to case as exFAT has it, is another entry's). Gives
// the program's files (eboot.bin last) or the reason the archive is refused.
std::string Examine(zip_t* zip, const std::string& title_id, std::vector<Entry>& files, uint64_t& managed_bytes,
	size_t& passed_over)
{
	files.clear();
	managed_bytes = 0;
	passed_over = 0;
	const zip_int64_t count = zip_get_num_entries(zip, 0);
	if (count <= 0 || static_cast<uint64_t>(count) > MaxEntries())
		return "The release's ZIP was refused: it has no files, or too many.";
	const std::string top = title_id + "/";
	std::set<std::string> names;
	uint64_t unpacked = 0;
	bool eboot = false, param = false;
	for (zip_int64_t i = 0; i < count; i++)
	{
		const zip_uint64_t index = static_cast<zip_uint64_t>(i);
		zip_stat_t st;
		zip_stat_init(&st);
		if (zip_stat_index(zip, index, 0, &st) != 0 || !(st.valid & ZIP_STAT_NAME) || !(st.valid & ZIP_STAT_SIZE) ||
			st.name == nullptr)
			return "The release's ZIP was refused: its list of files is damaged.";
		const std::string name = st.name;
		const bool folder = !name.empty() && name.back() == '/';
		if (const char* fault = PathFault(name, folder))
			return Refusal(name, fault);
		if (name != top && !StartsWith(name, top.c_str()))
			return Refusal(name, "is outside the title's folder");
		// The kind of file, where the archive records a Unix mode: only plain files and folders,
		// so no link can send a later entry elsewhere.
		zip_uint8_t system = 0;
		zip_uint32_t attributes = 0;
		if (zip_file_get_external_attributes(zip, index, 0, &system, &attributes) == 0 && system == ZIP_OPSYS_UNIX)
		{
			const unsigned kind = (attributes >> 16) & 0170000;
			if (kind != 0 && kind != 0100000 && kind != 0040000)
				return Refusal(name, kind == 0120000 ? "is a symbolic link" : "is not a plain file");
			if (kind == 0040000 && !folder)
				return Refusal(name, "is a folder without a folder's name");
		}
		if (!names.insert(Lowercase(folder ? name.substr(0, name.size() - 1) : name)).second)
			return Refusal(name, "is there twice (letters in another case count as the same)");
		if (st.size > MaxUnpacked() || (unpacked += st.size) > MaxUnpacked())
			return Format("The release's ZIP was refused: it unpacks to more than %s.", MiB(MaxUnpacked()).c_str());
		if (folder)
			continue;
		const std::string path = name.substr(top.size());
		if (!IsManaged(path))
		{
			passed_over++;
			continue;
		}
		if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) && st.encryption_method != ZIP_EM_NONE)
			return Refusal(name, "is encrypted");
		if (!(st.valid & ZIP_STAT_COMP_METHOD) || !zip_compression_method_supported(st.comp_method, 0))
			return Refusal(name, "is packed in a way that cannot be read");
		if (path == "eboot.bin")
			eboot = true;
		if (path == "sce_sys/param.json")
			param = true;
		files.push_back({index, path, st.size});
		managed_bytes += st.size;
	}
	if (!eboot)
		return "The release's ZIP was refused: it has no eboot.bin.";
	if (!param)
		return "The release's ZIP was refused: it has no sce_sys/param.json.";
	// A file where another file's folder would have to be.
	std::set<std::string> paths;
	for (const Entry& file : files)
		paths.insert(Lowercase(file.path));
	for (const Entry& file : files)
		for (std::string above = ParentOf(file.path); !above.empty(); above = ParentOf(above))
			if (paths.count(Lowercase(above)) != 0)
				return Refusal(top + file.path, "is below something that is a file");
	std::stable_partition(files.begin(), files.end(), [](const Entry& file) { return file.path != "eboot.bin"; });
	return "";
}

std::string ZipOpenError(int code)
{
	zip_error_t error;
	zip_error_init_with_code(&error, code);
	const std::string text = zip_error_strerror(&error);
	zip_error_fini(&error);
	return text;
}

// One entry into memory, no more than `most` bytes.
bool ReadEntry(zip_t* zip, const Entry& entry, size_t most, std::string& out)
{
	out.clear();
	if (entry.size > most)
		return false;
	zip_file_t* file = zip_fopen_index(zip, entry.index, 0);
	if (file == nullptr)
		return false;
	out.resize(static_cast<size_t>(entry.size));
	const zip_int64_t got = entry.size == 0 ? 0 : zip_fread(file, out.data(), entry.size);
	char extra;
	const bool ok = got == static_cast<zip_int64_t>(entry.size) && zip_fread(file, &extra, 1) == 0;
	return zip_fclose(file) == 0 && ok;
}

// Whether a param.json is this title's.
bool NamesTitle(const std::string& json, const std::string& title_id)
{
	rapidjson::Document document;
	document.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
	return !document.HasParseError() && document.IsObject() && JsonString(document, "titleId") == title_id;
}

enum class Outcome
{
	Done,
	Stopped,
	Failed
};

// Unpacks one file to its place in the stage, mode 0777: the console does not start an eboot.bin
// with less, and the FTP server is another process. (fchmod may be refused on exFAT; the drive's
// own modes are open to all there.)
Outcome Unpack(zip_t* zip, const Entry& entry, const std::string& path, std::vector<uint8_t>& buffer,
	std::string& error)
{
	MakeDirs(ParentOf(path));
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0777);
	if (fd < 0)
	{
		error = Format("%s could not be unpacked: the folder cannot be written (errno %d).", entry.path.c_str(), errno);
		return Outcome::Failed;
	}
	zip_file_t* file = zip_fopen_index(zip, entry.index, 0);
	if (file == nullptr)
	{
		close(fd);
		error = Format("%s could not be unpacked (%s).", entry.path.c_str(), zip_strerror(zip));
		return Outcome::Failed;
	}
	uint64_t written = 0;
	Outcome outcome = Outcome::Done;
	for (;;)
	{
		if (g_stop)
		{
			outcome = Outcome::Stopped;
			break;
		}
		const zip_int64_t got = zip_fread(file, buffer.data(), buffer.size());
		if (got < 0)
		{
			error = Format("%s could not be unpacked (%s).", entry.path.c_str(), zip_file_strerror(file));
			outcome = Outcome::Failed;
			break;
		}
		if (got == 0)
			break;
		if (written + static_cast<uint64_t>(got) > entry.size)
		{
			error = Format("%s is longer than the ZIP's list says.", entry.path.c_str());
			outcome = Outcome::Failed;
			break;
		}
		if (!WriteAll(fd, buffer.data(), static_cast<size_t>(got)))
		{
			error = Format("%s could not be unpacked: the drive is full or cannot be written.", entry.path.c_str());
			outcome = Outcome::Failed;
			break;
		}
		written += static_cast<uint64_t>(got);
	}
	if (zip_fclose(file) != 0 && outcome == Outcome::Done)
	{
		error = Format("%s is damaged in the ZIP.", entry.path.c_str());
		outcome = Outcome::Failed;
	}
	if (outcome == Outcome::Done && written != entry.size)
	{
		error = Format("%s is shorter than the ZIP's list says.", entry.path.c_str());
		outcome = Outcome::Failed;
	}
	fchmod(fd, 0777); // refused on exFAT: not an error
	// On the disk before it is renamed into the title folder.
	if (fsync(fd) != 0 && outcome == Outcome::Done)
	{
		error = Format("%s could not be written to the drive.", entry.path.c_str());
		outcome = Outcome::Failed;
	}
	if (close(fd) != 0 && outcome == Outcome::Done)
	{
		error = Format("%s could not be written to the drive.", entry.path.c_str());
		outcome = Outcome::Failed;
	}
	return outcome;
}

// The SHA-256 of a file, read in blocks. False when it cannot be read, or the worker was told to
// stop.
bool HashFile(const std::string& path, std::string& hex)
{
	const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	std::vector<uint8_t> buffer(WRITE_BLOCK);
	Sha256 sha;
	bool ok = true;
	for (;;)
	{
		const ssize_t got = read(fd, buffer.data(), buffer.size());
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0 || g_stop)
			ok = false;
		if (got <= 0 || !ok)
			break;
		sha.Add(buffer.data(), static_cast<size_t>(got));
	}
	close(fd);
	if (ok)
		hex = sha.Finish();
	return ok;
}

// ----------------------------------------------------------------------------------- download

// The steps after the download: size, checksum, the archive, the stage.
Outcome VerifyAndUnpack(const Setup& use, const Release& release, const std::string& dir, std::string& error)
{
	const Log& log = use.log;
	const std::string zip_path = dir + "release.zip";
	struct stat st;
	if (stat(zip_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		error = "The downloaded file is not there.";
		return Outcome::Failed;
	}
	const uint64_t size = static_cast<uint64_t>(st.st_size);
	if (size > MAX_ZIP_BYTES || (release.has_size && size != release.zip_size))
	{
		error = Format("The download is %llu bytes; the release says %llu.", static_cast<unsigned long long>(size),
			static_cast<unsigned long long>(release.zip_size));
		return Outcome::Failed;
	}

	// Up to three checksums may come with a release: GitHub's digest of the asset, a .sha256 file
	// beside the ZIP, and SHA256SUMS. Each one that is there must be the file's.
	std::vector<std::pair<std::string, const char*>> wanted;
	if (!release.digest.empty())
		wanted.push_back({release.digest, "GitHub's digest"});
	const std::pair<const std::string*, bool> files[] = {{&release.checksum_url, true}, {&release.sums_url, false}};
	for (const auto& [url, single] : files)
	{
		if (url->empty())
			continue;
		std::vector<uint8_t> answer;
		const int code = use.http_get ? use.http_get(*url, answer, ANSWER_SECONDS) : -1;
		if (g_stop)
			return Outcome::Stopped;
		if (code != 200 || answer.size() > MAX_CHECKSUM_BYTES)
		{
			error = "The release's checksum could not be read.";
			return Outcome::Failed;
		}
		const std::string listed = ChecksumFor(answer, release.zip_name, single);
		if (listed.empty())
		{
			if (single)
			{
				error = "The release's checksum file could not be read.";
				return Outcome::Failed;
			}
			Say(log, "SHA256SUMS has no line for %s", release.zip_name.c_str());
			continue;
		}
		wanted.push_back({listed, single ? "the .sha256 file" : "SHA256SUMS"});
	}
	for (size_t i = 1; i < wanted.size(); i++)
		if (wanted[i].first != wanted[0].first)
		{
			Say(log, "%s and %s differ", wanted[0].second, wanted[i].second);
			error = "The release's checksums differ from each other.";
			return Outcome::Failed;
		}
	if (!wanted.empty())
	{
		std::string actual;
		if (!HashFile(zip_path, actual))
		{
			if (g_stop)
				return Outcome::Stopped;
			error = "The downloaded file could not be read.";
			return Outcome::Failed;
		}
		if (actual != wanted[0].first)
		{
			Say(log, "SHA-256 %s, the release says %s", actual.c_str(), wanted[0].first.c_str());
			error = "The download is damaged: its checksum is not the release's.";
			return Outcome::Failed;
		}
		Say(log, "the checksum matches (%s, %zu sources)", actual.c_str(), wanted.size());
	}
	else
		Say(log, "the release has no checksum; the ZIP's own checks are used");

	int code = 0;
	zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &code);
	if (zip == nullptr)
	{
		error = "The download is not a ZIP that can be read (" + ZipOpenError(code) + ").";
		return Outcome::Failed;
	}

	Outcome outcome = Outcome::Done;
	std::vector<Entry> entries;
	uint64_t managed_bytes = 0;
	size_t passed_over = 0;
	error = Examine(zip, use.title_id, entries, managed_bytes, passed_over);
	if (error.empty())
	{
		std::string param;
		for (const Entry& entry : entries)
			if (entry.path == "sce_sys/param.json" && !ReadEntry(zip, entry, MAX_PARAM_JSON, param))
				param.clear();
		if (!NamesTitle(param, use.title_id))
			error = "The release's ZIP was refused: its param.json is not " + use.title_id + "'s.";
	}
	if (error.empty())
	{
		const int64_t free = FreeBytes(use.free_space, use.app_dir);
		if (free >= 0 && static_cast<uint64_t>(free) < managed_bytes + FREE_MARGIN)
			error = "There is not enough free space to unpack the update: " + MiB(managed_bytes + FREE_MARGIN) +
			        " are needed, " + MiB(static_cast<uint64_t>(free)) + " are free.";
	}
	if (error.empty())
	{
		Say(log, "the ZIP is sound: %zu of the program's files (%s), %zu others passed over", entries.size(),
			MiB(managed_bytes).c_str(), passed_over);
		std::vector<uint8_t> buffer(WRITE_BLOCK);
		std::string plan;
		for (const Entry& entry : entries)
		{
			outcome = Unpack(zip, entry, dir + "stage/" + entry.path, buffer, error);
			if (outcome != Outcome::Done)
				break;
			plan += entry.path + "\n";
		}
		if (outcome == Outcome::Done)
		{
			if (!WriteDurable(dir + "plan", plan))
				error = "The update's list of files could not be written.";
			else
				Say(log, "%zu files unpacked, ready to install", entries.size());
		}
	}
	zip_discard(zip);
	if (!error.empty())
		outcome = Outcome::Failed;
	return outcome;
}

void RunDownload(const Setup& use, const Release& release)
{
	const Log& log = use.log;
	const std::string dir = use.app_dir + ".update/";
	const std::string zip_path = dir + "release.zip";
	RemoveTree(dir);
	MakeDirs(use.app_dir + ".update");
	Outcome outcome = Outcome::Failed;
	std::string error;
	const int64_t free = FreeBytes(use.free_space, use.app_dir);
	if (!DirExists(dir))
		error = "The title's folder cannot be written.";
	else if (!IsHttps(release.zip_url))
		error = "The release's ZIP has no address to download it from.";
	else if (release.has_size && release.zip_size > MAX_ZIP_BYTES)
		error = "The release's ZIP is larger than an update may be.";
	else if (!use.http_download)
		error = "Downloads are not available.";
	else if (free >= 0 && static_cast<uint64_t>(free) < release.zip_size + FREE_MARGIN)
		error = "There is not enough free space for the update: " + MiB(release.zip_size + FREE_MARGIN) +
		        " are needed for the download, " + MiB(static_cast<uint64_t>(free)) + " are free.";
	else
	{
		Say(log, "downloading %s", release.zip_name.c_str());
		// The speed shown is evened out: a quarter of a second at a time, each new measure given
		// three tenths of the weight.
		double measured_at = Now();
		uint64_t measured_done = 0;
		const auto progress = [&](uint64_t done, uint64_t total) -> bool {
			if (g_stop)
				return false;
			const double time = Now();
			std::lock_guard<std::mutex> lock(g_state_mutex);
			g_current.done = done;
			g_current.total = total != 0 ? total : release.zip_size;
			if (done < measured_done)
			{
				// Begun again (a redirect, a retry).
				measured_done = done;
				measured_at = time;
			}
			else if (time - measured_at >= 0.25)
			{
				const double speed = static_cast<double>(done - measured_done) / (time - measured_at);
				g_current.speed = g_current.speed <= 0 ? speed : g_current.speed * 0.7 + speed * 0.3;
				measured_done = done;
				measured_at = time;
			}
			return true;
		};
		const int code = use.http_download(release.zip_url, zip_path, MAX_ZIP_BYTES, progress);
		if (g_stop)
			outcome = Outcome::Stopped;
		else if (code != 200)
			error = code < 0 ? "The download did not finish." : Format("The download's server answered %d.", code);
		else
		{
			chmod(zip_path.c_str(), 0666);
			{
				std::lock_guard<std::mutex> lock(g_state_mutex);
				g_current.state = State::Verifying;
				g_current.speed = 0;
			}
			Say(log, "downloaded, checking");
			outcome = VerifyAndUnpack(use, release, dir, error);
		}
	}

	if (outcome == Outcome::Done)
	{
		Finish(log, State::Ready);
		return;
	}
	RemoveTree(dir);
	if (outcome == Outcome::Stopped)
	{
		Say(log, "the download was stopped");
		Finish(log, State::Available);
	}
	else
		Finish(log, State::Failed, error);
}

// ------------------------------------------------------------------------------------ install

// Renames `from` to `to`, where nothing is: never onto a file (see exFAT above). Afterwards the
// drive must say that `from` is gone and that `to` is a file of the size `from` had. False with
// errno otherwise; what is on the disk is then whatever the drive did.
bool MoveVerified(const std::string& from, const std::string& to, bool lie = false)
{
	struct stat before;
	if (!LookAt(from, before) || !S_ISREG(before.st_mode))
	{
		errno = ENOENT;
		return false;
	}
	if (PathExists(to))
	{
		errno = EEXIST;
		return false;
	}
	if (!lie && rename(from.c_str(), to.c_str()) != 0)
		return false;
	struct stat after;
	if (PathExists(from) || !LookAt(to, after) || !S_ISREG(after.st_mode) || after.st_size != before.st_size)
	{
		errno = EIO;
		return false;
	}
	return true;
}

// Undoes what an install did, from the journal: the old files return from .update/backup/, the
// new ones go back to .update/stage/, the folders made for them are removed, and then .update/
// itself. Without a journal nothing was begun, and only .update/ is removed. `restored` counts
// the old files put back. False when an old file could not be put back: the journal and .update/
// then stay for the next try.
//
// A file is taken out of the title folder only when .update/new says the install added it. One
// with no backup that is not in that list is left where it is, whatever else is missing.
bool Undo(const std::string& app_dir, const Log& log, bool& had_journal, int& restored)
{
	const std::string dir = app_dir + ".update/";
	const std::string journal = dir + "journal";
	had_journal = PathExists(journal);
	restored = 0;
	if (!had_journal)
	{
		RemoveTree(dir);
		return true;
	}
	std::vector<std::string> paths;
	if (!ReadLines(journal, paths))
	{
		Say(log, "the journal of an interrupted install cannot be read; .update is kept");
		return false;
	}
#if defined(CRUX2_UPDATE_TEST)
	const int stop_after = Test::undo_stop_after;
#else
	const int stop_after = -1;
#endif
	int renames = 0;
	const auto cut = [&] { return stop_after >= 0 && renames++ >= stop_after; };
	std::vector<std::string> added;
	ReadLines(dir + "new", added);
	const std::set<std::string> fresh(added.begin(), added.end());
	bool complete = true;
	std::set<std::string> folders;
	int displaced = 0;
	for (size_t i = paths.size(); i-- > 0;)
	{
		const std::string& path = paths[i];
		if (!IsManaged(path))
			continue;
		const std::string place = app_dir + path;
		const std::string staged = dir + "stage/" + path;
		const std::string backup = dir + "backup/" + path;
		const bool installed = !PathExists(staged) && PathExists(place);
		if (PathExists(backup))
		{
			// The new file goes back to the stage first: were this interrupted between the two
			// renames, the next start would find the backup and a staged file, and finish it.
			if (installed)
			{
				if (cut())
					return false;
				MakeDirs(ParentOf(staged));
				MoveVerified(place, staged);
			}
			// Something still where the old file must return (a drive that kept a file it said it
			// had moved): moved aside, never renamed onto.
			if (PathExists(place))
			{
				const std::string aside = dir + "displaced/" + std::to_string(displaced++) + "/" + path;
				MakeDirs(ParentOf(aside));
				if (rename(place.c_str(), aside.c_str()) != 0 || PathExists(place))
					unlink(place.c_str());
			}
			if (cut())
				return false;
			MakeDirs(ParentOf(place));
			if (MoveVerified(backup, place))
				restored++;
			else
			{
				Say(log, "%s could not be put back (errno %d)", path.c_str(), errno);
				complete = false;
			}
		}
		else if (installed && fresh.count(path) != 0)
		{
			// A file the old version did not have, put in place. One that is still staged was not
			// reached, and is left.
			if (cut())
				return false;
			MakeDirs(ParentOf(staged));
			if (!MoveVerified(place, staged))
				unlink(place.c_str());
		}
		folders.insert(ParentOf(place));
	}
	if (!complete)
		return false;
	// The folders Install() made, the deepest first (they were listed the outer ones first). One
	// that has something in it stays.
	for (size_t i = added.size(); i-- > 0;)
		if (added[i].back() == '/' && PathFault(added[i], true) == nullptr && IsManaged(added[i] + "x"))
			rmdir((app_dir + added[i].substr(0, added[i].size() - 1)).c_str());
	// The journal goes only when the old files are surely in place, and the rest only when the
	// journal is surely gone.
	for (const std::string& folder : folders)
		SyncDir(folder);
	unlink(journal.c_str());
	SyncDir(dir);
	RemoveTree(dir);
	return true;
}

} // namespace

// ----------------------------------------------------------------------------------- versions

Version ParseVersion(const std::string& text)
{
	Version version;
	size_t at = 0;
	while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
		at++;
	if (at < text.size() && (text[at] == 'v' || text[at] == 'V'))
		at++;
	int parts[3] = {0, 0, 0};
	int count = 0;
	while (count < 3)
	{
		if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at])))
			break;
		long long value = 0;
		size_t digits = 0;
		for (; at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])); at++, digits++)
			if (digits < 9)
				value = value * 10 + (text[at] - '0');
		if (digits > 9)
			return Version();
		parts[count++] = static_cast<int>(value);
		// A dot continues the version only when a digit follows it.
		if (count < 3 && at + 1 < text.size() && text[at] == '.' && std::isdigit(static_cast<unsigned char>(text[at + 1])))
			at++;
		else
			break;
	}
	if (count < 2)
		return Version();
	version.major = parts[0];
	version.minor = parts[1];
	version.patch = parts[2];
	version.valid = true;
	return version;
}

int CompareVersions(const Version& a, const Version& b)
{
	if (a.valid != b.valid)
		return a.valid ? 1 : -1;
	if (!a.valid)
		return 0;
	if (a.major != b.major)
		return a.major < b.major ? -1 : 1;
	if (a.minor != b.minor)
		return a.minor < b.minor ? -1 : 1;
	if (a.patch != b.patch)
		return a.patch < b.patch ? -1 : 1;
	return 0;
}

Version VersionIn(const std::string& text)
{
	for (size_t at = 0; at < text.size(); at++)
	{
		if (!std::isdigit(static_cast<unsigned char>(text[at])))
			continue;
		// The start of a run of digits only (not "2" inside "12"), and not straight after a letter
		// other than 'v' ("Crux2" is a name, "v1" a version).
		if (at > 0 && std::isdigit(static_cast<unsigned char>(text[at - 1])))
			continue;
		if (at > 0 && std::isalpha(static_cast<unsigned char>(text[at - 1])) && text[at - 1] != 'v' && text[at - 1] != 'V')
			continue;
		const Version version = ParseVersion(text.substr(at));
		if (version.valid)
			return version;
	}
	return Version();
}

std::string ToString(const Version& version)
{
	if (!version.valid)
		return "0.0.0";
	return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
}

// ------------------------------------------------------------------------------------ helpers

bool IsManaged(const std::string& path)
{
	if (PathFault(path) != nullptr)
		return false;
	const std::string lower = Lowercase(path);
	for (const char* user : USER_FILES_INSIDE)
		if (lower == user)
			return false;
	for (const char* file : MANAGED_FILES)
		if (path == file)
			return true;
	for (const char* folder : MANAGED_FOLDERS)
	{
		const size_t length = std::strlen(folder);
		if (path.size() > length && path.compare(0, length, folder) == 0)
			return true;
	}
	return false;
}

namespace
{
// A line that only draws a rule: "---", "***", "* * *".
bool IsRule(const std::string& line)
{
	size_t marks = 0;
	for (const char c : line)
	{
		if (c == ' ')
			continue;
		if (c != line[0] || (c != '-' && c != '*' && c != '_'))
			return false;
		marks++;
	}
	return marks >= 3;
}

// "> [!NOTE]", "> [!WARNING]": GitHub's admonition marks, which say nothing on their own.
bool IsAdmonition(const std::string& rest)
{
	if (rest.size() < 4 || rest[0] != '[' || rest[1] != '!' || rest.back() != ']')
		return false;
	for (size_t i = 2; i + 1 < rest.size(); i++)
		if (!std::isalpha(static_cast<unsigned char>(rest[i])))
			return false;
	return true;
}

// "[text](address)" and "![text](address)" as the text alone.
std::string WithoutLinks(const std::string& text)
{
	std::string out;
	size_t at = 0;
	while (at < text.size())
	{
		const size_t open = text.find('[', at);
		if (open == std::string::npos)
			break;
		const size_t close = text.find(']', open + 1);
		if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != '(')
		{
			out.append(text, at, open + 1 - at);
			at = open + 1;
			continue;
		}
		const size_t end = text.find(')', close + 2);
		if (end == std::string::npos)
		{
			out.append(text, at, open + 1 - at);
			at = open + 1;
			continue;
		}
		size_t keep = open;
		if (keep > at && text[keep - 1] == '!')
			keep--;
		out.append(text, at, keep - at);
		out.append(text, open + 1, close - open - 1);
		at = end + 1;
	}
	out.append(text, at, std::string::npos);
	return out;
}
} // namespace

std::string PlainText(const std::string& markdown)
{
	std::string out;
	size_t position = 0;
	int empty_lines = 0;
	bool in_code = false;
	while (position < markdown.size())
	{
		size_t end = markdown.find('\n', position);
		if (end == std::string::npos)
			end = markdown.size();
		std::string line;
		for (size_t i = position; i < end; i++)
		{
			const unsigned char c = static_cast<unsigned char>(markdown[i]);
			if (c == '\t')
				line += "    ";
			else if (c >= 0x20 && c != 0x7f)
				line += static_cast<char>(c);
		}
		position = end + 1;
		while (!line.empty() && line.back() == ' ')
			line.pop_back();

		size_t indent = 0;
		while (indent < line.size() && line[indent] == ' ')
			indent++;
		std::string rest = line.substr(indent);
		std::string lead = line.substr(0, indent);
		if (StartsWith(rest, "```") || StartsWith(rest, "~~~"))
		{
			// A code block's fences go; what is between them stays as it is.
			in_code = !in_code;
			continue;
		}
		if (!in_code)
		{
			bool skip = false;
			while (!rest.empty() && rest[0] == '>')
				rest = Trim(rest.substr(1));
			if (IsAdmonition(rest))
				skip = true;
			else if (IsRule(rest))
				rest.clear();
			else if (!rest.empty() && rest[0] == '#')
			{
				size_t marks = 0;
				while (marks < rest.size() && rest[marks] == '#')
					marks++;
				if (marks == rest.size() || rest[marks] == ' ')
				{
					rest = Trim(rest.substr(marks));
					lead.clear();
				}
			}
			else if (rest.size() >= 2 && (rest[0] == '*' || rest[0] == '-' || rest[0] == '+') && rest[1] == ' ')
				rest = "- " + Trim(rest.substr(2));
			if (skip)
				continue;
			rest = WithoutLinks(rest);
			for (const char* mark : {"**", "`"})
				for (size_t at = rest.find(mark); at != std::string::npos; at = rest.find(mark, at))
					rest.erase(at, std::strlen(mark));
			while (!rest.empty() && rest.back() == ' ')
				rest.pop_back();
		}

		if (rest.empty())
		{
			empty_lines++;
			continue;
		}
		if (!out.empty())
			out.append(static_cast<size_t>(1 + std::min(empty_lines, 1)), '\n');
		out += lead + rest;
		empty_lines = 0;
	}
	return out;
}

std::string Sha256Hex(const void* data, size_t size)
{
	Sha256 sha;
	sha.Add(static_cast<const uint8_t*>(data), size);
	return sha.Finish();
}

// ---------------------------------------------------------------------------------------- API

bool Recover(const std::string& app_dir_given, const Log& log)
{
	const std::string app_dir = WithSlash(app_dir_given);
	if (app_dir.empty())
		return false;
	std::lock_guard<std::mutex> call(g_call_mutex);
	{
		// Called out of turn, while this very folder's .update is in use: there is nothing to
		// recover, and it must stay.
		std::lock_guard<std::mutex> lock(g_state_mutex);
		if (g_initialised && g_setup.app_dir == app_dir &&
			(g_current.state == State::Checking || g_current.state == State::Downloading ||
				g_current.state == State::Verifying || g_current.state == State::Ready))
			return false;
	}
	const bool leftover = PathExists(app_dir + ".update");
	bool had_journal = false;
	int restored = 0;
	const bool complete = Undo(app_dir, log, had_journal, restored);
	if (had_journal)
		Say(log, "an install was interrupted: %d old files put back%s", restored,
			complete ? "" : ", not all of them (tried again at the next start)");
	else if (leftover)
		Say(log, "removed what an earlier update left in .update");
	return had_journal;
}

void Init(const Setup& given)
{
	std::lock_guard<std::mutex> call(g_call_mutex);
	// A check or a download of an earlier set-up ends first.
	g_stop = true;
	g_worker.Join();
	g_stop = false;
	std::lock_guard<std::mutex> lock(g_state_mutex);
	g_setup = given;
	g_setup.app_dir = WithSlash(g_setup.app_dir);
	while (!g_setup.api_base.empty() && g_setup.api_base.back() == '/')
		g_setup.api_base.pop_back();
	g_found = Release();
	g_current = Status();
	g_initialised = true;
}

void Check()
{
	std::lock_guard<std::mutex> call(g_call_mutex);
	Setup use;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		if (!g_initialised)
			return;
		switch (g_current.state)
		{
			case State::Checking:
			case State::Downloading:
			case State::Verifying:
			case State::Ready: // unpacked and waiting: asking again would only lose it
			case State::Installed:
				return;
			default:
				break;
		}
		g_current = Status();
		g_current.state = State::Checking;
		use = g_setup;
	}
	StartWorker([use] { RunCheck(use); });
}

void Download()
{
	std::lock_guard<std::mutex> call(g_call_mutex);
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		if (!g_initialised || g_current.state != State::Available)
			return;
	}
	// The worker that found the release has set its last state: it is over, or all but. Only
	// then is the stop flag this download's.
	g_worker.Join();
	Setup use;
	Release release;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		g_stop = false;
		g_current.state = State::Downloading;
		g_current.error.clear();
		g_current.done = 0;
		g_current.total = g_found.zip_size;
		g_current.speed = 0;
		use = g_setup;
		release = g_found;
	}
	StartWorker([use, release] { RunDownload(use, release); });
}

void Reinstall()
{
	{
		std::lock_guard<std::mutex> call(g_call_mutex);
		std::lock_guard<std::mutex> lock(g_state_mutex);
		if (!g_initialised || g_current.state != State::UpToDate || !g_current.reinstall)
			return;
		Say(g_setup.log, "v%s to be put in place again", ToString(g_found.version).c_str());
		g_current.state = State::Available;
	}
	Download();
}

void Cancel()
{
	std::lock_guard<std::mutex> lock(g_state_mutex);
	if (g_current.state == State::Downloading || g_current.state == State::Verifying)
		g_stop = true;
}

bool Install()
{
	std::lock_guard<std::mutex> call(g_call_mutex);
	Setup use;
	std::string version;
	{
		std::lock_guard<std::mutex> lock(g_state_mutex);
		if (!g_initialised || g_current.state != State::Ready)
			return false;
		use = g_setup;
		version = g_current.version;
	}
	g_worker.Join();
	const Log& log = use.log;
	const std::string dir = use.app_dir + ".update/";

	// Everything that can be known beforehand is looked at before the first change: the plan,
	// each staged file, what is at each place now.
	std::string error;
	std::vector<std::string> plan;
	std::string plan_text, added_text;
	std::set<std::string> made;
	if (!ReadLines(dir + "plan", plan) || plan.empty())
		error = "The unpacked update is no longer there.";
	for (const std::string& path : plan)
	{
		if (!error.empty())
			break;
		struct stat st;
		if (!IsManaged(path))
			error = "The update's list of files is damaged.";
		else if (!IsRegularFile(dir + "stage/" + path))
			error = "The unpacked update is incomplete (" + path + ").";
		else if (LookAt(use.app_dir + path, st) && !S_ISREG(st.st_mode))
			error = path + " in the title's folder is not a file; it was left alone.";
		plan_text += path + "\n";
		// What the install adds: the folders that are not there yet, the outer ones first, and
		// the file when there is no older one.
		std::vector<std::string> above;
		for (std::string folder = ParentOf(path); !folder.empty(); folder = ParentOf(folder))
			above.push_back(folder);
		for (size_t i = above.size(); i-- > 0;)
			if (!PathExists(use.app_dir + above[i]) && made.insert(above[i]).second)
				added_text += above[i] + "/\n";
		if (!PathExists(use.app_dir + path))
			added_text += path + "\n";
	}
	if (!error.empty())
	{
		RemoveTree(dir);
		Finish(log, State::Failed, error);
		return false;
	}

	Say(log, "installing v%s (%zu files)", version.c_str(), plan.size());
#if defined(CRUX2_UPDATE_TEST)
	const int stop_after = Test::stop_after, fail_at = Test::fail_at, lie_at = Test::lie_at;
#else
	const int stop_after = -1, fail_at = -1, lie_at = -1;
#endif
	int renames = 0, attempts = 0;
	const auto step = [&](const std::string& from, const std::string& to) -> bool {
		const int attempt = attempts++;
		if (attempt == fail_at)
		{
			errno = EIO;
			return false;
		}
		if (!MoveVerified(from, to, attempt == lie_at))
			return false;
		renames++;
		return true;
	};
	// A test's power cut: everything is left as it is at that moment.
	const auto cut = [&] {
		if (stop_after < 0 || renames < stop_after)
			return false;
		Finish(log, State::Failed, "test: stopped as by a power cut");
		return true;
	};
	std::set<std::string> folders;
	// The list of what is new before the journal, the journal before the first rename.
	if (!WriteDurable(dir + "new", added_text) || !WriteDurable(dir + "journal", plan_text))
		error = "The update's journal could not be written.";
	else
	{
		SyncDir(dir);
		for (const std::string& path : plan)
		{
			if (cut())
				return false;
			const std::string place = use.app_dir + path;
			const std::string backup = dir + "backup/" + path;
			MakeDirs(ParentOf(backup));
			MakeDirs(ParentOf(place));
			folders.insert(ParentOf(place));
			// The old file away first, then the new one into the empty place.
			if (PathExists(place) && !step(place, backup))
			{
				error = Format("%s could not be moved aside (errno %d).", path.c_str(), errno);
				break;
			}
			if (cut())
				return false;
			if (!step(dir + "stage/" + path, place))
			{
				error = Format("%s could not be put in place (errno %d).", path.c_str(), errno);
				break;
			}
		}
	}
	if (!error.empty())
	{
		bool had_journal = false;
		int restored = 0;
		if (!Undo(use.app_dir, log, had_journal, restored))
			error += " Not every old file could be put back: start the title again.";
		Say(log, "the install was undone (%d old files put back)", restored);
		Finish(log, State::Failed, error);
		return false;
	}
	if (cut())
		return false;

	// The new files' names on the disk, then the journal gone, then the rest: at no moment is
	// there a journal beside a stage that is half removed.
	for (const std::string& folder : folders)
		SyncDir(folder);
	unlink((dir + "journal").c_str());
	SyncDir(dir);
	RemoveTree(dir);
	Say(log, "v%s is installed (%zu files); the title closes", version.c_str(), plan.size());
	Finish(log, State::Installed);
	return true;
}

Status GetStatus()
{
	std::lock_guard<std::mutex> lock(g_state_mutex);
	return g_current;
}

} // namespace PS5Update

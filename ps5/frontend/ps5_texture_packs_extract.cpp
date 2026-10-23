// Crux2 - HD texture packs: an archive unpacked as it comes (ps5_texture_packs_extract.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_texture_packs_extract.h"
#include "ps5_texture_packs_logic.h"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PS5TexturePacksExtract
{
namespace
{
struct Source
{
	const Read* read;
	std::vector<char> buffer;
};

la_ssize_t ReadCallback(struct archive*, void* data, const void** block)
{
	Source* const source = static_cast<Source*>(data);
	const long got = (*source->read)(source->buffer.data(), source->buffer.size());
	*block = source->buffer.data();
	return got < 0 ? ARCHIVE_FATAL : static_cast<la_ssize_t>(got);
}

bool WriteAll(int fd, const void* data, size_t size)
{
	const char* p = static_cast<const char*>(data);
	while (size > 0)
	{
		const ssize_t n = write(fd, p, size);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		p += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}
} // namespace

bool MakeFolders(const std::string& path)
{
	std::string so_far;
	for (size_t i = 0; i <= path.size(); i++)
	{
		if (i == path.size() || path[i] == '/')
		{
			if (!so_far.empty() && so_far != "/")
			{
				struct stat st;
				if (stat(so_far.c_str(), &st) != 0)
				{
					if (mkdir(so_far.c_str(), 0777) != 0 && errno != EEXIST)
						return false;
					(void)chmod(so_far.c_str(), 0777);
				}
				else if (!S_ISDIR(st.st_mode))
				{
					return false;
				}
			}
		}
		if (i < path.size())
			so_far.push_back(path[i]);
	}
	return true;
}

Result Extract(const Read& read, const std::string& root, const std::vector<std::string>& serials, const std::function<bool()>& stop)
{
	Result result;
	struct archive* a = archive_read_new();
	archive_read_support_format_rar(a);
	archive_read_support_format_rar5(a);
	archive_read_support_format_zip(a);
	archive_read_support_format_7zip(a);
	archive_read_support_filter_none(a);

	Source source{&read, std::vector<char>(1 << 20)};
	if (archive_read_open(a, &source, nullptr, ReadCallback, nullptr) != ARCHIVE_OK)
	{
		result.error = archive_error_string(a) ? archive_error_string(a) : "the archive could not be opened";
		archive_read_free(a);
		return result;
	}

	std::vector<char> block(1 << 20);
	for (;;)
	{
		if (stop && stop())
		{
			result.error = "stopped";
			break;
		}
		struct archive_entry* entry = nullptr;
		const int rc = archive_read_next_header(a, &entry);
		if (rc == ARCHIVE_EOF)
		{
			result.ok = true;
			break;
		}
		if (rc < ARCHIVE_WARN)
		{
			result.error = archive_error_string(a) ? archive_error_string(a) : "the archive could not be read";
			break;
		}
		const char* name = archive_entry_pathname(entry);
		const std::string target = (archive_entry_filetype(entry) == AE_IFREG && name) ? PS5TexturePacksLogic::EntryTarget(name, serials) : std::string();
		if (target.empty())
		{
			result.skipped++;
			archive_read_data_skip(a);
			continue;
		}
		const std::string path = root + "/" + target;
		const std::string folder = path.substr(0, path.rfind('/'));
		if (!MakeFolders(folder))
		{
			result.error = "a folder could not be made: " + folder + " (" + std::strerror(errno) + ")";
			break;
		}
		const std::string partial = path + ".partial";
		const int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
		{
			result.error = "a file could not be written: " + path + " (" + std::strerror(errno) + ")";
			break;
		}
		bool failed = false, stopped = false;
		for (;;)
		{
			const la_ssize_t n = archive_read_data(a, block.data(), block.size());
			if (n == 0)
				break;
			if (n < 0)
			{
				result.error = archive_error_string(a) ? archive_error_string(a) : "the archive could not be read";
				failed = true;
				break;
			}
			if (!WriteAll(fd, block.data(), static_cast<size_t>(n)))
			{
				result.error = "a file could not be written: " + path + " (" + std::strerror(errno) + ")";
				failed = true;
				break;
			}
			result.bytes += static_cast<uint64_t>(n);
			if (stop && stop())
			{
				stopped = true;
				break;
			}
		}
		close(fd);
		if (failed || stopped)
		{
			unlink(partial.c_str());
			if (stopped)
				result.error = "stopped";
			break;
		}
		if (rename(partial.c_str(), path.c_str()) != 0)
		{
			result.error = "a file could not be put in place: " + path + " (" + std::strerror(errno) + ")";
			unlink(partial.c_str());
			break;
		}
		(void)chmod(path.c_str(), 0666);
		result.files++;
	}
	archive_read_free(a);
	return result;
}
} // namespace PS5TexturePacksExtract

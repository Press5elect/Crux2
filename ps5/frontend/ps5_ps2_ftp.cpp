// Crux2 - the memory cards of a real PlayStation 2 over FTP (ps5_ps2_ftp.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_ps2_ftp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace PS5Ps2Ftp
{
namespace
{
constexpr uint64_t LARGEST_SAVE = 16ull << 20;

void SetTimeouts(int fd, int seconds)
{
	const struct timeval timeout = {seconds, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

int ConnectTo(const struct in_addr& address, unsigned port, int seconds)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	SetTimeouts(fd, seconds);
	struct sockaddr_in to;
	std::memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(static_cast<unsigned short>(port));
	to.sin_addr = address;
	if (connect(fd, reinterpret_cast<const struct sockaddr*>(&to), sizeof(to)) != 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

bool ReadAll(int fd, std::string* out, uint64_t limit)
{
	char buffer[16384];
	for (;;)
	{
		const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return false;
		if (n == 0)
			return true;
		out->append(buffer, static_cast<size_t>(n));
		if (out->size() > limit)
			return false;
	}
}

std::string Join(const std::string& folder, const std::string& name)
{
	return (!folder.empty() && folder.back() == '/') ? folder + name : folder + "/" + name;
}
} // namespace

bool ParseListLine(const std::string& raw, Entry* entry)
{
	std::string line = raw;
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
		line.pop_back();
	if (line.empty())
		return false;
	// Unix: "drwxrwxrwx 1 ps2 ps2 0 Sep  8  2013 CBC": nine fields, the name the rest.
	if (line[0] == 'd' || line[0] == '-' || line[0] == 'l')
	{
		size_t at = 0;
		std::string fields[8];
		for (int field = 0; field < 8; field++)
		{
			while (at < line.size() && line[at] == ' ')
				at++;
			const size_t start = at;
			while (at < line.size() && line[at] != ' ')
				at++;
			if (at == start)
				return false;
			fields[field] = line.substr(start, at - start);
		}
		// (One space separates the time or year from the name.)
		if (at < line.size() && line[at] == ' ')
			at++;
		if (at >= line.size())
			return false;
		entry->name = line.substr(at);
		entry->folder = (line[0] != '-');
		entry->size = entry->folder ? 0 : std::strtoull(fields[4].c_str(), nullptr, 10);
	}
	else
	{
		// MS-DOS: "09-08-13  12:00AM       <DIR>          CBC" / "... 380164 boot.elf".
		char date[32], time[32], what[64];
		int used = 0;
		if (std::sscanf(line.c_str(), "%31s %31s %63s %n", date, time, what, &used) != 3 || used <= 0 ||
			static_cast<size_t>(used) >= line.size())
			return false;
		entry->name = line.substr(static_cast<size_t>(used));
		entry->folder = (std::strcmp(what, "<DIR>") == 0);
		entry->size = entry->folder ? 0 : std::strtoull(what, nullptr, 10);
	}
	return entry->name != "." && entry->name != "..";
}

Client::~Client()
{
	Close();
}

void Client::Close()
{
	if (m_control >= 0)
	{
		const char quit[] = "QUIT\r\n";
		(void)!send(m_control, quit, sizeof(quit) - 1, 0);
		close(m_control);
	}
	m_control = -1;
}

bool Client::ReadReply(int* code)
{
	// A reply: "123 text", or "123-text" ... up to "123 text".
	std::string line, first;
	for (;;)
	{
		line.clear();
		for (;;)
		{
			char c;
			const ssize_t got = recv(m_control, &c, 1, 0);
			if (got < 0 && errno == EINTR)
				continue;
			if (got <= 0)
				return false;
			if (c == '\n')
				break;
			if (c != '\r' && line.size() < 2048)
				line.push_back(c);
		}
		if (first.empty())
		{
			first = line;
			if (line.size() >= 4 && line[3] == '-')
				continue;
			break;
		}
		if (line.size() >= 4 && line.compare(0, 3, first, 0, 3) == 0 && line[3] == ' ')
			break;
	}
	m_reply = first;
	*code = (first.size() >= 3) ? std::atoi(first.substr(0, 3).c_str()) : 0;
	return true;
}

int Client::Command(const std::string& line)
{
	if (m_control < 0)
		return -1;
	const std::string out = line + "\r\n";
	size_t sent = 0;
	while (sent < out.size())
	{
		const ssize_t n = send(m_control, out.data() + sent, out.size() - sent, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
		{
			Close();
			return -1;
		}
		sent += static_cast<size_t>(n);
	}
	int code = 0;
	if (!ReadReply(&code))
	{
		Close();
		return -1;
	}
	return code;
}

bool Client::Connect(const std::string& host, unsigned port, std::string* why)
{
	Close();
	struct in_addr address;
	if (inet_pton(AF_INET, host.c_str(), &address) != 1)
	{
		*why = "\"" + host + "\" is not an address (four numbers, like 192.168.1.20)";
		return false;
	}
	m_host = host;
	m_control = ConnectTo(address, port, m_timeout);
	if (m_control < 0)
	{
		*why = "nothing answers at " + host + ":" + std::to_string(port) + " (" + std::strerror(errno) + ")";
		return false;
	}
	int code = 0;
	if (!ReadReply(&code) || code != 220)
	{
		*why = "what answers there is not an FTP server";
		Close();
		return false;
	}
	code = Command("USER anonymous");
	if (code == 331)
		code = Command("PASS crux2@");
	if (code != 230 && code != 202)
	{
		*why = "the server did not let Crux2 in (" + m_reply + ")";
		Close();
		return false;
	}
	Command("TYPE I");
	return true;
}

int Client::OpenPassive(std::string* why)
{
	if (Command("PASV") != 227)
	{
		*why = "passive mode refused (" + m_reply + ")";
		return -1;
	}
	const char* const numbers = std::strchr(m_reply.c_str(), '(');
	unsigned h[4], p[2];
	if (!numbers || std::sscanf(numbers, "(%u,%u,%u,%u,%u,%u)", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6)
	{
		*why = "passive mode's answer did not parse";
		return -1;
	}
	// (The address the server names may be one it does not answer on: the one it was reached at.)
	struct in_addr address;
	inet_pton(AF_INET, m_host.c_str(), &address);
	const int fd = ConnectTo(address, p[0] * 256 + p[1], m_timeout);
	if (fd < 0)
		*why = "the data connection could not be made";
	return fd;
}

int Client::OpenActive(int* listener, std::string* why)
{
	// Our own address, as the control connection has it; a port the system picks.
	struct sockaddr_in local;
	socklen_t length = sizeof(local);
	if (getsockname(m_control, reinterpret_cast<struct sockaddr*>(&local), &length) != 0)
		return -1;
	*listener = socket(AF_INET, SOCK_STREAM, 0);
	if (*listener < 0)
		return -1;
	struct sockaddr_in any = local;
	any.sin_port = 0;
	if (bind(*listener, reinterpret_cast<struct sockaddr*>(&any), sizeof(any)) != 0 || listen(*listener, 1) != 0)
	{
		*why = "active mode: no port could be opened here";
		return -1;
	}
	length = sizeof(any);
	getsockname(*listener, reinterpret_cast<struct sockaddr*>(&any), &length);
	const uint32_t a = ntohl(local.sin_addr.s_addr);
	const unsigned port = ntohs(any.sin_port);
	char command[64];
	std::snprintf(command, sizeof(command), "PORT %u,%u,%u,%u,%u,%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255, port >> 8, port & 255);
	if (Command(command) != 200)
	{
		*why = "active mode refused (" + m_reply + ")";
		return -1;
	}
	return 0;
}

int Client::OpenData(const std::string& command, std::string* why)
{
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!m_active)
		{
			const int data = OpenPassive(why);
			if (data >= 0)
			{
				const int code = Command(command);
				if (code == 150 || code == 125)
					return data;
				close(data);
				if (code < 0)
					return -1;
				*why = m_reply;
				// A listing refused in passive mode: active mode next (ps2ftpd lists "/" so).
				m_active = true;
				continue;
			}
			m_active = true;
			continue;
		}
		int listener = -1;
		if (OpenActive(&listener, why) != 0)
		{
			if (listener >= 0)
				close(listener);
			return -1;
		}
		const int code = Command(command);
		if (code != 150 && code != 125)
		{
			close(listener);
			*why = m_reply;
			return -1;
		}
		struct pollfd wait = {listener, POLLIN, 0};
		const int ready = poll(&wait, 1, m_timeout * 1000);
		const int data = (ready > 0) ? accept(listener, nullptr, nullptr) : -1;
		close(listener);
		if (data < 0)
		{
			*why = "the PlayStation 2 did not open the data connection";
			return -1;
		}
		SetTimeouts(data, m_timeout);
		return data;
	}
	return -1;
}

bool Client::List(const std::string& folder, std::vector<Entry>* entries, std::string* why)
{
	entries->clear();
	if (m_control < 0)
	{
		*why = "not connected";
		return false;
	}
	const int data = OpenData("LIST " + folder, why);
	if (data < 0)
		return false;
	std::string text;
	const bool read = ReadAll(data, &text, 4u << 20);
	close(data);
	int code = 0;
	if (!ReadReply(&code) || code != 226 || !read)
	{
		*why = read ? ("the listing ended with " + m_reply) : "the listing was cut off";
		return false;
	}
	size_t at = 0;
	while (at < text.size())
	{
		const size_t end = text.find('\n', at);
		Entry entry;
		if (ParseListLine(text.substr(at, end == std::string::npos ? std::string::npos : end - at), &entry))
			entries->push_back(std::move(entry));
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return true;
}

bool Client::Read(const std::string& path, std::string* data, uint64_t limit, std::string* why)
{
	data->clear();
	const int fd = OpenData("RETR " + path, why);
	if (fd < 0)
		return false;
	const bool read = ReadAll(fd, data, limit);
	close(fd);
	int code = 0;
	if (!ReadReply(&code) || code != 226 || !read)
	{
		*why = read ? ("the transfer ended with " + m_reply) : "the transfer was cut off or too large";
		return false;
	}
	return true;
}

bool ListSaves(Client& client, std::vector<Save>* saves, std::string* why)
{
	saves->clear();
	bool any_card = false;
	std::string last_why;
	static const struct
	{
		const char* path;
		const char* name;
	} cards[] = {{"/mc/0", "Memory card 1"}, {"/mc/1", "Memory card 2"}};
	for (const auto& card : cards)
	{
		std::vector<Entry> top;
		if (!client.List(card.path, &top, &last_why))
			continue;
		any_card = true;
		for (const Entry& folder : top)
		{
			if (!folder.folder)
				continue;
			std::vector<Entry> files;
			const std::string path = Join(card.path, folder.name);
			if (!client.List(path, &files, &last_why))
				continue;
			bool icon = false;
			Save save{card.name, path, folder.name, 0, 0};
			for (const Entry& f : files)
			{
				if (f.folder)
					continue;
				save.files++;
				save.bytes += f.size;
				std::string lower = f.name;
				for (char& c : lower)
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				icon |= (lower == "icon.sys");
			}
			if (icon)
				saves->push_back(std::move(save));
		}
	}
	if (!any_card)
	{
		*why = last_why.empty() ? std::string("no memory card could be listed") : "no memory card could be listed (" + last_why + ")";
		return false;
	}
	return true;
}

bool ReadSave(Client& client, const std::string& path, std::vector<SaveFile>* files, std::string* why)
{
	files->clear();
	std::vector<Entry> entries;
	if (!client.List(path, &entries, why))
		return false;
	uint64_t total = 0;
	for (const Entry& e : entries)
	{
		if (e.folder)
			continue;
		SaveFile file{e.name, std::string()};
		if (!client.Read(Join(path, e.name), &file.data, LARGEST_SAVE, why))
		{
			*why = e.name + ": " + *why;
			return false;
		}
		total += file.data.size();
		if (total > LARGEST_SAVE)
		{
			*why = "the save is larger than any memory card";
			return false;
		}
		files->push_back(std::move(file));
	}
	if (files->empty())
	{
		*why = "the save's folder holds no file";
		return false;
	}
	return true;
}
} // namespace PS5Ps2Ftp

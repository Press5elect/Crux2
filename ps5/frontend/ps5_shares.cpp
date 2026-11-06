// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Crux2 v1.0.19 - the network shares (ps5_shares.h), from OGBox 0.0.3's ogbox_shares.cpp.
#include "ps5_shares.h"

#include <algorithm>
#include <cctype>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifndef PS5_SHARES_HOST_TEST
#include "ps5_vfs.h"
#endif

namespace PS5Shares {

namespace {

std::string trimmed(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
		a++;
	}
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
		b--;
	}
	return s.substr(a, b - a);
}

std::string lower(std::string s)
{
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

} // namespace

std::string percentEncode(const std::string &s)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (const unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string accountEncode(const std::string &user)
{
	// "DOMAIN;user": the two encoded apart, the ';' kept (the share's address reads it so)
	const size_t semi = user.find(';');
	return semi == std::string::npos ? percentEncode(user)
	                                 : percentEncode(user.substr(0, semi)) + ";" + percentEncode(user.substr(semi + 1));
}

std::string percentDecode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
			std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
			out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string shareDefaultName(const Share &s)
{
	std::string p = s.path;
	const size_t q = p.find('?');
	if (q != std::string::npos) {
		p.resize(q);
	}
	while (!p.empty() && p.back() == '/') {
		p.pop_back();
	}
	const size_t slash = p.find_last_of('/');
	std::string last = slash == std::string::npos ? p : p.substr(slash + 1);
	return last.empty() ? s.host : last;
}

bool readShares(const std::string &file, ShareFile &out, std::string *error)
{
	out = ShareFile{};
	FILE *f = std::fopen(file.c_str(), "r");
	if (!f) {
		if (error) {
			*error = std::strerror(errno);
		}
		return false;
	}
	std::string pendingName;
	char line[2048];
	while (std::fgets(line, sizeof(line), f)) {
		std::string text = trimmed(line);
		if (text.compare(0, 3, "\xef\xbb\xbf") == 0) {
			text = trimmed(text.substr(3));
		}
		if (text.compare(0, 2, "#@") == 0) {
			const size_t eq = text.find('=');
			if (eq != std::string::npos && lower(trimmed(text.substr(2, eq - 2))) == "name") {
				pendingName = trimmed(text.substr(eq + 1));
			}
			continue;
		}
		if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[') {
			continue;
		}
		const size_t eq = text.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		const std::string key = lower(trimmed(text.substr(0, eq)));
		std::string value = trimmed(text.substr(eq + 1));
		if (key == "user") {
			out.user = value.empty() ? "guest" : value;
		} else if (key == "password") {
			out.password = value;
		} else if (key == "uid") {
			out.uid = value;
		} else if (key == "gid") {
			out.gid = value;
		} else if (key == "path" && !value.empty()) {
			Share s;
			if (lower(value.substr(0, 6)) == "nfs://") {
				s.nfs = true;
				value.erase(0, 6);
			} else {
				for (char &c : value) {
					c = c == '\\' ? '/' : c;
				}
				if (lower(value.substr(0, 6)) == "smb://") {
					value.erase(0, 6);
				}
				while (!value.empty() && value[0] == '/') {
					value.erase(0, 1);
				}
			}
			const size_t slash = value.find('/');
			std::string head = value.substr(0, slash);
			s.path = slash == std::string::npos ? std::string() : value.substr(slash + 1);
			if (s.nfs) {
				s.path = "/" + s.path;
			}
			const size_t at = head.rfind('@');
			if (!s.nfs && at != std::string::npos) {
				const std::string account = head.substr(0, at);
				head.erase(0, at + 1);
				const size_t colon = account.find(':');
				const std::string user = colon == std::string::npos ? account : account.substr(0, colon);
				s.user = percentDecode(user); // (a "DOMAIN;user" keeps its ';')
				s.password = colon == std::string::npos ? std::string() : percentDecode(account.substr(colon + 1));
			}
			s.host = head;
			s.name = pendingName.empty() ? shareDefaultName(s) : pendingName;
			out.shares.push_back(s);
		}
		pendingName.clear();
	}
	std::fclose(f);
	return true;
}

bool writeShares(const std::string &file, const ShareFile &in, std::string *error)
{
	std::string text =
		"# Crux2's network shares. Settings > Folders writes this file when it changes them; editing it\n"
		"# by hand works too (restart Crux2 after). One \"path =\" line per share, its name in the\n"
		"# \"#@ name =\" line above it (without one, the share is named by its last folder).\n"
		"# SMB (a Windows PC, most NAS): server/share/folder, the server by its IP address; an account of\n"
		"# its own as user:password@server/share/folder (percent-encoded). user and password below: the\n"
		"# account for the SMB shares without one (guest and no password for an open share).\n"
		"# NFS (a NAS or a Linux PC): nfs://server/export/folder; uid and gid: the account the NAS is\n"
		"# asked as (1000 when not given).\n\n";
	text += "user = " + (in.user.empty() ? std::string("guest") : in.user) + "\n";
	text += "password = " + in.password + "\n";
	if (!in.uid.empty()) {
		text += "uid = " + in.uid + "\n";
	}
	if (!in.gid.empty()) {
		text += "gid = " + in.gid + "\n";
	}
	for (const Share &s : in.shares) {
		text += "\n#@ name = " + s.name + "\n";
		if (s.nfs) {
			std::string p = s.path;
			while (!p.empty() && p[0] == '/') {
				p.erase(0, 1);
			}
			text += "path = nfs://" + s.host + "/" + p + "\n";
		} else {
			std::string account;
			if (!s.user.empty()) {
				account = accountEncode(s.user) + (s.password.empty() ? std::string() : ":" + percentEncode(s.password)) + "@";
			}
			text += "path = " + account + s.host + "/" + s.path + "\n";
		}
	}
	const std::string tmp = file + ".new";
	FILE *f = std::fopen(tmp.c_str(), "w");
	if (!f) {
		if (error) {
			*error = std::strerror(errno);
		}
		return false;
	}
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	const bool closed = std::fclose(f) == 0;
	if (!ok || !closed || std::rename(tmp.c_str(), file.c_str()) != 0) {
		if (error) {
			*error = std::strerror(errno);
		}
		std::remove(tmp.c_str());
		return false;
	}
	return true;
}

std::string shareAddress(const Share &s)
{
	if (s.nfs) {
		std::string p = s.path;
		const size_t q = p.find('?');
		if (q != std::string::npos) {
			p.resize(q);
		}
		return "nfs://" + s.host + (p.empty() || p[0] != '/' ? "/" : "") + p;
	}
	return "smb://" + s.host + "/" + s.path;
}

std::string shareMountName(const std::string &name, const std::vector<std::string> &taken)
{
	std::string n;
	for (const char c : name) {
		const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
		n += ok ? c : '_';
	}
	if (n.size() > 28) {
		n.resize(28);
	}
	if (n.empty() || n == "." || n == "..") {
		n = "share";
	}
	std::string unique = n;
	for (unsigned k = 2;; k++) {
		bool used = false;
		for (const std::string &t : taken) {
			used = used || t == "/smb/" + unique;
		}
		if (!used) {
			return unique;
		}
		unique = n + "-" + std::to_string(k);
	}
}

bool mountShare(Share &s, const ShareFile &file, const std::vector<std::string> &taken, std::string *error)
{
	const std::string name = shareMountName(s.name, taken);
	std::string url;
	if (s.nfs) {
		url = shareAddress(s);
		const size_t q = s.path.find('?');
		if (q != std::string::npos) {
			url += s.path.substr(q);
		}
		for (const auto &kv : { std::make_pair(std::string("uid"), file.uid), std::make_pair(std::string("gid"), file.gid) }) {
			if (!kv.second.empty() && url.find(kv.first + "=") == std::string::npos) {
				url += (url.find('?') == std::string::npos ? "?" : "&") + kv.first + "=" + kv.second;
			}
		}
	} else {
		const std::string user = s.user.empty() ? file.user : s.user;
		const std::string password = s.user.empty() ? file.password : s.password;
		url = "smb://" + accountEncode(user.empty() ? std::string("guest") : user) +
			(password.empty() ? std::string() : ":" + percentEncode(password)) + "@" + s.host + "/" + s.path;
	}
#ifndef PS5_SHARES_HOST_TEST
	const int r = s.nfs ? ps5vfs_mount_nfs(name.c_str(), url.c_str()) : ps5vfs_mount_smb(name.c_str(), url.c_str());
	if (r != 0) {
		if (error) {
			*error = "that address can't be used";
		}
		return false;
	}
#endif
	s.mount = "/smb/" + name;
	return true;
}

void unmountShare(Share &s)
{
	if (!s.mount.empty()) {
#ifndef PS5_SHARES_HOST_TEST
		ps5vfs_unmount(s.mount.c_str());
#endif
		s.mount.clear();
	}
}


// ---- the servers on the network --------------------------------------------------------

namespace {

/* this console's IPv4 address (the route out: a UDP socket "connected", nothing sent), 0 none */
uint32_t localAddress()
{
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return 0;
	}
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(9);
	to.sin_addr.s_addr = htonl(0xC0000201);   // 192.0.2.1 (TEST-NET-1: never reached)
	uint32_t ip = 0;
	sockaddr_in me{};
	socklen_t len = sizeof(me);
	if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0 &&
		getsockname(fd, reinterpret_cast<sockaddr *>(&me), &len) == 0) {
		ip = ntohl(me.sin_addr.s_addr);
	}
	close(fd);
	return ip;
}

/* a NetBIOS node status answer's file server name (suffix 0x20, unique), else its first unique */
std::string nbstatName(const uint8_t *b, size_t n)
{
	if (n < 57) {
		return std::string();
	}
	const unsigned count = b[56];
	std::string first;
	for (unsigned i = 0; i < count && 57 + (i + 1) * 18 <= n; i++) {
		const uint8_t *e = b + 57 + i * 18;
		const bool group = (e[16] & 0x80) != 0;
		std::string name(reinterpret_cast<const char *>(e), 15);
		while (!name.empty() && (name.back() == ' ' || name.back() == 0)) {
			name.pop_back();
		}
		if (group || name.empty()) {
			continue;
		}
		if (e[15] == 0x20) {
			return name;
		}
		if (first.empty() && e[15] == 0x00) {
			first = name;
		}
	}
	return first;
}

} // namespace

std::vector<FoundServer> findServers()
{
	std::vector<FoundServer> out;
	const uint32_t me = localAddress();
	if (!me) {
		return out;
	}
	const uint32_t net = me & 0xFFFFFF00u;
	/* port 445 of the /24's hosts, 64 at a time, each batch given 400 ms */
	std::vector<uint32_t> open;
	for (uint32_t base = 1; base < 255; base += 64) {
		std::vector<std::pair<int, uint32_t>> socks;
		for (uint32_t h = base; h < base + 64 && h < 255; h++) {
			const uint32_t ip = net | h;
			if (ip == me) {
				continue;
			}
			const int fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0) {
				continue;
			}
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(445);
			a.sin_addr.s_addr = htonl(ip);
			const int rc = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
			if (rc == 0) {
				open.push_back(ip);
				close(fd);
			} else if (errno == EINPROGRESS) {
				socks.push_back({ fd, ip });
			} else {
				close(fd);
			}
		}
		std::vector<pollfd> pfd;
		for (const auto &s : socks) {
			pfd.push_back({ s.first, POLLOUT, 0 });
		}
		for (int waited = 0; waited < 400 && !pfd.empty(); waited += 50) {
			if (poll(pfd.data(), pfd.size(), 50) <= 0) {
				continue;
			}
			for (size_t i = 0; i < pfd.size(); i++) {
				if (pfd[i].fd >= 0 && (pfd[i].revents & (POLLOUT | POLLERR | POLLHUP))) {
					int err = 1;
					socklen_t len = sizeof(err);
					if ((pfd[i].revents & POLLOUT) && getsockopt(pfd[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
						open.push_back(socks[i].second);
					}
					pfd[i].fd = -1;     // (answered: not polled again)
				}
			}
		}
		for (const auto &s : socks) {
			close(s.first);
		}
	}
	std::sort(open.begin(), open.end());
	open.erase(std::unique(open.begin(), open.end()), open.end());
	/* their names: a NetBIOS node status request ("*") to each, the answers for 500 ms */
	const int udp = socket(AF_INET, SOCK_DGRAM, 0);
	std::vector<std::string> names(open.size());
	if (udp >= 0 && !open.empty()) {
		uint8_t q[50] = { 0x4F, 0x47, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 'C', 'K' };
		for (int i = 15; i < 45; i++) {
			q[i] = 'A';
		}
		q[45] = 0x00;
		q[46] = 0x00;
		q[47] = 0x21;   // NBSTAT
		q[48] = 0x00;
		q[49] = 0x01;   // IN
		for (const uint32_t ip : open) {
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(137);
			a.sin_addr.s_addr = htonl(ip);
			sendto(udp, q, sizeof(q), 0, reinterpret_cast<sockaddr *>(&a), sizeof(a));
		}
		for (int waited = 0; waited < 500; waited += 50) {
			pollfd p{ udp, POLLIN, 0 };
			if (poll(&p, 1, 50) <= 0) {
				continue;
			}
			uint8_t b[1500];
			sockaddr_in from{};
			socklen_t fl = sizeof(from);
			const ssize_t n = recvfrom(udp, b, sizeof(b), 0, reinterpret_cast<sockaddr *>(&from), &fl);
			if (n <= 0) {
				continue;
			}
			const auto it = std::find(open.begin(), open.end(), ntohl(from.sin_addr.s_addr));
			if (it != open.end()) {
				names[static_cast<size_t>(it - open.begin())] = nbstatName(b, static_cast<size_t>(n));
			}
		}
	}
	if (udp >= 0) {
		close(udp);
	}
	for (size_t i = 0; i < open.size(); i++) {
		char ip[16];
		std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", open[i] >> 24, (open[i] >> 16) & 255, (open[i] >> 8) & 255, open[i] & 255);
		out.push_back({ ip, names[i] });
	}
	return out;
}

namespace {
ShareFile s_current;
std::string s_current_file;
} // namespace

ShareFile &Current()
{
	return s_current;
}

const std::string &CurrentFile()
{
	return s_current_file;
}

std::vector<std::string> MountsTaken(const Share *except)
{
	std::vector<std::string> taken;
	for (const Share &s : s_current.shares) {
		if (&s != except && !s.mount.empty()) {
			taken.push_back(s.mount);
		}
	}
	return taken;
}

int LoadAndMount(const std::string &file, std::string *error)
{
	s_current_file = file;
	if (!readShares(file, s_current, error)) {
		return -1;
	}
	int mounted = 0;
	for (Share &s : s_current.shares) {
		if (mountShare(s, s_current, MountsTaken(&s))) {
			mounted++;
		}
	}
	return mounted;
}

bool Save(std::string *error)
{
	return writeShares(s_current_file, s_current, error);
}

} // namespace PS5Shares

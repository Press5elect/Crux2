// Crux2 - the write broker's client (ps5_broker.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Client and Launch taken from OGBox (Press5elect, the same author: ps5/src/platform/ps5_broker.cpp
// at db63d46); Session and File are Crux2's, after OGBox's storage move (ogbox_storage.cpp MoveHelper).

#include "ps5_broker.h"

#include "../broker/posix.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__PROSPERO__) && !defined(PS5_BROKER_TEST)
#include "Host.h"
#include "ps5_platform.h"
#define BROKER_LOG(...) ps5px_log(__VA_ARGS__)
#else
#define BROKER_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace PS5Broker
{
namespace
{
using ::broker::now_ns;

constexpr int kHelloMs = 8000;           // the loader starts the payload: its first answer
constexpr int kReplyMs = 120000;         // a request's answer (a COPY of 32 MiB from a slow drive)
constexpr int kSyncMs = 300000;          // a file's fsync
constexpr int kFinishMs = 60000;         // given up: its end (what it was sent carried out first)
constexpr uint64_t kCopyPiece = 32ull << 20; // a COPY: the progress moves a piece at a time
constexpr size_t kWritePiece = 8u << 20; // a WRITE
constexpr size_t kAhead = 4;             // WRITEs sent ahead of their answers

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* format, ...)
{
	char line[768];
	va_list args;
	va_start(args, format);
	std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	return line;
}

double MiBps(uint64_t bytes, uint64_t ns)
{
	return ns ? static_cast<double>(bytes) / 1048576.0 / (static_cast<double>(ns) / 1e9) : 0.0;
}

std::mutex s_test_mutex;
Connector s_test_connect;
std::string s_test_root;
bool s_test_enabled = false;

Connector TheConnector()
{
	{
		std::lock_guard<std::mutex> lock(s_test_mutex);
		if (s_test_connect)
			return s_test_connect;
	}
#if defined(__PROSPERO__) && !defined(PS5_BROKER_TEST)
	return [](std::string& why) { return Launch(std::string(ps5px_title_dir()) + "/write-broker.elf", why); };
#else
	return [](std::string& why) {
		why = "no broker on this machine";
		return -1;
	};
#endif
}

// The path as the broker sees it; false: not one it takes, or not on the console's storage.
bool BrokerPath(const std::string& local, std::string& out, bool data_only)
{
	std::string root;
	{
		std::lock_guard<std::mutex> lock(s_test_mutex);
		root = s_test_root;
	}
	out = (!root.empty() && local.compare(0, root.size(), root) == 0) ? local.substr(root.size()) : local;
	if (data_only && out.compare(0, 6, "/data/") != 0)
		return false;
	return ::broker::path_allowed(out.data(), out.size());
}

std::string Said(const Client* c, int e)
{
	if (e < 0)
		return c ? c->Error() : std::string("no connection");
	return e == ENOSYS ? std::string("this broker can't copy (ENOSYS)") : std::string(std::strerror(e));
}
} // namespace

// ------------------------------------------------------------------------------------------ client

Client::~Client()
{
	if (m_fd >= 0)
		close(m_fd);
}

bool Client::Fail(const char* what)
{
	const int e = errno;
	m_error = Format("%s (errno %d: %s)", what, e, std::strerror(e));
	return false;
}

bool Client::Send(Op op, uint32_t handle, uint64_t offset, uint64_t length, uint32_t flags, const void* body, size_t size)
{
	if (m_fd < 0)
	{
		errno = ENOTCONN;
		return Fail("no connection");
	}
	if (size > ::broker::kMaxBody + ::broker::kMaxPath + 64)
	{
		errno = EMSGSIZE;
		return Fail("a request too large");
	}
	::broker::Request q;
	q.op = static_cast<uint16_t>(op);
	q.seq = m_seq++;
	q.handle = handle;
	q.offset = offset;
	q.length = length;
	q.size = static_cast<uint32_t>(size);
	q.flags = flags;
	if (!::broker::send_all(m_fd, &q, sizeof(q), timeout_ms) || (size != 0 && !::broker::send_all(m_fd, body, size, timeout_ms)))
		return Fail("a request could not be sent");
	m_pending.emplace_back(q.seq, q.op);
	return true;
}

bool Client::Finish(int wait_ms)
{
	if (m_fd < 0)
		return true;
	shutdown(m_fd, SHUT_WR);
	const uint64_t until = now_ns() + static_cast<uint64_t>(wait_ms) * 1000000ull;
	bool ended = false;
	unsigned char sink[4096];
	while (!ended)
	{
		const uint64_t now = now_ns();
		if (now >= until)
			break;
		pollfd p{m_fd, POLLIN, 0};
		const int ready = poll(&p, 1, static_cast<int>(std::min<uint64_t>((until - now) / 1000000ull + 1, 1000)));
		if (ready < 0 && errno != EINTR)
			break;
		if (ready <= 0)
			continue;
		const ssize_t got = read(m_fd, sink, sizeof(sink));
		if (got < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		ended = got <= 0; // EOF (or the connection reset: it is gone either way)
	}
	close(m_fd);
	m_fd = -1;
	m_pending.clear();
	return ended;
}

bool Client::Receive(Reply& reply, std::vector<uint8_t>* body, int wait_ms)
{
	if (m_pending.empty())
	{
		errno = EINVAL;
		return Fail("no request is waiting for its reply");
	}
	if (!::broker::recv_exact(m_fd, &reply, sizeof(reply), wait_ms))
		return Fail("no reply from the broker");
	const auto expected = m_pending.front();
	m_pending.pop_front();
	if (reply.magic != ::broker::kMagic || reply.version != ::broker::kVersion || reply.seq != expected.first ||
		reply.op != expected.second || reply.size > ::broker::kMaxBody + 4096)
	{
		m_error = Format("a reply out of order or not one (seq %u for %u, op %u for %u, size %u)", reply.seq, expected.first, reply.op,
			expected.second, reply.size);
		return false;
	}
	std::vector<uint8_t> sink;
	std::vector<uint8_t>& into = body ? *body : sink;
	into.resize(reply.size);
	if (reply.size != 0 && !::broker::recv_exact(m_fd, into.data(), reply.size, timeout_ms))
		return Fail("a reply's body broke off");
	return true;
}

int Client::Call(Op op, uint32_t handle, uint64_t offset, uint64_t length, uint32_t flags, const void* body, size_t size, Reply& reply,
	std::vector<uint8_t>* out, int wait_ms)
{
	if (!Send(op, handle, offset, length, flags, body, size) || !Receive(reply, out, wait_ms))
		return -1;
	return reply.error;
}

int Client::Hello(std::string& text, uint32_t& version, int wait_ms)
{
	Reply r;
	std::vector<uint8_t> body;
	const int e = Call(Op::hello, 0, 0, ::broker::kVersion, 0, nullptr, 0, r, &body, wait_ms);
	version = static_cast<uint32_t>(r.value);
	text.assign(body.begin(), body.end());
	return e;
}

int Client::Open(const std::string& path, uint32_t flags, uint32_t& handle, bool& direct)
{
	Reply r;
	const int e = Call(Op::open, 0, 0, 0, flags, path.data(), path.size(), r, nullptr, timeout_ms);
	handle = static_cast<uint32_t>(r.value);
	direct = r.flags & 1;
	return e;
}

int Client::Write(uint32_t handle, uint64_t offset, const void* data, size_t size, uint64_t& written)
{
	Reply r;
	const int e = Call(Op::write, handle, offset, 0, 0, data, size, r, nullptr, timeout_ms);
	written = r.value;
	return e;
}

int Client::Sync(uint32_t handle, int wait_ms)
{
	Reply r;
	return Call(Op::sync, handle, 0, 0, 0, nullptr, 0, r, nullptr, wait_ms);
}

int Client::Close(uint32_t handle)
{
	Reply r;
	return Call(Op::close, handle, 0, 0, 0, nullptr, 0, r, nullptr, timeout_ms);
}

int Client::Quit()
{
	Reply r;
	return Call(Op::quit, 0, 0, 0, 0, nullptr, 0, r, nullptr, timeout_ms);
}

int Client::Copy(uint32_t from, uint64_t fromOffset, uint32_t to, uint64_t toOffset, uint64_t length, uint64_t& copied, int wait_ms)
{
	::broker::CopyArgs a;
	a.from = from;
	a.from_offset = fromOffset;
	Reply r;
	const int e = Call(Op::copy, to, toOffset, length, 0, &a, sizeof(a), r, nullptr, wait_ms);
	copied = e < 0 ? 0 : r.value;
	return e;
}

int Launch(const std::string& elf, std::string& why, unsigned port)
{
	const int file = open(elf.c_str(), O_RDONLY);
	if (file < 0)
	{
		why = Format("%s can't be opened (errno %d: %s)", elf.c_str(), errno, std::strerror(errno));
		return -1;
	}
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		why = Format("no socket (errno %d: %s)", errno, std::strerror(errno));
		close(file);
		return -1;
	}
	const struct timeval timeout = {5, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	struct sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<unsigned short>(port));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
	{
		why = Format("no payload loader at 127.0.0.1:%u (errno %d: %s)", port, errno, std::strerror(errno));
		close(fd);
		close(file);
		return -1;
	}
	const int one = 1, buffer = 4 << 20;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
	std::vector<uint8_t> chunk(64 << 10);
	size_t sent = 0;
	for (;;)
	{
		const ssize_t got = read(file, chunk.data(), chunk.size());
		if (got == 0)
			break;
		if (got < 0 || !::broker::send_all(fd, chunk.data(), static_cast<size_t>(got), 5000))
		{
			why = Format("sending %s to the payload loader broke off after %zu bytes (errno %d: %s)", elf.c_str(), sent, errno,
				std::strerror(errno));
			close(fd);
			close(file);
			return -1;
		}
		sent += static_cast<size_t>(got);
	}
	close(file);
	why = Format("%s (%zu bytes) sent to the payload loader at 127.0.0.1:%u", elf.c_str(), sent, port);
	return fd;
}

// --------------------------------------------------------------------------------- the setting

bool Enabled()
{
	{
		std::lock_guard<std::mutex> lock(s_test_mutex);
		if (s_test_connect)
			return s_test_enabled;
	}
#if defined(__PROSPERO__) && !defined(PS5_BROKER_TEST)
	return Host::GetBaseBoolSettingValue("PS5", "WriteBroker", false);
#else
	return false;
#endif
}

bool Wanted(const std::string& path)
{
	std::string p;
	return Enabled() && BrokerPath(path, p, true);
}

void SetForTest(Connector connect, const std::string& root, bool enabled)
{
	std::lock_guard<std::mutex> lock(s_test_mutex);
	s_test_connect = std::move(connect);
	s_test_root = root;
	s_test_enabled = enabled;
}

// ------------------------------------------------------------------------------------- session

Session::Session(const char* what) : m_what(what) {}

Session::~Session()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_client)
	{
		const int e = m_client->Quit();
		if (e != 0)
			BROKER_LOG("broker: %s: its quit: %s", m_what, Said(m_client.get(), e).c_str());
		m_client->Finish(kFinishMs);
		m_client.reset();
	}
	if (m_tried && (m_files != 0 || m_fellBack != 0))
		BROKER_LOG("broker: %s: %s", m_what, Summary().c_str());
}

std::string Session::Summary() const
{
	return Format("%u files, %llu MiB in %.1f s through the broker (%.1f MiB/s), %u written by the title after all", m_files,
		static_cast<unsigned long long>(m_bytes >> 20), static_cast<double>(m_ns) / 1e9, MiBps(m_bytes, m_ns), m_fellBack);
}

// (m_mutex held) Started at the first file; false: the title writes.
bool Session::Up()
{
	if (m_tried)
		return m_client != nullptr;
	m_tried = true;
	if (!Enabled())
		return false;
	std::string launched, text;
	const int fd = TheConnector()(launched);
	if (fd < 0)
	{
		BROKER_LOG("broker: %s: not through the broker: it could not be started: %s; the title writes the files itself", m_what,
			launched.c_str());
		return false;
	}
	m_client = std::make_unique<Client>(fd);
	m_client->timeout_ms = kReplyMs;
	uint32_t version = 0;
	const int e = m_client->Hello(text, version, kHelloMs);
	if (e != 0 || version != ::broker::kVersion)
	{
		BROKER_LOG("broker: %s: not through the broker: %s; the title writes the files itself", m_what,
			e != 0 ? ("it didn't answer (" + Said(m_client.get(), e) + ")").c_str() :
					 ("it is of another version (" + std::to_string(version) + ")").c_str());
		m_client->Finish(kFinishMs);
		m_client.reset();
		return false;
	}
	BROKER_LOG("broker: %s: writing through the broker (%s; %s)", m_what, launched.c_str(), text.c_str());
	return true;
}

// (m_mutex held) A request failed: the rest written by the title; the broker ended first (what it
// was sent carried out), so nothing of it writes into a file the title writes again.
void Session::Drop(const std::string& why)
{
	BROKER_LOG("broker: %s: the broker stops: %s; the title writes this file again and the rest itself", m_what, why.c_str());
	if (m_client && !m_client->Finish(kFinishMs))
		BROKER_LOG("broker: %s: the broker didn't end within %d s", m_what, kFinishMs / 1000);
	m_client.reset();
}

bool Session::Copy(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
	const std::function<void(uint64_t)>& moved, std::string* why)
{
	why->clear();
	std::string f, t;
	if (!BrokerPath(from, f, false) || !BrokerPath(to, t, true))
		return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!Up())
		return false;
	Client& c = *m_client;
	const uint64_t began = now_ns();
	uint32_t in = 0, out = 0;
	bool direct = false;
	int e = c.Open(f, ::broker::open_read, in, direct);
	if (e != 0)
	{
		*why = "opening " + from + ": " + Said(&c, e);
		if (e < 0)
			Drop(*why);
		m_fellBack++;
		return false;
	}
	e = c.Open(t, ::broker::open_write | ::broker::open_create | ::broker::open_truncate | ::broker::open_direct, out, direct);
	if (e != 0)
	{
		*why = "opening " + to + ": " + Said(&c, e);
		if (e < 0)
			Drop(*why);
		else
			c.Close(in);
		m_fellBack++;
		return false;
	}
	uint64_t at = 0;
	bool stopped = false;
	while (e == 0 && at < size)
	{
		if (stop && stop())
		{
			stopped = true;
			break;
		}
		const uint64_t n = std::min(size - at, kCopyPiece);
		uint64_t got = 0;
		e = c.Copy(in, at, out, at, n, got, kReplyMs);
		if (e != 0)
			*why = "copying " + from + " at " + std::to_string(at) + ": " + Said(&c, e);
		else if (got != n)
		{
			*why = from + " ended at " + std::to_string(at + got) + " of " + std::to_string(size) + " bytes (it changed while it was copied)";
			e = EIO;
		}
		else
		{
			at += got;
			if (moved)
				moved(got);
		}
	}
	if (e == 0 && !stopped && (e = c.Sync(out, kSyncMs)) != 0)
		*why = "syncing " + to + ": " + Said(&c, e);
	if (e >= 0 && c.Fd() >= 0)
	{
		const int closedIn = c.Close(in), closedOut = c.Close(out);
		if (e == 0 && !stopped && (closedOut != 0 || closedIn != 0))
		{
			e = closedOut != 0 ? closedOut : closedIn;
			*why = "closing " + std::string(closedOut != 0 ? to : from) + ": " + Said(&c, e);
		}
	}
	if (stopped)
	{
		*why = "stopped";
		return false;
	}
	struct stat there;
	if (e == 0 && (stat(to.c_str(), &there) != 0 || static_cast<uint64_t>(there.st_size) != size))
	{
		*why = to + ": not the size it was copied at (" + std::to_string(size) + " bytes)";
		e = EIO;
	}
	if (e != 0)
	{
		m_fellBack++;
		// A broken connection, or an error the broker may meet again: let it end, write the rest.
		Drop(*why);
		return false;
	}
	m_bytes += size;
	m_ns += now_ns() - began;
	m_files++;
	return true;
}

std::unique_ptr<File> Session::Create(const std::string& path, std::string* why)
{
	why->clear();
	std::string p;
	if (!BrokerPath(path, p, true))
		return nullptr;
	std::unique_lock<std::mutex> lock(m_mutex);
	if (!Up())
		return nullptr;
	uint32_t handle = 0;
	bool direct = false;
	const int e = m_client->Open(p, ::broker::open_write | ::broker::open_create | ::broker::open_truncate, handle, direct);
	if (e != 0)
	{
		*why = "opening " + path + ": " + Said(m_client.get(), e);
		if (e < 0)
			Drop(*why);
		else
			BROKER_LOG("broker: %s: %s; the title writes it itself", m_what, why->c_str());
		m_fellBack++;
		return nullptr;
	}
	return std::unique_ptr<File>(new File(*this, std::move(lock), path, handle));
}

// ---------------------------------------------------------------------------------------- file

File::File(Session& s, std::unique_lock<std::mutex> lock, const std::string& local, uint32_t handle)
	: m_session(s), m_lock(std::move(lock)), m_local(local), m_handle(handle), m_began(now_ns())
{
	m_fill.reserve(kWritePiece);
}

File::~File()
{
	if (m_handle != 0 || m_fd >= 0)
	{
		std::string why;
		Finish(&why);
	}
}

bool File::LocalWrite(const void* data, size_t size, uint64_t offset)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	size_t done = 0;
	while (done < size)
	{
		const ssize_t w = pwrite(m_fd, p + done, size - done, static_cast<off_t>(offset + done));
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
		{
			m_error = "cannot write " + m_local + ": " + (w < 0 ? std::strerror(errno) : "the drive is full");
			return false;
		}
		done += static_cast<size_t>(w);
	}
	return true;
}

// The broker failed: let it end (what it got is carried out), then the pieces it didn't confirm and
// everything after are written by the title.
bool File::FallBack(const std::string& why)
{
	m_session.m_fellBack++;
	m_session.Drop(why);
	m_handle = 0;
	m_fd = open(m_local.c_str(), O_WRONLY | O_CREAT, 0666);
	if (m_fd < 0)
	{
		m_error = "cannot write " + m_local + ": " + std::strerror(errno);
		m_failed = true;
		return false;
	}
	for (const Piece& piece : m_sent)
	{
		if (!LocalWrite(piece.bytes.data(), piece.bytes.size(), piece.offset))
		{
			m_failed = true;
			return false;
		}
	}
	m_sent.clear();
	return true;
}

// Sends what is filled (all: and takes every answer); false: the file failed.
bool File::Flush(bool all)
{
	Client* const c = m_session.m_client.get();
	if (!m_fill.empty())
	{
		const uint64_t offset = m_at - m_fill.size();
		if (m_handle == 0)
		{
			if (!LocalWrite(m_fill.data(), m_fill.size(), offset))
			{
				m_failed = true;
				return false;
			}
			m_fill.clear();
		}
		else
		{
			Piece piece;
			piece.offset = offset;
			piece.bytes.swap(m_fill);
			m_fill.clear();
			m_fill.reserve(kWritePiece);
			const bool sent = c->Send(Op::write, m_handle, piece.offset, 0, 0, piece.bytes.data(), piece.bytes.size());
			m_sent.push_back(std::move(piece));
			if (!sent)
				return FallBack("writing " + m_local + ": " + c->Error());
		}
	}
	while (m_handle != 0 && !m_sent.empty() && (all || m_sent.size() >= kAhead))
	{
		Reply r;
		if (!c->Receive(r, nullptr, kReplyMs))
			return FallBack("writing " + m_local + ": " + c->Error());
		const Piece& piece = m_sent.front();
		if (r.error != 0 || r.value != piece.bytes.size())
			return FallBack("writing " + m_local + " at " + std::to_string(piece.offset) + ": " +
							(r.error != 0 ? std::string(std::strerror(r.error)) : std::string("a short write")));
		m_sent.pop_front();
	}
	return true;
}

bool File::Write(const void* data, size_t size)
{
	if (m_failed)
		return false;
	const uint8_t* p = static_cast<const uint8_t*>(data);
	while (size != 0)
	{
		const size_t n = std::min(size, kWritePiece - m_fill.size());
		m_fill.insert(m_fill.end(), p, p + n);
		m_at += n;
		p += n;
		size -= n;
		if (m_fill.size() == kWritePiece && !Flush(false))
			return false;
	}
	return true;
}

bool File::Finish(std::string* why)
{
	bool ok = !m_failed && Flush(true);
	if (ok && m_handle != 0)
	{
		Client* const c = m_session.m_client.get();
		int e = c->Sync(m_handle, kSyncMs);
		if (e == 0)
			e = c->Close(m_handle);
		if (e != 0)
		{
			// (Everything was confirmed written: only the sync or close failed. The title syncs it.)
			const std::string said = "finishing " + m_local + ": " + Said(c, e);
			ok = FallBack(said);
		}
		else
		{
			m_handle = 0;
			m_session.m_bytes += m_at;
			m_session.m_ns += now_ns() - m_began;
			m_session.m_files++;
		}
	}
	if (m_fd >= 0)
	{
		if (ok && fsync(m_fd) != 0 && errno != EINVAL)
		{
			m_error = "cannot write " + m_local + ": " + std::strerror(errno);
			ok = false;
		}
		if (close(m_fd) != 0 && ok)
		{
			m_error = "cannot write " + m_local + ": " + std::strerror(errno);
			ok = false;
		}
		m_fd = -1;
	}
	m_handle = 0;
	m_failed = !ok;
	if (!ok && why)
		*why = m_error;
	if (m_lock.owns_lock())
		m_lock.unlock();
	return ok;
}
} // namespace PS5Broker

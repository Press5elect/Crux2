// Crux2 - the write broker's client: large writes to the console's storage done by a payload.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// The Client and Launch are taken from OGBox (Press5elect, the same author: ps5/src/platform/ps5_broker.*
// at db63d46), without its storage write test.
//
// The console holds a title's writes to its storage back after a burst of about 1.3 GiB (242 MiB/s,
// then about 2 MiB/s; the budget is the process's: PS5_PayloadSDK platform/docs/PROBE.md). The
// broker (ps5/broker, write-broker.elf in the title folder) is a payload, a process of its own: the
// title sends it to the payload loader on 127.0.0.1:9021, as it sends the elevation helper, and then
// speaks ps5/broker/protocol.hpp with it over the same connection. OGBox measured it on the console
// at about 200 MiB/s past 2 GiB, where the title's own writes had fallen to a few MiB/s.
//
// Crux2 uses it ([PS5] WriteBroker, Settings > PlayStation 5 > Fast writes to the console's storage;
// off by default: untested on a console in Crux2) for what it writes in GBs to /data/: a texture
// pack moved or copied there (the broker reads and writes the file itself: COPY), a pack unpacked
// there as it downloads and a disc's image made there (the title's bytes sent to it: WRITE). Every
// one falls back to the title's own writes when the broker can't be started or fails, without
// losing a byte. USB drives have no such budget: nothing to them goes through it.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../broker/protocol.hpp"

namespace PS5Broker
{
using ::broker::Op;
using ::broker::Reply;

/// One connection to the broker. Requests may be sent ahead of their replies (pipelined); the
/// replies are taken in the order the requests went, and one out of that order is a protocol error.
/// The calls return 0, the broker's errno, or -1 when the connection failed (`Error` says how).
class Client
{
public:
	explicit Client(int fd = -1) : m_fd(fd) {}
	~Client();
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

	int Fd() const { return m_fd; }
	const std::string& Error() const { return m_error; }
	size_t InFlight() const { return m_pending.size(); }

	bool Send(Op op, uint32_t handle, uint64_t offset, uint64_t length, uint32_t flags, const void* body, size_t size);
	/// No more requests: the sending side shut, the broker's answers read and let go until it ends (it
	/// carries out every request it got whole first), for at most wait_ms; then closed. false: it
	/// hadn't ended by then.
	bool Finish(int wait_ms);
	/// The next reply (and its body, when it has one and body is given).
	bool Receive(Reply& reply, std::vector<uint8_t>* body, int wait_ms);
	/// Send, then its reply; -1 or the reply's error.
	int Call(Op op, uint32_t handle, uint64_t offset, uint64_t length, uint32_t flags, const void* body, size_t size, Reply& reply,
		std::vector<uint8_t>* out, int wait_ms);

	int Hello(std::string& text, uint32_t& version, int wait_ms);
	int Open(const std::string& path, uint32_t flags, uint32_t& handle, bool& direct);
	int Write(uint32_t handle, uint64_t offset, const void* data, size_t size, uint64_t& written);
	int Sync(uint32_t handle, int wait_ms);
	int Close(uint32_t handle);
	int Quit();
	/// The broker copies length bytes (at most kMaxBody) of the file `from` at fromOffset into `to` at
	/// toOffset itself; copied: what it wrote (short only at from's end). ENOSYS: a broker without COPY.
	int Copy(uint32_t from, uint64_t fromOffset, uint32_t to, uint64_t toOffset, uint64_t length, uint64_t& copied, int wait_ms);

	int timeout_ms = 30000; // a reply's wait, for the calls without their own

private:
	int m_fd;
	uint32_t m_seq = 1;
	std::deque<std::pair<uint32_t, uint16_t>> m_pending; // seq, op
	std::string m_error;
	bool Fail(const char* what);
};

/// Connects to the payload loader on 127.0.0.1:port and sends it the broker's ELF: the connection
/// (the broker's once the loader has started it), or -1 with why.
int Launch(const std::string& elf, std::string& why, unsigned port = 9021);

/// The broker's connection, or -1 with why.
using Connector = std::function<int(std::string& why)>;

/// [PS5] WriteBroker (read when a job starts).
bool Enabled();
/// Whether a file at this path is written through the broker: the setting is on, the path is on the
/// console's storage (/data/) and the broker takes it.
bool Wanted(const std::string& path);

/// The PC tests': the broker is reached through `connect` (a socketpair to broker::serve on a
/// thread), paths are under `root` locally (the broker is given them without it), and the setting
/// is `enabled`. An empty connector puts the console's back.
void SetForTest(Connector connect, const std::string& root, bool enabled);

class File;

/// A broker for one job (a pack's move, an unpacking, a disc image): started at the first file that
/// wants it, told to quit when the job ends (the destructor). Not started again once it failed: the
/// rest of the job is written by the title. One file at a time (a mutex).
class Session
{
public:
	/// what: the job, for the boot log ("texture packs", "disc image").
	explicit Session(const char* what);
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	/// The broker copies `from` (size bytes) into `to` (created or emptied) and syncs it; moved is
	/// told each piece. false: not copied, why says why; the caller copies it itself, from its start.
	/// `from` must be a path the broker takes; else false at once (nothing done, why empty).
	bool Copy(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
		const std::function<void(uint64_t)>& moved, std::string* why);

	/// A file the title writes in order (offset 0 up), whose bytes the broker writes. The file is
	/// created (emptied) here. null: not through the broker (the caller writes it itself), why says
	/// why or is empty when the path isn't one it takes.
	std::unique_ptr<File> Create(const std::string& path, std::string* why);

	/// What it did, for the job's line in the boot log.
	std::string Summary() const;

private:
	friend class File;
	const char* m_what;
	mutable std::mutex m_mutex;
	std::unique_ptr<Client> m_client;
	bool m_tried = false;
	uint64_t m_bytes = 0, m_ns = 0;
	unsigned m_files = 0, m_fellBack = 0;
	bool Up();
	void Drop(const std::string& why);
};

/// A file written through a Session's broker, in order. Its bytes are sent in pieces of up to 8 MiB,
/// a few ahead of their answers; a piece is kept until the broker has written it. If the broker
/// fails, it is let end (what it got is written), the pieces it didn't confirm are written by the
/// title at their places, and the rest of the file is the title's own writes: no byte is lost.
class File
{
public:
	~File();
	bool Write(const void* data, size_t size);
	/// The bytes so far are in the file and it is synced and closed. false: why.
	bool Finish(std::string* why);
	/// Through the broker still (false: it fell back to the title's own writes).
	bool Brokered() const { return m_handle != 0; }
	uint64_t Size() const { return m_at; }

private:
	friend class Session;
	File(Session& s, std::unique_lock<std::mutex> lock, const std::string& local, uint32_t handle);
	struct Piece
	{
		uint64_t offset = 0;
		std::vector<uint8_t> bytes;
	};
	Session& m_session;
	std::unique_lock<std::mutex> m_lock;
	std::string m_local;
	uint32_t m_handle;      // 0: written by the title (m_fd)
	int m_fd = -1;
	uint64_t m_at = 0;      // the bytes given so far
	uint64_t m_began = 0;
	std::vector<uint8_t> m_fill;
	std::deque<Piece> m_sent; // sent, not yet confirmed
	bool m_failed = false;
	std::string m_error;
	bool Flush(bool all);
	bool FallBack(const std::string& why);
	bool LocalWrite(const void* data, size_t size, uint64_t offset);
};
} // namespace PS5Broker

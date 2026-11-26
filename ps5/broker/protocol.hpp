/*
 * Crux2 - the write broker's messages (ps5/broker): what the title and the payload say to each
 * other over the connection the payload loader handed the payload.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 * Taken from OGBox (Press5elect, the same author: ps5/broker at db63d46).
 *
 * The console holds a title's writes to its storage back after a burst of about 1.3 GiB
 * (PS5_PayloadSDK platform/docs/PROBE.md, "Sustained writes"); the budget is the process's. The
 * broker is a payload, a process of its own, that does the title's writes for it (in OGBox it kept
 * about 200 MiB/s past 2 GiB on the console). Crux2's side: ps5/frontend/ps5_broker.h. The version
 * numbers below are OGBox's, where each request came in.
 *
 * The framing is the elevation helper's (ps5/elevation/protocol.hpp): fixed-width little-endian
 * frames, no pointers across the socket. Every request has a sequence number and gets one reply,
 * with the request's sequence number and an errno (0: done); requests on one connection are carried
 * out in the order they were sent. A request is a Request, then `size` bytes of body; a reply is a
 * Reply, then `size` bytes of body.
 *
 *   HELLO     length = the client's version          -> value = the broker's; body = a line of text
 *   OPEN      flags = OpenFlags, body = the path      -> value = a handle; body = OpenInfo (1.0.4)
 *   WRITE     handle, offset, body = the bytes        -> value = bytes written (all, or error set)
 *   READ      handle, offset, length                  -> value = bytes read; body = them (short at EOF)
 *   TRUNCATE  handle, length                          -> (ftruncate)
 *   SYNC      handle                                  -> (fsync)
 *   CLOSE     handle
 *   UNLINK    body = the path
 *   BENCH     body = BenchArgs then the path          -> body = BenchResult, then its steps
 *   QUIT                                              -> the broker answers, then ends
 *   COPY      handle = to, offset = to's offset, length (at most kMaxBody), body = CopyArgs
 *                                                     -> value = bytes copied (1.0.4: the data
 *             folder's move; the broker reads `from` and writes `to` itself, kCopyChunk at a time:
 *             the bytes don't cross the connection. Short only at from's end, or with error set:
 *             value is then what was written. A broker from before answers ENOSYS)
 *
 * Paths: only under /data/, /mnt/usb<n>/ and /mnt/ext<n>/, and none with a "." or ".." part.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace broker
{
inline constexpr std::uint32_t kMagic = 0x31524257; // "WBR1"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint32_t kMaxBody = 64u << 20;      // the largest WRITE (and READ)
inline constexpr std::uint32_t kMaxPath = 1024;
inline constexpr unsigned kMaxHandles = 64;               // 1.0.4: 16 before (the cache drives' files)
inline constexpr unsigned kMaxSteps = 64;                 // BENCH's steps (of 256 MiB: 16 GiB)
inline constexpr std::uint64_t kStep = 256ull << 20;
inline constexpr std::uint32_t kCopyChunk = 16u << 20;    // COPY's pread and pwrite, at most this a time

enum class Op : std::uint16_t
{
    hello = 1,
    open = 2,
    write = 3,
    read = 4,
    truncate = 5,
    sync = 6,
    close = 7,
    unlink = 8,
    bench = 9,
    quit = 10,
    copy = 11, // 1.0.4
};

// The broker's own open flags (the title's and the host's O_ values differ).
enum OpenFlags : std::uint32_t
{
    open_read = 1,
    open_write = 2,
    open_create = 4,
    open_truncate = 8,
    open_direct = 16, // O_DIRECT, when the file system takes it (else opened without: Reply::flags says)
};

// 1.0.4: OPEN's answer's body: the file opened, so the title can tell it is the one it means (the
// cache drives' write-back: a file renamed under it).
struct OpenInfo
{
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
};
static_assert(sizeof(OpenInfo) == 16);

// 1.0.4: COPY's body: the file it reads (a handle of this connection) and where from.
struct CopyArgs
{
    std::uint32_t from = 0;
    std::uint32_t reserved = 0;
    std::uint64_t from_offset = 0;
};
static_assert(sizeof(CopyArgs) == 16);

struct Request
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t op = 0;
    std::uint32_t seq = 0;
    std::uint32_t handle = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t size = 0;  // body bytes after this
    std::uint32_t flags = 0;
};
static_assert(sizeof(Request) == 40);

struct Reply
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t op = 0;
    std::uint32_t seq = 0;
    std::int32_t error = 0;  // an errno; 0: done
    std::uint64_t value = 0;
    std::uint32_t size = 0;  // body bytes after this
    std::uint32_t flags = 0; // OPEN: 1 when O_DIRECT is in use
};
static_assert(sizeof(Reply) == 32);

enum class Pattern : std::uint32_t
{
    seq = 0,     // `chunk` bytes at a time from 0, timed per 256 MiB
    rand4k = 1,  // 4 KiB at random 4 KiB-aligned offsets in a file of `space` bytes
    rand16k = 2, // the same, 16 KiB
};

enum BenchFlags : std::uint32_t
{
    bench_direct = 1, // O_DIRECT, when the file system takes it
    bench_unlink = 2, // delete the file afterwards
};

struct BenchArgs
{
    std::uint64_t total = 0;  // seq: bytes to write at most
    std::uint64_t space = 0;  // rand: the file's size
    std::uint32_t chunk = 0;  // seq: bytes a write
    std::uint32_t pattern = 0;
    std::uint32_t seconds = 0; // stop after this long (checked between writes)
    std::uint32_t flags = 0;
};
static_assert(sizeof(BenchArgs) == 32);

struct BenchStep
{
    std::uint64_t bytes = 0;
    std::uint64_t ns = 0;
};

struct BenchResult
{
    std::uint64_t bytes = 0;   // written (before the sync)
    std::uint64_t ns = 0;      // the writes' time
    std::uint64_t sync_ns = 0; // the fsync after them
    std::uint64_t prep_ns = 0; // opening (and, rand, sizing the file)
    std::uint64_t ops = 0;     // writes made
    std::uint64_t p50_ns = 0, p95_ns = 0, p99_ns = 0, max_ns = 0; // one write's time
    std::int32_t error = 0;    // the first errno met (the bench stops there)
    std::uint32_t direct = 0;  // 1: O_DIRECT was in use
    std::uint32_t steps = 0;   // BenchSteps after this (seq)
    std::uint32_t capped = 0;  // 1: stopped by the time limit
};
static_assert(sizeof(BenchResult) == 88);

// Paths the broker accepts: under /data/, /mnt/usb<n>/ or /mnt/ext<n>/, absolute, no NUL, no empty,
// "." or ".." part, something after the prefix.
constexpr bool path_allowed(const char *path, std::size_t length) noexcept
{
    if (length < 2 || length > kMaxPath || path[0] != '/')
        return false;
    for (std::size_t i = 0; i < length; i++)
        if (path[i] == '\0')
            return false;
    std::size_t rest = 0;
    const auto starts = [&](const char *prefix) {
        std::size_t n = 0;
        while (prefix[n] != '\0')
            n++;
        if (length < n)
            return std::size_t(0);
        for (std::size_t i = 0; i < n; i++)
            if (path[i] != prefix[i])
                return std::size_t(0);
        return n;
    };
    if (const std::size_t data = starts("/data/"))
        rest = data;
    else
    {
        std::size_t n = starts("/mnt/usb");
        if (n == 0)
            n = starts("/mnt/ext");
        if (n == 0)
            return false;
        std::size_t digits = 0;
        while (n + digits < length && path[n + digits] >= '0' && path[n + digits] <= '9')
            digits++;
        if (digits == 0 || digits > 3 || n + digits >= length || path[n + digits] != '/')
            return false;
        rest = n + digits + 1;
    }
    if (rest >= length)
        return false;
    // every part from the first: not empty, not "." or ".."
    std::size_t start = 1;
    for (std::size_t i = 1; i <= length; i++)
    {
        if (i == length || path[i] == '/')
        {
            const std::size_t n = i - start;
            if (n == 0 && i != length)
                return false;
            if (n == 1 && path[start] == '.')
                return false;
            if (n == 2 && path[start] == '.' && path[start + 1] == '.')
                return false;
            start = i + 1;
        }
    }
    return path[length - 1] != '/';
}

// The test's bytes: each 8-byte word from its place in the file and a salt, so any range can be
// checked without keeping what was sent.
constexpr std::uint64_t mix64(std::uint64_t x) noexcept
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

inline void fill_pattern(void *buffer, std::size_t length, std::uint64_t offset, std::uint64_t salt) noexcept
{
    auto *bytes = static_cast<unsigned char *>(buffer);
    for (std::size_t i = 0; i < length; i += 8)
    {
        const std::uint64_t word = mix64(((offset + i) >> 3) ^ (salt * 0x9e3779b97f4a7c15ull));
        const std::size_t n = length - i < 8 ? length - i : 8;
        std::memcpy(bytes + i, &word, n);
    }
}

// FNV-1a, 64 bits: what the verification logs for a range.
inline std::uint64_t checksum(const void *data, std::size_t length, std::uint64_t h = 0xcbf29ce484222325ull) noexcept
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < length; i++)
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    return h;
}

// Both ends use this loop (the elevation helper's): TCP splits frames. false: an error or EOF.
template <typename Byte, typename Operation>
bool transfer(Byte *bytes, std::size_t size, Operation operation) noexcept
{
    while (size != 0)
    {
        const auto count = operation(bytes, size);
        if (count <= 0 || static_cast<std::size_t>(count) > size)
            return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace broker

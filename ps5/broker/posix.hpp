/*
 * Crux2 - the write broker's file and socket work, shared by the payload (ps5/broker/payload) and
 * the title's client (ps5/frontend/ps5_broker.cpp).
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 * Taken from OGBox (Press5elect, the same author: ps5/broker at db63d46).
 *
 * POSIX only (the console's FreeBSD libc in the payload and the title; the PC test's libc).
 */
#pragma once

#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

namespace broker
{
inline std::uint64_t now_ns() noexcept
{
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(t.tv_nsec);
}

// ---- the socket

// All of it, or false (an error, EOF, or nothing for timeout_ms: errno ETIMEDOUT then).
inline bool recv_exact(int fd, void *data, std::size_t size, int timeout_ms) noexcept
{
    return transfer(static_cast<unsigned char *>(data), size, [fd, timeout_ms](unsigned char *bytes, std::size_t n) -> long {
        for (;;)
        {
            pollfd p{fd, POLLIN, 0};
            const int ready = poll(&p, 1, timeout_ms);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready == 0)
                errno = ETIMEDOUT;
            if (ready <= 0)
                return -1;
            const ssize_t got = read(fd, bytes, n);
            if (got < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (got == 0)
                errno = ECONNRESET;
            return static_cast<long>(got);
        }
    });
}

inline bool send_all(int fd, const void *data, std::size_t size, int timeout_ms) noexcept
{
    return transfer(static_cast<const unsigned char *>(data), size, [fd, timeout_ms](const unsigned char *bytes, std::size_t n) -> long {
        for (;;)
        {
            pollfd p{fd, POLLOUT, 0};
            const int ready = poll(&p, 1, timeout_ms);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready == 0)
                errno = ETIMEDOUT;
            if (ready <= 0)
                return -1;
            ssize_t sent = send(fd, bytes, n, MSG_NOSIGNAL);
            if (sent < 0 && errno == ENOTSOCK)
                sent = write(fd, bytes, n); // (a pipe in a test)
            if (sent < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return static_cast<long>(sent);
        }
    });
}

// ---- files

// pwrite until all is written: a short pwrite goes on from where it stopped. cap (> 0, the PC
// test's) makes every call write at most that many bytes. 0, or the errno; *done the bytes written.
inline int pwrite_all(int fd, const void *data, std::size_t size, std::uint64_t offset, std::size_t cap,
                      std::size_t *done) noexcept
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::size_t at = 0;
    int error = 0;
    while (at < size)
    {
        std::size_t n = size - at;
        if (cap != 0 && n > cap)
            n = cap;
        const ssize_t wrote = pwrite(fd, bytes + at, n, static_cast<off_t>(offset + at));
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
        {
            error = wrote < 0 ? errno : EIO;
            break;
        }
        at += static_cast<std::size_t>(wrote);
    }
    if (done)
        *done = at;
    return error;
}

// pread until size or EOF. 0, or the errno; *done the bytes read.
inline int pread_all(int fd, void *data, std::size_t size, std::uint64_t offset, std::size_t *done) noexcept
{
    auto *bytes = static_cast<unsigned char *>(data);
    std::size_t at = 0;
    int error = 0;
    while (at < size)
    {
        const ssize_t got = pread(fd, bytes + at, size - at, static_cast<off_t>(offset + at));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0)
            error = errno;
        if (got <= 0)
            break;
        at += static_cast<std::size_t>(got);
    }
    if (done)
        *done = at;
    return error;
}

// OpenFlags to the system's; O_DIRECT left out again when the file system refuses it (*direct
// says which). 0, or the errno.
inline int open_file(const char *path, std::uint32_t flags, int *fd, bool *direct) noexcept
{
    int mode = (flags & open_write) ? ((flags & open_read) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & open_create)
        mode |= O_CREAT;
    if (flags & open_truncate)
        mode |= O_TRUNC;
    *direct = false;
    if ((flags & open_direct) && O_DIRECT != 0)
    {
        *fd = open(path, mode | O_DIRECT, 0666);
        if (*fd >= 0)
        {
            *direct = true;
            return 0;
        }
        if (errno != EINVAL)
            return errno;
    }
    *fd = open(path, mode, 0666);
    return *fd >= 0 ? 0 : errno;
}

// A write refused only because of O_DIRECT (EINVAL: its alignment): go on without it.
inline bool drop_direct(int fd) noexcept
{
    const int now = fcntl(fd, F_GETFL);
    return O_DIRECT != 0 && now >= 0 && (now & O_DIRECT) && fcntl(fd, F_SETFL, now & ~O_DIRECT) == 0;
}

inline void *aligned_buffer(std::size_t size) noexcept
{
    void *p = nullptr;
    if (posix_memalign(&p, 65536, size < 65536 ? 65536 : size) != 0)
        return nullptr;
    return p;
}

// ---- BENCH: the payload's own writes (and the title's, phases A and F)

struct BenchOut
{
    BenchResult r{};
    BenchStep steps[kMaxSteps]{};
};

using BenchProgress = void (*)(void *ctx, std::uint64_t bytes, std::uint64_t ops);

namespace detail
{
// One write's time, kept for the percentiles (up to 2M of them; the rest are counted, not kept).
struct Latencies
{
    std::uint64_t *at = nullptr;
    std::size_t count = 0, room = 0;
    void add(std::uint64_t ns) noexcept
    {
        if (count == room)
        {
            const std::size_t more = room ? room * 2 : 4096;
            if (more > (2u << 20))
                return;
            void *grown = std::realloc(at, more * sizeof(*at));
            if (!grown)
                return;
            at = static_cast<std::uint64_t *>(grown);
            room = more;
        }
        at[count++] = ns;
    }
    void finish(BenchResult &r) noexcept
    {
        if (count != 0)
        {
            std::sort(at, at + count);
            r.p50_ns = at[count * 50 / 100];
            r.p95_ns = at[count * 95 / 100];
            r.p99_ns = at[count * 99 / 100];
            r.max_ns = at[count - 1];
        }
        std::free(at);
        at = nullptr;
    }
};
} // namespace detail

inline void run_bench(const char *path, const BenchArgs &a, BenchOut &out, BenchProgress progress = nullptr,
                      void *ctx = nullptr, std::size_t io_cap = 0) noexcept
{
    out = BenchOut{};
    BenchResult &r = out.r;
    const auto pattern = static_cast<Pattern>(a.pattern);
    const bool random = pattern == Pattern::rand4k || pattern == Pattern::rand16k;
    const std::size_t chunk = random ? (pattern == Pattern::rand4k ? 4096u : 16384u) : a.chunk;
    if (chunk == 0 || chunk > kMaxBody || (random && a.space < chunk) || (!random && a.pattern != 0) ||
        (!random && a.total > kMaxSteps * kStep))
    {
        r.error = EINVAL;
        return;
    }
    const std::uint64_t t_open = now_ns();
    int fd = -1;
    bool direct = false;
    std::uint32_t flags = open_write | open_create | (random ? 0u : open_truncate);
    if (a.flags & bench_direct)
        flags |= open_direct;
    if (const int e = open_file(path, flags, &fd, &direct))
    {
        r.error = e;
        return;
    }
    if (random)
    {
        struct stat st{};
        if (fstat(fd, &st) == 0 && static_cast<std::uint64_t>(st.st_size) < a.space && ftruncate(fd, static_cast<off_t>(a.space)) != 0)
            r.error = errno;
    }
    void *buffer = aligned_buffer(chunk);
    if (!buffer && r.error == 0)
        r.error = ENOMEM;
    if (buffer)
        fill_pattern(buffer, chunk, 0, now_ns());
    r.prep_ns = now_ns() - t_open;

    detail::Latencies lat;
    const std::uint64_t limit = static_cast<std::uint64_t>(a.seconds) * 1000000000ull;
    const std::uint64_t t0 = now_ns();
    std::uint64_t step_start = t0, step_bytes = 0;
    std::uint64_t rng = now_ns() | 1;
    const std::uint64_t blocks = random ? a.space / chunk : 0;
    while (r.error == 0)
    {
        const std::uint64_t now = now_ns();
        if (limit != 0 && now - t0 >= limit)
        {
            r.capped = 1;
            break;
        }
        std::uint64_t offset = r.bytes;
        std::size_t n = chunk;
        if (random)
        {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            offset = ((rng * 0x2545f4914f6cdd1dull) >> 11) % blocks * chunk;
        }
        else
        {
            if (r.bytes >= a.total)
                break;
            n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, a.total - r.bytes));
        }
        std::size_t done = 0;
        int e = pwrite_all(fd, buffer, n, offset, io_cap, &done);
        if (e == EINVAL && direct && done == 0 && drop_direct(fd))
        {
            direct = false;
            e = pwrite_all(fd, buffer, n, offset, io_cap, &done);
        }
        const std::uint64_t end = now_ns();
        lat.add(end - now);
        r.ops++;
        r.bytes += done;
        step_bytes += done;
        if (e != 0)
            r.error = e;
        if (!random && (step_bytes >= kStep) && r.steps < kMaxSteps)
        {
            out.steps[r.steps++] = BenchStep{step_bytes, end - step_start};
            step_start = end;
            step_bytes = 0;
        }
        if (progress)
            progress(ctx, r.bytes, r.ops);
    }
    const std::uint64_t t_end = now_ns();
    r.ns = t_end - t0;
    if (!random && step_bytes != 0 && r.steps < kMaxSteps)
        out.steps[r.steps++] = BenchStep{step_bytes, t_end - step_start};
    if (fsync(fd) != 0 && r.error == 0)
        r.error = errno;
    r.sync_ns = now_ns() - t_end;
    close(fd);
    r.direct = direct ? 1 : 0;
    lat.finish(r);
    std::free(buffer);
    if (a.flags & bench_unlink)
        unlink(path);
}
} // namespace broker

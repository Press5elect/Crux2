/*
 * Crux2 - the write broker's request handler (server.hpp; the messages: protocol.hpp).
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 * Taken from OGBox (Press5elect, the same author: ps5/broker at db63d46).
 *
 * One request at a time, in the order they came: its body read whole, the work done, one reply.
 * Files are the broker's own descriptors, a handle each (1..16). Buffers come from malloc
 * (a payload is refused sceKernelAllocateDirectMemory).
 */
#include "server.hpp"

#include "posix.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace broker
{
namespace
{
struct Server
{
    const ServerOptions &opt;
    int in, out;
    int fds[kMaxHandles];
    unsigned char *body = nullptr;
    std::size_t room = 0;
    void *copying = nullptr;          // COPY's buffer (kCopyChunk, aligned for O_DIRECT), made at the first
    unsigned served = 0;
    bool silent = false;              // the PC test's death: the request carried out, not answered

    Server(const ServerOptions &o, int i, int w) : opt(o), in(i), out(w)
    {
        for (int &fd : fds)
            fd = -1;
    }
    ~Server()
    {
        for (int fd : fds)
            if (fd >= 0)
                close(fd);
        std::free(body);
        std::free(copying);
    }

    void log(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        if (!opt.log)
            return;
        char line[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        opt.log(line);
    }

    bool grow(std::size_t size)
    {
        if (size <= room)
            return true;
        void *more = std::realloc(body, size);
        if (!more)
            return false;
        body = static_cast<unsigned char *>(more);
        room = size;
        return true;
    }

    void handle_silently(const Request &q)
    {
        silent = true;
        End end = End::closed;
        handle(q, end);
    }

    bool reply(const Request &q, int error, std::uint64_t value = 0, const void *data = nullptr, std::uint32_t size = 0,
               std::uint32_t flags = 0)
    {
        if (silent)
            return true;
        Reply r;
        r.op = q.op;
        r.seq = q.seq;
        r.error = error;
        r.value = value;
        r.size = data ? size : 0;
        r.flags = flags;
        return send_all(out, &r, sizeof(r), opt.message_ms) && (r.size == 0 || send_all(out, data, r.size, opt.message_ms));
    }

    // The path in the body: checked, then (the PC test) put under its root. "" when refused.
    bool path(const unsigned char *p, std::size_t n, char *real, std::size_t room_real)
    {
        if (!path_allowed(reinterpret_cast<const char *>(p), n))
            return false;
        const char *root = opt.root ? opt.root : "";
        const int wrote = std::snprintf(real, room_real, "%s%.*s", root, static_cast<int>(n), reinterpret_cast<const char *>(p));
        return wrote > 0 && static_cast<std::size_t>(wrote) < room_real;
    }

    int file(std::uint32_t handle) const
    {
        return handle >= 1 && handle <= kMaxHandles ? fds[handle - 1] : -1;
    }

    // false: the connection is gone
    bool handle(const Request &q, End &end)
    {
        char real[kMaxPath + 512];
        switch (static_cast<Op>(q.op))
        {
        case Op::hello: {
            char text[96];
            const int n = std::snprintf(text, sizeof(text), "write-broker %u pid=%d uid=%d", kVersion, static_cast<int>(getpid()),
                                        static_cast<int>(getuid()));
            return reply(q, 0, kVersion, text, static_cast<std::uint32_t>(n));
        }
        case Op::open: {
            if (!path(body, q.size, real, sizeof(real)))
                return reply(q, EACCES);
            unsigned slot = 0;
            while (slot < kMaxHandles && fds[slot] >= 0)
                slot++;
            if (slot == kMaxHandles)
                return reply(q, EMFILE);
            int fd = -1;
            bool direct = false;
            if (const int e = open_file(real, q.flags, &fd, &direct))
                return reply(q, e);
            fds[slot] = fd;
            OpenInfo info;
            struct stat st;
            if (fstat(fd, &st) == 0)
            {
                info.dev = static_cast<std::uint64_t>(st.st_dev);
                info.ino = static_cast<std::uint64_t>(st.st_ino);
            }
            return reply(q, 0, slot + 1, &info, sizeof(info), direct ? 1 : 0);
        }
        case Op::write: {
            const int fd = file(q.handle);
            if (fd < 0)
                return reply(q, EBADF);
            std::size_t done = 0;
            int e = pwrite_all(fd, body, q.size, q.offset, opt.io_cap, &done);
            if (e == EINVAL && done == 0 && drop_direct(fd))
                e = pwrite_all(fd, body, q.size, q.offset, opt.io_cap, &done);
            return reply(q, e, done);
        }
        case Op::read: {
            const int fd = file(q.handle);
            if (fd < 0)
                return reply(q, EBADF);
            if (q.length > kMaxBody)
                return reply(q, EMSGSIZE);
            if (!grow(static_cast<std::size_t>(q.length)))
                return reply(q, ENOMEM);
            std::size_t done = 0;
            const int e = pread_all(fd, body, static_cast<std::size_t>(q.length), q.offset, &done);
            return reply(q, e, done, body, static_cast<std::uint32_t>(done));
        }
        case Op::truncate: {
            const int fd = file(q.handle);
            if (fd < 0)
                return reply(q, EBADF);
            return reply(q, ftruncate(fd, static_cast<off_t>(q.length)) == 0 ? 0 : errno);
        }
        case Op::sync: {
            const int fd = file(q.handle);
            if (fd < 0)
                return reply(q, EBADF);
            return reply(q, fsync(fd) == 0 ? 0 : errno);
        }
        case Op::close: {
            const int fd = file(q.handle);
            if (fd < 0)
                return reply(q, EBADF);
            fds[q.handle - 1] = -1;
            return reply(q, close(fd) == 0 ? 0 : errno);
        }
        case Op::unlink:
            if (!path(body, q.size, real, sizeof(real)))
                return reply(q, EACCES);
            return reply(q, unlink(real) == 0 ? 0 : errno);
        case Op::bench: {
            BenchArgs a;
            if (q.size < sizeof(a))
                return reply(q, EINVAL);
            std::memcpy(&a, body, sizeof(a));
            if (!path(body + sizeof(a), q.size - sizeof(a), real, sizeof(real)))
                return reply(q, EACCES);
            if (a.seconds == 0 || a.seconds > 600)
                return reply(q, EINVAL);
            auto *o = static_cast<BenchOut *>(std::malloc(sizeof(BenchOut)));
            if (!o)
                return reply(q, ENOMEM);
            run_bench(real, a, *o, nullptr, nullptr, opt.io_cap);
            log("bench pattern %u: %llu bytes, %llu writes in %.2f s, error %d", a.pattern,
                static_cast<unsigned long long>(o->r.bytes), static_cast<unsigned long long>(o->r.ops), o->r.ns / 1e9, o->r.error);
            unsigned char result[sizeof(BenchResult) + sizeof(o->steps)];
            std::memcpy(result, &o->r, sizeof(o->r));
            std::memcpy(result + sizeof(o->r), o->steps, o->r.steps * sizeof(BenchStep));
            const auto n = static_cast<std::uint32_t>(sizeof(o->r) + o->r.steps * sizeof(BenchStep));
            std::free(o);
            return reply(q, 0, 0, result, n);
        }
        case Op::quit:
            end = End::quit;
            return reply(q, 0);
        case Op::copy: {
            CopyArgs a;
            if (q.size < sizeof(a))
                return reply(q, EINVAL);
            std::memcpy(&a, body, sizeof(a));
            const int from = file(a.from), to = file(q.handle);
            if (from < 0 || to < 0)
                return reply(q, EBADF);
            if (q.length > kMaxBody)
                return reply(q, EMSGSIZE);
            if (!copying && !(copying = aligned_buffer(kCopyChunk)))
                return reply(q, ENOMEM);
            // a piece read, then written; on to the next until length, from's end or an error
            std::uint64_t done = 0;
            int e = 0;
            while (done < q.length)
            {
                const std::size_t n = static_cast<std::size_t>(q.length - done < kCopyChunk ? q.length - done : kCopyChunk);
                std::size_t got = 0, wrote = 0;
                e = pread_all(from, copying, n, a.from_offset + done, &got);
                if (e != 0 || got == 0)
                    break;
                e = pwrite_all(to, copying, got, q.offset + done, opt.io_cap, &wrote);
                if (e == EINVAL && drop_direct(to))   // (O_DIRECT's alignment: a file's last piece)
                {
                    std::size_t more = 0;
                    e = pwrite_all(to, static_cast<unsigned char *>(copying) + wrote, got - wrote, q.offset + done + wrote, opt.io_cap,
                                   &more);
                    wrote += more;
                }
                done += wrote;
                if (e != 0 || got < n)
                    break;
            }
            return reply(q, e, done);
        }
        }
        return reply(q, ENOSYS);
    }

    End run()
    {
        for (;;)
        {
            Request q;
            {
                pollfd p{in, POLLIN, 0};
                int ready;
                do
                    ready = poll(&p, 1, opt.idle_ms);
                while (ready < 0 && errno == EINTR);
                if (ready == 0)
                {
                    log("idle for %d s: ending", opt.idle_ms / 1000);
                    return End::idle;
                }
            }
            if (!recv_exact(in, &q, sizeof(q), opt.message_ms))
                return End::closed;
            if (q.magic != kMagic || q.version != kVersion)
            {
                log("not a request (magic %#x, version %u): ending", q.magic, q.version);
                reply(q, EPROTO);
                return End::protocol;
            }
            // the body: whole (a WRITE's bytes are written in one go), or drained when too large
            const std::size_t limit = kMaxBody + sizeof(BenchArgs) + kMaxPath;
            if (q.size > limit)
            {
                unsigned char sink[4096];
                for (std::uint64_t left = q.size; left != 0;)
                {
                    const std::size_t n = left < sizeof(sink) ? static_cast<std::size_t>(left) : sizeof(sink);
                    if (!recv_exact(in, sink, n, opt.message_ms))
                        return End::closed;
                    left -= n;
                }
                if (!reply(q, EMSGSIZE))
                    return End::closed;
                continue;
            }
            if (!grow(q.size ? q.size : 1))
            {
                reply(q, ENOMEM);
                return End::memory;
            }
            if (q.size != 0 && !recv_exact(in, body, q.size, opt.message_ms))
                return End::closed;
            End end = End::closed;
            if (opt.die_after != 0 && ++served >= opt.die_after)
            {
                handle_silently(q);
                return End::closed;
            }
            if (!handle(q, end))
                return End::closed;
            if (end == End::quit)
                return End::quit;
        }
    }
};
} // namespace

End serve(int in_fd, int out_fd, const ServerOptions &options)
{
    Server s(options, in_fd, out_fd);
    return s.run();
}
} // namespace broker

/*
 * ps5-native-app-boilerplate - elfldr elevation client.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 */
#include "elevation.hpp"

#include <array>
#include <fcntl.h>
#include <unistd.h>

namespace
{
struct NetSockaddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

extern "C"
{
    int sceKernelOpen(const char *path, int flags, mode_t mode);
    int sceKernelClose(int descriptor);
    std::int64_t sceKernelRead(int descriptor, void *buffer, std::size_t length);
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetSocketClose(int socket);
    int sceKernelUsleep(unsigned int microseconds);
}

// From Porpoise 8452ecb and 69f569f (GPL-3.0-or-later): the payload loader may not be listening yet
// the instant the title starts (a cold start from the home screen, a loader still coming up), so a
// connection that is refused is tried again, every quarter of a second for four seconds, before the
// title gives up and stays in its sandbox. Only the connection is tried again: once the helper has
// been sent, its answer is final.
constexpr int connect_tries = 16;
constexpr unsigned int connect_pause_us = 250'000;
int s_attempts = 0;

bool send_all(int socket, const void *data, std::size_t size) noexcept
{
    return elevation::wire::transfer(static_cast<const std::uint8_t *>(data), size,
                                     [socket](const auto *bytes, std::size_t remaining)
                                     { return sceNetSend(socket, bytes, remaining, 0); });
}

bool receive(int socket, elevation::wire::Message &message) noexcept
{
    return elevation::wire::transfer(reinterpret_cast<std::uint8_t *>(&message), sizeof(message),
                                     [socket](auto *bytes, std::size_t remaining)
                                     { return sceNetRecv(socket, bytes, remaining, 0); });
}

elevation::Status exchange(int socket, const elevation::wire::Message &request) noexcept
{
    using namespace elevation;
    using wire::Kind;
    if (!send_all(socket, &request, sizeof(request)))
        return Status::transport_error;
    wire::Message reply{};
    if (!receive(socket, reply))
        return Status::transport_error;
    if (wire::matches(reply, request, Kind::response) && reply.status != Status::ok)
        return reply.status;
    if (!wire::matches(reply, request, Kind::prepare) || reply.status != Status::ok)
        return Status::protocol_error;

    // The native same-UID syscall clones credentials before the helper edits them.
    // The helper independently verifies that p_ucred actually changed.
    wire::Message prepared = request;
    prepared.kind = Kind::prepared;
    if (seteuid(geteuid()) != 0)
        prepared.status = Status::prepare_failed;
    if (!send_all(socket, &prepared, sizeof(prepared)) || !receive(socket, reply))
        return Status::transport_error;
    if (!wire::matches(reply, request, Kind::response))
        return Status::protocol_error;
    if (prepared.status != Status::ok)
        return Status::prepare_failed;
    return reply.status;
}

elevation::Status submit(int socket, int helper, const elevation::wire::Message &request, bool *connected) noexcept
{
    using elevation::Status;
    // SceNet uses integer microseconds and its own timeout options, not BSD timeval.
    constexpr int socket_level = 0xffff;
    constexpr int timeout_us = elevation::wire::io_timeout_us;
    for (const int option : {0x1105, 0x1106, 0x1109}) // send, receive, connect
    {
        if (sceNetSetsockopt(socket, socket_level, option, &timeout_us, sizeof(timeout_us)) < 0)
            return Status::transport_error;
    }
    constexpr std::uint16_t port = 9021;
    const NetSockaddrIn address{sizeof(NetSockaddrIn),
                                2,
                                static_cast<std::uint16_t>((port << 8) | (port >> 8)),
                                0x0100007f,
                                0,
                                {0}};
    if (sceNetConnect(socket, &address, sizeof(address)) < 0)
        return Status::transport_error;
    *connected = true;

    std::array<std::uint8_t, 4096> buffer{};
    for (;;)
    {
        const auto count = sceKernelRead(helper, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0 || !send_all(socket, buffer.data(), static_cast<std::size_t>(count)))
            return Status::transport_error;
    }
    // elfldr consumes the ELF's section extent, then hands this same connection
    // to the helper as stdin/stdout. Keep it open for the protocol exchange.
    return exchange(socket, request);
}
} // namespace

elevation::Status elevation::request(Capability capability, const char *helper_path) noexcept
{
    wire::Message message{};
    message.pid = static_cast<std::uint32_t>(getpid());
    message.capability = capability;
    if (const auto error = wire::validate(message); error != Status::ok)
        return error;
    if (helper_path == nullptr)
        return Status::invalid_request;
    const int helper = sceKernelOpen(helper_path, O_RDONLY, 0);
    if (helper < 0)
        return Status::unavailable;
    auto result = Status::transport_error;
    for (s_attempts = 1;; ++s_attempts)
    {
        // (A socket whose connection was refused is not used again: each try has a new one.)
        const int socket = sceNetSocket("sandbox_elevator", 2, 1, 6);
        if (socket < 0)
            break;
        bool connected = false;
        result = submit(socket, helper, message, &connected);
        (void)sceNetSocketClose(socket);
        if (connected || s_attempts >= connect_tries)
            break;
        (void)sceKernelUsleep(connect_pause_us);
    }
    (void)sceKernelClose(helper);
    return result;
}

int elevation::attempts() noexcept
{
    return s_attempts;
}

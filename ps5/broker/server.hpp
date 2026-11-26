/*
 * Crux2 - the write broker's request handler (ps5/broker/server.cpp): the payload's whole work,
 * built for the console (payload/main.cpp) and for the PC test (tests/broker).
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 * Taken from OGBox (Press5elect, the same author: ps5/broker at db63d46).
 */
#pragma once

#include <cstddef>

namespace broker
{
struct ServerOptions
{
    int idle_ms = 10 * 60 * 1000; // no request for this long: the broker ends (safety)
    int message_ms = 30 * 1000;   // a request's body, a reply's sending: at most this long between bytes
    const char *root = nullptr;   // the PC test's: every accepted path is opened under this folder
    std::size_t io_cap = 0;       // the PC test's: each pwrite writes at most this many bytes
    unsigned die_after = 0;       // the PC test's: ends after carrying out this many requests, the last unanswered
    void (*log)(const char *line) = nullptr;
};

enum class End
{
    quit = 0,       // QUIT
    closed = 1,     // the connection closed (or broke)
    idle = 2,       // idle_ms without a request
    protocol = 3,   // a frame that wasn't one (answered EPROTO, then ended)
    memory = 4,
};

// Serves requests read from in_fd, answered on out_fd (the payload: 0 and 1, the loader's
// connection), one at a time in order, until one of End's.
End serve(int in_fd, int out_fd, const ServerOptions &options);
} // namespace broker

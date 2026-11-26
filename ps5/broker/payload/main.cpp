/*
 * Crux2 - the write broker (write-broker.elf): a payload that does a title's file writes for it.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 * Taken from OGBox (Press5elect, the same author: ps5/broker at db63d46).
 *
 * Crux2 (Settings > System > Fast writes to the console's storage; the title's side:
 * ps5/frontend/ps5_broker.cpp). The title sends this ELF to the payload loader on 127.0.0.1:9021
 * (elfldr), as it sends the sandbox elevation helper; the loader reads the ELF up to the end of its
 * section table, starts it as a process of its own and hands it the rest of that connection as
 * its standard input and output. It then answers the requests of protocol.hpp there (server.cpp) until QUIT, until
 * the connection closes (the title ended), or after ten minutes without a request. It writes only
 * under /data/, /mnt/usb<n>/ and /mnt/ext<n>/.
 */
#include "../server.hpp"

#include <cstdio>

extern "C"
{
#include <ps5/klog.h>
#include <signal.h>
#include <unistd.h>
}

namespace
{
void klog_line(const char *line)
{
    klog_printf("[write-broker] %s\n", line);
}
} // namespace

int main()
{
    // the title's end closes the connection: a write to it then is an error, not a signal
    signal(SIGPIPE, SIG_IGN);
    klog_printf("[write-broker] pid %d: serving the title's connection\n", static_cast<int>(getpid()));
    broker::ServerOptions options;
    options.log = &klog_line;
    const broker::End end = broker::serve(STDIN_FILENO, STDOUT_FILENO, options);
    static const char *const why[] = {"quit", "the connection closed", "idle", "not a request", "no memory"};
    klog_printf("[write-broker] pid %d: ended (%s)\n", static_cast<int>(getpid()), why[static_cast<int>(end)]);
    return end == broker::End::quit ? 0 : 1;
}

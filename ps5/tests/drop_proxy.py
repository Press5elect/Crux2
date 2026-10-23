#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - a TCP relay that can drop its connections, for the virtual file layer's tests.
#
#   drop_proxy.py <listen port> <target port>
#
# Relays every connection on 127.0.0.1:<listen port> to 127.0.0.1:<target port>. On SIGUSR1 it
# closes every connection it holds and goes on listening: to the client that is a server that
# closed an idle connection (a NAS does, after some minutes), with the server still there.
import selectors
import signal
import socket
import sys

listen_port, target_port = int(sys.argv[1]), int(sys.argv[2])
selector = selectors.DefaultSelector()
pairs = {}
drop = False


def on_signal(_number, _frame):
    global drop
    drop = True


signal.signal(signal.SIGUSR1, on_signal)
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", listen_port))
listener.listen(16)
listener.setblocking(False)
selector.register(listener, selectors.EVENT_READ)


def close_pair(sock):
    other = pairs.pop(sock, None)
    for s in (sock, other):
        if s is None:
            continue
        pairs.pop(s, None)
        try:
            selector.unregister(s)
        except (KeyError, ValueError):
            pass
        s.close()


while True:
    try:
        events = selector.select(0.05)
    except InterruptedError:
        events = []
    if drop:
        drop = False
        for sock in list(pairs):
            close_pair(sock)
    for key, _mask in events:
        sock = key.fileobj
        if sock is listener:
            client, _address = listener.accept()
            try:
                server = socket.create_connection(("127.0.0.1", target_port), 5)
            except OSError:
                client.close()
                continue
            pairs[client], pairs[server] = server, client
            selector.register(client, selectors.EVENT_READ)
            selector.register(server, selectors.EVENT_READ)
            continue
        if sock not in pairs:
            continue
        try:
            data = sock.recv(1 << 16)
        except OSError:
            data = b""
        if not data:
            close_pair(sock)
            continue
        try:
            pairs[sock].sendall(data)
        except OSError:
            close_pair(sock)

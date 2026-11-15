#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - a made-up Internet Archive for fetch_test.cpp: files with HTTP ranges, on 127.0.0.1.
#
#   range_server.py <folder of files> <file the port is written to>
#
#   /download/NAME  a redirect to /data/NAME (as archive.org sends a download to a data server)
#   /data/NAME      the file, with ranges (206, Content-Range)
#   /norange/NAME   the file whole (200) whatever is asked
#   /flaky/NAME     with ranges, but the first answer for each part (past the first byte, by its end) is cut
#                   off halfway
#   /broken/NAME    the first byte, then 503 for every part
#   /slowdown/NAME  with ranges, but the first answer for the part from byte 655360 (the 11th of
#                   64 KiB) slows down to 4 KiB a second after its first 4 KiB
#   /stats          "connections N" and a line a path ("/data 12"): what was asked
#   /reset          the counts back to 0
import http.server
import os
import re
import sys
import threading
import time

folder, port_file = sys.argv[1], sys.argv[2]
lock = threading.Lock()
counts = {"connections": 0, "ranged": 0}
cut = set()


def count(key):
    with lock:
        counts[key] = counts.get(key, 0) + 1


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        count("connections")

    def log_message(self, *args):
        pass

    def send(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        m = re.match(r"^/(download|data|norange|flaky|broken|slowdown)/([^/]+)$", self.path)
        if self.path == "/stats":
            with lock:
                text = "".join(f"{k} {v}\n" for k, v in sorted(counts.items()))
            return self.send(200, text.encode())
        if self.path == "/reset":
            with lock:
                counts.clear()
                counts.update({"connections": 0, "ranged": 0})
                cut.clear()
            return self.send(200, b"ok\n")
        if not m:
            return self.send(404)
        kind, name = m.groups()
        count("/" + kind)
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            return self.send(404)
        if kind == "download":
            return self.send(302, headers=[("Location", f"/data/{name}")])
        data = open(path, "rb").read()
        asked = self.headers.get("Range")
        r = re.match(r"^bytes=(\d+)-(\d*)$", asked or "")
        if kind == "norange" or not r:
            return self.send(200, data)
        count("ranged")
        start = int(r.group(1))
        end = int(r.group(2)) if r.group(2) else len(data) - 1
        end = min(end, len(data) - 1)
        if start > end:
            return self.send(416, headers=[("Content-Range", f"bytes */{len(data)}")])
        if kind == "broken" and start != 0:
            return self.send(503)
        part = data[start:end + 1]
        headers = [("Content-Range", f"bytes {start}-{end}/{len(data)}"), ("Accept-Ranges", "bytes")]
        if kind == "flaky" and start != 0:
            with lock:
                first = end not in cut
                cut.add(end)
            if first and len(part) > 2:
                self.send_response(206)
                for k, v in headers:
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(part)))
                self.end_headers()
                self.wfile.write(part[: len(part) // 2])
                self.wfile.flush()
                self.close_connection = True
                return
        if kind == "slowdown" and start == 655360:
            with lock:
                first = "slow" not in cut
                cut.add("slow")
            if first:
                self.send_response(206)
                for k, v in headers:
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(part)))
                self.end_headers()
                self.wfile.write(part[:4096])
                self.wfile.flush()
                for at in range(4096, len(part), 1024):
                    time.sleep(0.25)
                    self.wfile.write(part[at:at + 1024])
                    self.wfile.flush()
                return
        self.send(206, part, headers)


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 64  # (socketserver's 5 drops some of 10 connections made at once)

    def handle_error(self, request, client_address):
        pass  # (a connection the client closed: curl leaves a part it was stopped in)


server = Server(("127.0.0.1", 0), Handler)
server.daemon_threads = True
with open(port_file + ".tmp", "w") as f:
    f.write(str(server.server_address[1]))
os.replace(port_file + ".tmp", port_file)
server.serve_forever()

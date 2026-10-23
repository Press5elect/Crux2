#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - reading a PlayStation 2's memory cards over FTP (frontend/ps5_ps2_ftp.cpp), on a PC,
# against pyftpdlib made to answer as ps2ftpd does: "ps2ftpd ready", anonymous, cards at /mc/0 and
# /mc/1. Run twice: with passive mode, and with passive mode refused (active mode then).
#
#   ps5/tests/ps2ftp/run-ps2ftp-tests.sh <work folder>      (pip install pyftpdlib)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../../frontend" && pwd)
work=${1:?usage: run-ps2ftp-tests.sh <work folder>}
rm -rf "$work/root"
mkdir -p "$work/root/mc/0/BASLUS-21376BLACK" "$work/root/mc/0/BESLES-50001SAVE" "$work/root/mc/1/BASLUS-20002GAME" "$work/root/mc/1/NOT A SAVE"
head -c 964 /dev/urandom > "$work/root/mc/0/BASLUS-21376BLACK/icon.sys"
head -c 50000 /dev/urandom > "$work/root/mc/0/BASLUS-21376BLACK/BLACK.DAT"
head -c 964 /dev/urandom > "$work/root/mc/0/BESLES-50001SAVE/icon.sys"
head -c 964 /dev/urandom > "$work/root/mc/1/BASLUS-20002GAME/ICON.SYS"
head -c 100 /dev/urandom > "$work/root/mc/1/NOT A SAVE/data"
clang++ -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$front" \
    "$here/ps2ftp_test.cpp" "$front/ps5_ps2_ftp.cpp" -o "$work/ps2ftp_test"
status=0
for mode in passive active-only; do
    python3 - "$work/root" "$mode" "$work/port" <<'PY' &
import sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer
root, mode, portfile = sys.argv[1:4]
auth = DummyAuthorizer()
auth.add_anonymous(root)
class Ps2(FTPHandler):
    banner = "ps2ftpd ready."
    def ftp_PASV(self, line):
        if mode == "active-only":
            self.respond("500 Not understood.")
            return
        return super().ftp_PASV(line)
Ps2.authorizer = auth
server = FTPServer(("127.0.0.1", 0), Ps2)
open(portfile, "w").write(str(server.socket.getsockname()[1]))
server.serve_forever(timeout=0.2)
PY
    server=$!
    for _ in $(seq 50); do [[ -s $work/port ]] && break; sleep 0.1; done
    "$work/ps2ftp_test" "$(cat "$work/port")" "$mode" || status=1
    kill $server 2>/dev/null; wait $server 2>/dev/null || true
    rm -f "$work/port"
done
exit $status

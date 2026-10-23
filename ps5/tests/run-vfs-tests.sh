#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - host tests of the virtual file layer.
#
#   ps5/tests/run-vfs-tests.sh
#
# Builds ps5/frontend/vfs for this PC and runs it against a fixed tree in memory,
# an FTP server on the loopback (pyftpdlib), a Samba share (smbd) and an NFS server
# (nfs-ganesha with its VFS back end, and rpcbind), each serving the same generated
# folder. A server that is not installed skips its test.
#   LIBSMB2_PREFIX   a libsmb2 built for this PC (include/smb2, lib/libsmb2.a)
#   LIBNFS_PREFIX    a libnfs built for this PC (include/nfsc, lib/libnfs.a); by default the
#                    title's own pinned libnfs (.deps/src/libnfs, fetched by tools/build-deps.sh)
#                    is built for the PC into the work folder
set -euo pipefail
tests=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$tests/../.." && pwd)
work=${VFS_TEST_DIR:-$(mktemp -d)}
smb_prefix=${LIBSMB2_PREFIX:-/usr}
rm -rf "$work/smb" "$work/nfs"
rm -rf "$work/written" "$work/written-ftp" "$work/written-nfs"
mkdir -p "$work/tree/games/sub folder" "$work/smb/private" "$work/smb/log" "$work/smb/run" "$work/written" "$work/written-ftp" \
    "$work/written-nfs" "$work/nfs/run"

# libnfs for the PC: the same release the console's build links (its API changes between releases).
nfs_prefix=${LIBNFS_PREFIX:-}
if [[ -z $nfs_prefix ]]; then
    nfs_prefix="$work/libnfs-host"
    if [[ ! -f $nfs_prefix/lib/libnfs.a ]]; then
        [[ -d $root/.deps/src/libnfs ]] ||
            { echo "no libnfs: run ps5/tools/build-deps.sh (it fetches libnfs), or set LIBNFS_PREFIX" >&2; exit 2; }
        echo "== building libnfs for this PC"
        cmake -S "$root/.deps/src/libnfs" -B "$work/libnfs-host-build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_COMPILER=clang -DBUILD_SHARED_LIBS=OFF -DENABLE_TLS=OFF -DENABLE_MULTITHREADING=OFF \
            -DCMAKE_INSTALL_PREFIX="$nfs_prefix" -DCMAKE_INSTALL_LIBDIR=lib > "$work/libnfs-host.log" 2>&1 &&
            cmake --build "$work/libnfs-host-build" -j"$(nproc)" >> "$work/libnfs-host.log" 2>&1 &&
            cmake --install "$work/libnfs-host-build" >> "$work/libnfs-host.log" 2>&1 ||
            { tail -20 "$work/libnfs-host.log" >&2; exit 1; }
    fi
fi

# The layer and the libraries under it, for every program below.
vfs=("$tests"/../frontend/vfs/ps5_vfs.cpp "$tests"/../frontend/vfs/ps5_vfs_smb.cpp "$tests"/../frontend/vfs/ps5_vfs_ftp.cpp
     "$tests"/../frontend/vfs/ps5_vfs_nfs.cpp "$smb_prefix/lib/libsmb2.a" "$nfs_prefix/lib/libnfs.a" -lpthread)
flags=(-std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$smb_prefix/include" -I"$nfs_prefix/include")

clang++ "${flags[@]}" "$tests/vfs_test.cpp" "${vfs[@]}" -o "$work/vfs_test"

# The tree: a few "disc images" of awkward sizes, a cue sheet, names with spaces.
# The backups of memory cards and saved states (frontend/ps5_backup.cpp) write through the layer.
clang++ "${flags[@]}" -I"$tests/../frontend" "$tests/backup/backup_test.cpp" "$tests"/../frontend/ps5_backup.cpp \
    "${vfs[@]}" -o "$work/backup_test"

# The card manager's engine (frontend/ps5_cards.cpp) reads and writes whole files the same way.
clang++ "${flags[@]}" -I"$tests/../frontend" "$tests/cards/cards_engine_test.cpp" "$tests"/../frontend/ps5_cards.cpp \
    "$tests"/../frontend/ps5_backup.cpp "${vfs[@]}" -lz -o "$work/cards_engine_test"

# The logs' copy to a drive goes the same way (frontend/ps5_logs.cpp).
clang++ "${flags[@]}" -I"$tests/../frontend" "$tests/logs/logs_test.cpp" "$tests"/../frontend/ps5_logs.cpp \
    "$tests"/../frontend/ps5_backup.cpp "${vfs[@]}" -o "$work/logs_test"

python3 - "$work/tree" <<'PY'
import os, random, sys
root = sys.argv[1]
random.seed(3)
def put(name, size):
    with open(os.path.join(root, name), "wb") as f:
        f.write(random.randbytes(size))
put("Game One (USA).bin", 3 * 1024 * 1024 + 1234)
put("games/Game Two (Europe) (Disc 1).chd", 1500000)
put("games/sub folder/tiny.bin", 2352)
put("games/exact.bin", 256 * 1024 * 2)
put("games/empty.bin", 0)
with open(os.path.join(root, "Game One (USA).cue"), "w") as f:
    f.write('FILE "Game One (USA).bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
PY
chmod -R a+rX "$work/tree"

status=0
echo "== memory"
"$work/vfs_test" memory || status=1
echo "== nfs addresses and network.cfg's NFS lines (no server)"
"$work/vfs_test" nfs-parse "$work" || status=1
echo "== the logs: their end as lines, the run before, the copy"
mkdir -p "$work/logs"
"$work/logs_test" "$work/logs" || status=1
echo "== the card manager: saves brought in, written out, copied, deleted, put back"
mkdir -p "$work/cards-engine"
"$work/cards_engine_test" "$work/cards-engine" || status=1
echo "== backups, to a folder of the PC"
mkdir -p "$work/backup"
"$work/backup_test" "$work/backup" || status=1

if python3 -c 'import pyftpdlib' 2>/dev/null; then
    echo "== ftp"
    port=21210
    python3 -m pyftpdlib -i 127.0.0.1 -p $port -d "$work/tree" > "$work/ftp.log" 2>&1 &
    ftp_pid=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    "$work/vfs_test" ftp $port "$work/tree" || status=1
    kill $ftp_pid 2>/dev/null || true
    echo "== ftp, writing"
    port=21211
    python3 -m pyftpdlib -w -i 127.0.0.1 -p $port -d "$work/written-ftp" > "$work/ftp-write.log" 2>&1 &
    ftp_pid=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    "$work/vfs_test" ftp-write $port "$work/written-ftp" || status=1
    kill $ftp_pid 2>/dev/null || true
else
    echo "== ftp: skipped (pyftpdlib is not installed)"
fi

if command -v smbd >/dev/null; then
    echo "== smb"
    port=44500
    cat > "$work/smb/smb.conf" <<CONF
[global]
   server role = standalone server
   smb ports = $port
   bind interfaces only = yes
   interfaces = lo
   map to guest = Bad User
   guest account = $(id -un)
   private dir = $work/smb/private
   passdb backend = tdbsam:$work/smb/private/passdb.tdb
   lock directory = $work/smb/run
   state directory = $work/smb/run
   cache directory = $work/smb/run
   pid directory = $work/smb/run
   ncalrpc dir = $work/smb/run
   log file = $work/smb/log/smbd.log
   disable spoolss = yes
   load printers = no
   server min protocol = SMB2
[games]
   path = $work/tree
   guest ok = yes
   read only = yes
   force user = $(id -un)
[backup]
   path = $work/written
   guest ok = yes
   read only = no
   force user = $(id -un)
CONF
    # An account with a password, for the signed (SMB 3) path. '%' and '@' in it go percent-encoded.
    printf 'p@ss%%word\np@ss%%word\n' | smbpasswd -c "$work/smb/smb.conf" -a -s "$(id -un)" > /dev/null
    setsid smbd -F --no-process-group -s "$work/smb/smb.conf" > "$work/smb/log/out.log" 2>&1 &
    smb_pid=$!
    for _ in $(seq 100); do (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
    "$work/vfs_test" smb "smb://127.0.0.1:$port/games" "$work/tree" || status=1
    echo "== smb, a folder inside the share"
    "$work/vfs_test" smb "smb://127.0.0.1:$port/games/games" "$work/tree/games" || status=1
    echo "== smb, with an account"
    "$work/vfs_test" smb "smb://$(id -un):p%40ss%25word@127.0.0.1:$port/games" "$work/tree" || status=1
    echo "== smb, from network.cfg"
    printf '# test\r\npath = 127.0.0.1:%s/games/games\r\nPath = \\\\127.0.0.1:%s\\games\\games\\\r\n\r\nuser = %s\r\npassword = p@ss%%word\r\n' \
        $port $port "$(id -un)" > "$work/network.cfg"
    "$work/vfs_test" config "$work/network.cfg" games "$work/tree/games" || status=1
    # The template network.cfg's own account line: "user = guest" with no password is a guest, with
    # the SMB 2.1 second attempt (a server that maps guests gives the session no signing key).
    echo "== smb, as the written-out guest"
    "$work/vfs_test" smb "smb://guest@127.0.0.1:$port/games" "$work/tree" || status=1
    echo "== smb, from the template's network.cfg"
    printf 'path = 127.0.0.1:%s/games/games\npath = smb://127.0.0.1:%s/games/games/\nuser = guest\npassword =\n' \
        $port $port > "$work/network-guest.cfg"
    "$work/vfs_test" config "$work/network-guest.cfg" games "$work/tree/games" || status=1
    echo "== smb, writing (as a guest, and with an account)"
    "$work/vfs_test" smb-write "smb://127.0.0.1:$port/backup" "$work/written" || status=1
    "$work/vfs_test" smb-write "smb://$(id -un):p%40ss%25word@127.0.0.1:$port/backup" "$work/written" || status=1
    echo "== smb, writing into a folder inside the share"
    mkdir -p "$work/written/inside"
    "$work/vfs_test" smb-write "smb://127.0.0.1:$port/backup/inside" "$work/written/inside" || status=1
    echo "== smb, a share nobody may write to"
    "$work/vfs_test" smb-readonly "smb://127.0.0.1:$port/games" || status=1
    echo "== backups, to a share, and to one nobody may write to"
    "$work/backup_test" "$work/backup" smb "smb://127.0.0.1:$port/backup" "$work/written" 2>&1 | grep -v "^  vfs:" || status=1
    "$work/backup_test" "$work/backup" smb-readonly "smb://127.0.0.1:$port/games" "$work/tree" 2>&1 | grep -v "^  vfs:" || status=1
    # A server that closes the connection while the share is in use: through a relay that drops
    # its connections when told to.
    echo "== smb, a connection the server closed"
    relay_port=44501
    python3 "$tests/drop_proxy.py" $relay_port $port &
    relay_pid=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$relay_port) 2>/dev/null && break; sleep 0.1; done
    # (libsmb2 itself leaves a few dozen bytes behind for a request whose connection died under
    # it, once for each such request: its own, and too small to matter for a NAS that drops a
    # connection now and then. The leak check is told so, for this run only.)
    printf 'leak:smb2_getinfo_async\nleak:smb2_stat\nleak:smb2_opendir\nleak:smb2_open\nleak:smb2_pread\n' > "$work/lsan-libsmb2.supp"
    LSAN_OPTIONS="suppressions=$work/lsan-libsmb2.supp:print_suppressions=0" \
        "$work/vfs_test" smb-drop "smb://127.0.0.1:$relay_port/games" "$work/tree" $relay_pid || status=1
    echo "== smb, writing over a connection the server closes"
    rm -rf "${work:?}/written/"*
    printf 'leak:smb2_mkdir\nleak:smb2_pwrite\nleak:smb2_unlink\nleak:smb2_rename\nleak:smb2_rmdir\nleak:smb2_close\n' >> "$work/lsan-libsmb2.supp"
    LSAN_OPTIONS="suppressions=$work/lsan-libsmb2.supp:print_suppressions=0" \
        "$work/vfs_test" smb-write-drop "smb://127.0.0.1:$relay_port/backup" "$work/written" $relay_pid || status=1
    kill $relay_pid 2>/dev/null || true
    kill $smb_pid 2>/dev/null || true
else
    echo "== smb: skipped (smbd is not installed)"
fi

# NFS: nfs-ganesha serving the tree (read-only) and a folder to write in, on ports of its own on the
# loopback, registered with rpcbind (which NFS 3 clients ask for the ports). Both need root: rpcbind
# listens on port 111, and ganesha's VFS back end opens files by their handles.
if command -v ganesha.nfsd >/dev/null && command -v rpcbind >/dev/null && [[ $(id -u) == 0 ]]; then
    nfs_port=20490
    mount_port=20491
    started_rpcbind=
    if ! (exec 3<>/dev/tcp/127.0.0.1/111) 2>/dev/null; then
        mkdir -p /run/rpcbind
        setsid rpcbind -f -w > "$work/nfs/rpcbind.log" 2>&1 &
        started_rpcbind=$!
        for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/111) 2>/dev/null && break; sleep 0.1; done
    fi
    # The folder to write in belongs to the user the layer gives by default (uid and gid 1000).
    chown 1000:1000 "$work/written-nfs"
    cat > "$work/nfs/ganesha.conf" <<CONF
NFS_CORE_PARAM {
   Bind_addr = 127.0.0.1;
   NFS_Port = $nfs_port;
   MNT_Port = $mount_port;
   NLM_Port = 20492;
   Rquota_Port = 20493;
   Enable_NLM = false;
   Enable_RQUOTA = false;
   Enable_UDP = false;
   Protocols = 3, 4;
}
NFSV4 { Graceless = true; RecoveryBackend = fs; }
NFS_KRB5 { Active_krb5 = false; }
EXPORT {
   Export_Id = 1;
   Path = "$work/tree";
   Pseudo = /games;
   Access_Type = RO;
   Squash = No_Root_Squash;
   SecType = sys;
   Transports = TCP;
   FSAL { Name = VFS; }
}
EXPORT {
   Export_Id = 2;
   Path = "$work/written-nfs";
   Pseudo = /backup;
   Access_Type = RW;
   Squash = No_Root_Squash;
   SecType = sys;
   Transports = TCP;
   FSAL { Name = VFS; }
}
LOG { Default_Log_Level = EVENT; }
CONF
    setsid ganesha.nfsd -F -L "$work/nfs/ganesha.log" -f "$work/nfs/ganesha.conf" -p "$work/nfs/run/ganesha.pid" \
        > "$work/nfs/out.log" 2>&1 &
    nfs_pid=$!
    for _ in $(seq 100); do (exec 3<>/dev/tcp/127.0.0.1/$nfs_port) 2>/dev/null && break; sleep 0.1; done
    sleep 1
    echo "== nfs (NFS 3: the export from the server's list, ports from rpcbind)"
    "$work/vfs_test" nfs "nfs://127.0.0.1$work/tree" "$work/tree" || status=1
    echo "== nfs, a folder inside the export"
    "$work/vfs_test" nfs "nfs://127.0.0.1$work/tree/games" "$work/tree/games" || status=1
    echo "== nfs, NFS 4 (the export by its path in the server's tree), and a folder inside it"
    "$work/vfs_test" nfs "nfs://127.0.0.1:$nfs_port/games?version=4" "$work/tree" || status=1
    "$work/vfs_test" nfs "nfs://127.0.0.1:$nfs_port/games/games?version=4" "$work/tree/games" || status=1
    echo "== nfs, the ports given (no rpcbind asked)"
    "$work/vfs_test" nfs "nfs://127.0.0.1:$nfs_port$work/tree?mountport=$mount_port&version=3" "$work/tree" || status=1
    echo "== nfs, from network.cfg"
    printf '# test\r\npath = nfs://127.0.0.1%s/games\r\npath = NFS://127.0.0.1:%s/games/games/?version=4\r\nuid = 1000\r\ngid = 1000\r\n' \
        "$work/tree" $nfs_port > "$work/network-nfs.cfg"
    "$work/vfs_test" config "$work/network-nfs.cfg" games "$work/tree/games" || status=1
    echo "== nfs, writing (NFS 3, and NFS 4)"
    "$work/vfs_test" nfs-write "nfs://127.0.0.1$work/written-nfs" "$work/written-nfs" || status=1
    "$work/vfs_test" nfs-write "nfs://127.0.0.1:$nfs_port/backup?version=4" "$work/written-nfs" || status=1
    echo "== nfs, writing into a folder inside the export"
    mkdir -p "$work/written-nfs/inside"
    chown 1000:1000 "$work/written-nfs/inside"
    "$work/vfs_test" nfs-write "nfs://127.0.0.1$work/written-nfs/inside" "$work/written-nfs/inside" || status=1
    echo "== nfs, an export nobody may write to, and a user the folder does not let write"
    "$work/vfs_test" nfs-readonly "nfs://127.0.0.1$work/tree" || status=1
    "$work/vfs_test" nfs-readonly "nfs://127.0.0.1$work/written-nfs?uid=4321&gid=4321" || status=1
    echo "== nfs, a connection the server closed"
    relay_port=20499
    python3 "$tests/drop_proxy.py" $relay_port $nfs_port &
    relay_pid=$!
    for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$relay_port) 2>/dev/null && break; sleep 0.1; done
    "$work/vfs_test" nfs-drop "nfs://127.0.0.1:$relay_port$work/tree?mountport=$mount_port" "$work/tree" $relay_pid || status=1
    echo "== nfs, writing over a connection the server closes"
    rm -rf "${work:?}/written-nfs/"*
    "$work/vfs_test" nfs-write-drop "nfs://127.0.0.1:$relay_port$work/written-nfs?mountport=$mount_port" "$work/written-nfs" \
        $relay_pid || status=1
    kill $relay_pid 2>/dev/null || true
    kill $nfs_pid 2>/dev/null || true
    [[ -n $started_rpcbind ]] && { kill "$started_rpcbind" 2>/dev/null || true; }
else
    echo "== nfs: skipped (needs nfs-ganesha with its VFS back end, rpcbind, and root)"
fi

[[ $status == 0 ]] && echo "all virtual file layer tests passed" || echo "virtual file layer tests FAILED"
exit $status

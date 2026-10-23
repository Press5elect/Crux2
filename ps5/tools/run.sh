#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - a test run on the console: the title runs by itself for a while, measures,
# and ends through its own exit; klog is captured and a crash is symbolised.
#
#   ps5/tools/run.sh                              30 s in the interface
#   ps5/tools/run.sh games/my-test-disc.iso       boot that image (a path in the title folder,
#                                                 or as the title sees it: /app0/..., /smb/...)
#   SECONDS_TO_RUN=120 ps5/tools/run.sh ...       another length
#   SCREENSHOT=0 ps5/tools/run.sh ...             no screenshot of the last frame
#
# It writes test-run.txt into the title folder on the console; the launch that reads it deletes
# it, so the next launch by hand is an ordinary one (ps5/frontend/ps5_host.cpp, "Test runs").
# PS5_Vulkan's run tool then checks that the console runs nothing, launches the title through
# the control payload (ps5vkctl), captures klog until the title's "test: end" line, a crash
# record or the title's exit, and symbolises a crash against this build's ELF. The title's own
# record of the run, crux2-boot.log, is fetched as well (and the screenshot, snaps/test-run.png).
#
# The console's address and ports come from PS5_Vulkan's ignored .env (PS5_HOST, FTP_PORT,
# KLOG_PORT, PS5VKCTL_PORT). Check first that the build on the console is this one: the boot
# log's "build identity" line against build/ps5/build-identity.txt.
#
# Made from PS5 Vulkan Template's ps5/tools/run.sh (Copyright (C) 2026 Mihawk, MIT).
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")
seconds=${SECONDS_TO_RUN:-30}
run_dir="$root/klog/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run_dir"

test_run="seconds $seconds"
[[ -n ${1:-} ]] && test_run+=$'\n'"game $1"
[[ -n ${1:-} && ${SCREENSHOT:-1} != 0 ]] && test_run+=$'\n'"screenshot"

python3 - "$vulkan/tools" "$title_id" "$test_run" <<'PY'
import io, sys
from ftplib import FTP
sys.path.insert(0, sys.argv[1])
import ps5_console
settings = ps5_console.load_settings()
ftp = FTP()
ftp.connect(settings["host"], settings["ftp_port"], timeout=15)
ftp.login(settings["ftp_user"], settings["ftp_password"])
ftp.storbinary(f"STOR /data/homebrew/{sys.argv[2]}/test-run.txt", io.BytesIO(f"{sys.argv[3]}\n".encode()))
ftp.quit()
print("test run: " + sys.argv[3].replace("\n", "; "))
PY

fetch=(--fetch crux2-boot.log)
[[ -n ${1:-} && ${SCREENSHOT:-1} != 0 ]] && fetch+=(--fetch snaps/test-run.png)
cd "$run_dir"
python3 "$vulkan/tools/run-title.py" "$title_id" \
    --until '\[PCSX2\] test: end' --timeout "${RUN_TIMEOUT:-$(( seconds + 180 ))}" \
    --echo '\[PCSX2\] (test:|build|FATAL|error:|crash)|fatal signal|GPU_FAULT' \
    --elf "$root/build/ps5/link/llvm-pie.elf" --output "$run_dir/klog.log" "${fetch[@]}"
echo "the run's files: $run_dir"
echo "this build:      $(cat "$root/build/ps5/build-identity.txt" 2>/dev/null || echo '(not built here)')"

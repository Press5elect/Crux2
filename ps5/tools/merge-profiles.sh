#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the counts of a profiling build's runs, merged into the profile the next build is
# compiled with (PS5PX_PGO=use ps5/tools/build.sh).
#
#   ps5/tools/merge-profiles.sh                              every game's folder under ps5/pgo/runs/
#                                                            and ps5/pgo/cs-runs/, each game counting
#                                                            alike: writes ps5/pgo/crux2.profdata
#   ps5/tools/merge-profiles.sh <file.profraw | folder>...   the files and folders given
#
# Keep one folder per game: ps5/pgo/runs/<game>/ for the first round (PS5PX_PGO=generate), and
# ps5/pgo/cs-runs/<game>/ for the second (PS5PX_PGO=cs-generate, built on the first round's
# profile). The second round's counts are of the code after inlining; they go into the same
# profile beside the first round's, and a build with it uses both.
#
# The .profraw files are what a profiling build (PS5PX_PGO=generate) wrote on the console:
# <data>/pgo/crux2-<when>.profraw, one a run, also in a log copy (Settings > System > Log).
# Each run counts as much as it ran; to make the games played weigh alike whatever their time,
# give each its own folder and set PS5PX_PGO_EQUAL=1 (each folder is then scaled to the same
# total). llvm-profdata must be the release of the SDK's clang, as the raw format is.
#   PS5PX_PGO_PROFILE   where the profile goes (default ps5/pgo/crux2.profdata)
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=$(bash "$ps5/tools/setup-sdk.sh")
if [[ $# -eq 0 ]]; then
    mapfile -t games < <(find "$ps5/pgo/runs" "$ps5/pgo/cs-runs" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort)
    [[ ${#games[@]} -gt 0 ]] || { sed -n '4,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
    export PS5PX_PGO_EQUAL=${PS5PX_PGO_EQUAL:-1}
    set -- "${games[@]}"
fi
major=$("$sdk/bin/prospero-clang" --version | sed -n 's/.*clang version \([0-9]*\)\..*/\1/p' | head -1)
profdata=$(command -v "llvm-profdata-$major" || true)
[[ -n $profdata ]] || { echo "llvm-profdata-$major is needed (the SDK's clang is $major)" >&2; exit 1; }
out=${PS5PX_PGO_PROFILE:-$ps5/pgo/crux2.profdata}
mkdir -p "$(dirname "$out")"

inputs=()
for given in "$@"; do
    if [[ -d $given ]]; then
        mapfile -t files < <(find "$given" -name '*.profraw' -size +0 | sort)
        [[ ${#files[@]} -gt 0 ]] || { echo "no .profraw in $given" >&2; exit 1; }
        if [[ ${PS5PX_PGO_EQUAL:-} == 1 ]]; then
            # The folder merged alone, then given a weight that brings its total to the same as
            # the others' (llvm-profdata weights are whole numbers: the largest total gets 1).
            "$profdata" merge -o "$given/.folder.profdata" "${files[@]}"
            inputs+=("folder:$given")
        else
            inputs+=("${files[@]}")
        fi
    else
        inputs+=("$given")
    fi
done

if [[ ${PS5PX_PGO_EQUAL:-} == 1 ]]; then
    totals=()
    largest=1
    for input in "${inputs[@]}"; do
        [[ $input == folder:* ]] || continue
        total=$("$profdata" show --detailed-summary "${input#folder:}/.folder.profdata" 2>/dev/null |
            awk -F': *' '/^Total count:/ { printf "%d", $2 }')
        totals+=("$total")
        (( total > largest )) && largest=$total
    done
    weighted=()
    index=0
    for input in "${inputs[@]}"; do
        if [[ $input == folder:* ]]; then
            total=${totals[$index]}
            index=$((index + 1))
            weight=$(( total > 0 ? (largest + total / 2) / total : 1 ))
            weighted+=("--weighted-input=$weight,${input#folder:}/.folder.profdata")
            echo "  ${input#folder:}: weight $weight" >&2
        else
            weighted+=("$input")
        fi
    done
    "$profdata" merge -o "$out" "${weighted[@]}"
else
    "$profdata" merge -o "$out" "${inputs[@]}"
fi

for input in "${inputs[@]}"; do
    [[ $input == folder:* ]] && rm -f "${input#folder:}/.folder.profdata"
done
"$profdata" show --detailed-summary "$out" | sed -n '1,12p'
echo "profile: $out ($(du -h "$out" | cut -f1)); build with: ps5/tools/build.sh (it uses the profile by itself)"

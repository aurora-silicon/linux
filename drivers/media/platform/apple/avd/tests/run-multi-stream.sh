#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Run only on an explicitly approved decoder device; each process owns a context.
set -eu
bench=${1:?usage: run-multi-stream.sh BENCH VIDEO MEDIA CLIP N OUT}
video=${2:?explicit AVD video node required}
media=${3:?explicit matching media node required}
clip=${4:?input clip required}
streams=${5:?stream count required}
out=${6:?new output directory required}
case "$streams" in 1|2|4) ;; *) exit 2 ;; esac
mkdir "$out"
pids=
i=0
while [ "$i" -lt "$streams" ]; do
	"$bench" --input "$clip" --video "$video" --media "$media" \
		--queue 4 --passes 6 --hashes "$out/$i.hashes.tsv" \
		>"$out/$i.jsonl" 2>"$out/$i.stderr" &
	pids="$pids $!"
	i=$((i + 1))
done
status=0
for pid in $pids; do
	wait "$pid" || status=1
done
exit "$status"

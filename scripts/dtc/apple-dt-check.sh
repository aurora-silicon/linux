#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# apple-dt-check.sh BASE HEAD WORKDIR: run CHECK_DTBS on every Apple device tree at BASE and at HEAD
# and fail when HEAD has a warning BASE does not. Also runs dt_binding_check on every binding HEAD
# changes. Phandle values in the messages are masked, so renumbering is not a new warning, and so
# is the pattern list of a "does not match any of the regexes" message, which grows with every
# vendor prefix the bindings add. Upstream dtschema's chosen.yaml sets additionalProperties: false,
# so properties documented in in-tree bindings/chosen/*.yaml are subtracted from "chosen: ...
# do(es) not match any of the regexes:" lines while preserving warnings for any undocumented
# properties and all schema type/value checks. A firmware-profile variant (a .dts that includes
# another board's .dts, such as t8122-j613-25g83) is reported under its base board's name, so the
# base's existing warnings are not new warnings of the variant; anything the variant adds still is.
# Needs dtschema (dt-validate), clang/lld (LLVM=1), flex, bison, python3. Prints a summary.
set -u
BASE=$1 HEAD=$2 W=$3
export ARCH=arm64 LLVM=1
mkdir -p "$W"
filter_documented_chosen() { # chosen_bindings_dir
	python3 -c '
import glob, os, re, sys

def load_yaml(path):
	try:
		import yaml
		with open(path, encoding="utf-8") as f:
			return yaml.safe_load(f)
	except ImportError:
		from ruamel.yaml import YAML
		with open(path, encoding="utf-8") as f:
			return YAML(typ="safe").load(f)

chosen_dir = sys.argv[1]
documented = set()
for path in sorted(glob.glob(os.path.join(chosen_dir, "*.yaml"))):
	doc = load_yaml(path)
	if not isinstance(doc, dict):
		continue
	props = doc.get("properties")
	if not isinstance(props, dict):
		continue
	for key, val in props.items():
		if isinstance(key, str) and not key.startswith("$") and val is not False:
			documented.add(key)

pat = re.compile(
	r"^(?P<prefix>[^\n]*\.dtb: chosen: )"
	r"(?P<props>\x27[^\x27]+\x27(?:, \x27[^\x27]+\x27)*)"
	r" (?:does|do) not match any of the regexes:(?P<tail>.*)$"
)

for raw in sys.stdin:
	line = raw.rstrip("\n")
	m = pat.match(line)
	if not m:
		sys.stdout.write(raw)
		continue
	props = re.findall(r"\x27([^\x27]+)\x27", m.group("props"))
	remaining = [p for p in props if p not in documented]
	if not remaining:
		continue
	verb = "does" if len(remaining) == 1 else "do"
	joined = ", ".join(f"\x27{p}\x27" for p in remaining)
	prefix = m.group("prefix")
	tail = m.group("tail")
	sys.stdout.write(f"{prefix}{joined} {verb} not match any of the regexes:{tail}\n")
' "$1"
}
warnings() { # rev tag
	local t=$W/src-$2 o=$W/out-$2
	rm -f "$W/$2.warn"
	git worktree add -q --detach "$t" "$1" || return 1
	make -s -C "$t" O="$o" defconfig >/dev/null || return 1
	local dtbs; dtbs=$(cd "$t/arch/arm64/boot/dts" && ls apple/*.dts | sed 's/dts$/dtb/')
	make -C "$t" O="$o" -j"$(nproc)" CHECK_DTBS=y $dtbs > "$W/$2.log" 2>&1
	local rc=$?
	local variants; variants=$(cd "$t/arch/arm64/boot/dts" &&
		grep -l '^#include "[^"]*\.dts"' apple/*.dts 2>/dev/null | while read -r v; do
			sed -nE 's#^\#include "([^"]*)\.dts".*#s|^'"${v%.dts}"'\\.dtb: |apple/\1.dtb: |;#p' "$v" | head -1
		done)
	grep -E '\.dtb: |[Ww]arning|rror' "$W/$2.log" | grep -v '^  ' |
		sed -E "s#$t/##g; s#$o/##g" |
		sed -E "s#arch/arm64/boot/dts/##; $variants" | sed -E 's#^apple/#arch/arm64/boot/dts/apple/#' |
		perl -pe 's{(\[\[[^:]*?\]\])}{(my $x = $1) =~ s/\d+/N/g; $x}ge; s{b\x27(?:\\.|[^\x27])*\x27}{b\x27N\x27}g; s{( not match any of the regexes): .*}{$1: ...}' |
		filter_documented_chosen "$t/Documentation/devicetree/bindings/chosen" |
		sort -u > "$W/$2.warn"
	echo "$2: make rc=$rc, $(wc -l < "$W/$2.warn") warning lines, $(echo $dtbs | wc -w) DTBs"
	return $rc
}
fail=0
warnings "$BASE" base || echo "base build failed (its warnings still count)"
if [ ! -f "$W/base.warn" ]; then
	# No list to compare against: every HEAD warning counts as new.
	echo "BASE $BASE could not be checked out or configured"
	: > "$W/base.warn"
	fail=1
fi
warnings "$HEAD" head; hrc=$?
comm -13 "$W/base.warn" "$W/head.warn" > "$W/new.warn"
comm -23 "$W/base.warn" "$W/head.warn" > "$W/gone.warn"
echo "new warnings: $(wc -l < "$W/new.warn"), removed: $(wc -l < "$W/gone.warn")"
[ $hrc = 0 ] || { echo "HEAD device trees do not build"; fail=1; }
if [ -s "$W/new.warn" ]; then echo "--- new CHECK_DTBS warnings"; cat "$W/new.warn"; fail=1; fi
b=$(git diff --name-only --diff-filter=AM "$BASE" "$HEAD" -- 'Documentation/devicetree/bindings/*.yaml')
for f in $b; do
	if make -s -C "$W/src-head" O="$W/out-head" dt_binding_check DT_SCHEMA_FILES="${f#Documentation/devicetree/bindings/}" > "$W/binding.log" 2>&1 &&
	   ! grep -qE 'warning|error' "$W/binding.log"; then
		echo "binding ok: $f"
	else
		echo "--- binding check failed: $f"; cat "$W/binding.log"; fail=1
	fi
done
git worktree remove --force "$W/src-base" 2>/dev/null; true
exit $fail

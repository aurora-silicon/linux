#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# apple-dt-check.sh BASE HEAD WORKDIR: run CHECK_DTBS on every Apple device tree at BASE and at HEAD
# and fail when HEAD has a warning BASE does not. Also runs dt_binding_check on every binding HEAD
# changes. Phandle values in the messages are masked, so renumbering is not a new warning, and so
# is the pattern list of a "does not match any of the regexes" message, which grows with every
# vendor prefix the bindings add. Upstream dtschema's chosen.yaml sets additionalProperties: false,
# so properties documented in in-tree bindings/chosen/*.yaml are subtracted from "chosen: ...
# do(es) not match any of the regexes:" lines emitted by http://devicetree.org/schemas/chosen.yaml#
# while preserving warnings from any other schema, warnings for any undocumented properties, and
# all schema type/value checks. A firmware-profile variant (a .dts that includes another board's
# .dts, such as t8122-j613-25g83) is reported under its base board's name, so the base's existing
# warnings are not new warnings of the variant; anything the variant adds still is.
# Needs dtschema (dt-validate), clang/lld (LLVM=1), flex, bison, python3. Prints a summary.
set -u
BASE=$1 HEAD=$2 W=$3
export ARCH=arm64 LLVM=1
# sort and comm must agree on the order: some UTF-8 locales collate these lines differently.
export LC_ALL=C
mkdir -p "$W"
filter_documented_chosen() { # chosen_bindings_dir
	python3 -c '
import glob, os, re, sys

CHOSEN_SCHEMA_ID = "http://devicetree.org/schemas/chosen.yaml#"

def get_yaml_loader():
	try:
		import yaml
		return yaml.safe_load
	except ImportError:
		pass
	try:
		from ruamel.yaml import YAML
		loader = YAML(typ="safe")
		return loader.load
	except ImportError:
		sys.stderr.write(
			"apple-dt-check.sh: neither PyYAML nor ruamel.yaml is available "
			"to parse bindings/chosen/*.yaml\n"
		)
		sys.exit(1)

load_yaml = get_yaml_loader()
chosen_dir = sys.argv[1]
documented = set()
for path in sorted(glob.glob(os.path.join(chosen_dir, "*.yaml"))):
	with open(path, encoding="utf-8") as f:
		doc = load_yaml(f)
	if not isinstance(doc, dict):
		continue
	props = doc.get("properties")
	if not isinstance(props, dict):
		continue
	for key, val in props.items():
		if isinstance(key, str) and not key.startswith("$") and val is not False:
			documented.add(key)

pat = re.compile(
	r"^(?P<prefix>[^\s\n][^\n]*\.dtb: chosen: )"
	r"(?P<props>\x27[^\x27]+\x27(?:, \x27[^\x27]+\x27)*)"
	r" (?:does|do) not match any of the regexes:(?P<tail>.*)$"
)
inline_schema_re = re.compile(r"\s+from schema \$id:\s*(\S+)\s*$")
hint_line_re = re.compile(r"^[ \t]+hint: ")
schema_line_re = re.compile(r"^[ \t]+from schema \$id:\s*(\S+)\s*$")

lines = sys.stdin.readlines()
i = 0
n = len(lines)
while i < n:
	raw = lines[i]
	line = raw.rstrip("\r\n")
	m = pat.match(line)
	if not m:
		sys.stdout.write(raw)
		i += 1
		continue

	cont = []
	schema_id = None
	m_inline = inline_schema_re.search(m.group("tail"))
	if m_inline:
		schema_id = m_inline.group(1)
		i += 1
	else:
		j = i + 1
		while j < n and hint_line_re.match(lines[j]):
			cont.append(lines[j])
			j += 1
		if j < n:
			m_schema = schema_line_re.match(lines[j].rstrip("\r\n"))
			if m_schema:
				schema_id = m_schema.group(1)
				cont.append(lines[j])
				j += 1
		i = j

	if schema_id != CHOSEN_SCHEMA_ID:
		sys.stdout.write(raw)
		for c in cont:
			sys.stdout.write(c)
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
	for c in cont:
		sys.stdout.write(c)
' "$1"
}
extract_warnings() { # src_dir out_dir log_file out_warn tag
	local t=$1 o=$2 log=$3 out=$4 tag=$5
	local filtered=$W/$tag.filtered.log
	rm -f "$filtered" "$out"
	if ! filter_documented_chosen "$t/Documentation/devicetree/bindings/chosen" < "$log" > "$filtered"; then
		rm -f "$filtered" "$out"
		echo "apple-dt-check.sh: $tag: failed to filter documented /chosen warnings" >&2
		filter_failed=1
		return 1
	fi
	local variants; variants=$(cd "$t/arch/arm64/boot/dts" &&
		grep -l '^#include "[^"]*\.dts"' apple/*.dts 2>/dev/null | while read -r v; do
			sed -nE 's#^\#include "([^"]*)\.dts".*#s|^'"${v%.dts}"'\\.dtb: |apple/\1.dtb: |;#p' "$v" | head -1
		done)
	grep -E '\.dtb: |[Ww]arning|rror' "$filtered" | grep -v '^  ' |
		sed -E "s#$t/##g; s#$o/##g" |
		sed -E "s#arch/arm64/boot/dts/##; $variants" | sed -E 's#^apple/#arch/arm64/boot/dts/apple/#' |
		perl -pe 's{(\[\[[^:]*?\]\])}{(my $x = $1) =~ s/\d+/N/g; $x}ge; s{(?:^|(?<=[\s\[(=]))b\x27(?:\\.|[^\x27])*\x27}{b\x27N\x27}g; s{( not match any of the regexes): .*}{$1: ...}' |
		sort -u > "$out"
	local ps=("${PIPESTATUS[@]}")
	if [ "${ps[0]}" -gt 1 ] || [ "${ps[1]}" -gt 1 ] || [ "${ps[2]}" -ne 0 ] || [ "${ps[3]}" -ne 0 ] || [ "${ps[4]}" -ne 0 ] || [ "${ps[5]}" -ne 0 ] || [ "${ps[6]}" -ne 0 ]; then
		rm -f "$out"
		echo "apple-dt-check.sh: $tag: warning extraction pipeline failed (status: ${ps[*]})" >&2
		filter_failed=1
		return 1
	fi
	# One line per undocumented property, so documenting one of a node's properties does not
	# turn that node's remaining ones into a "new" warning.
	python3 -c '
import re, sys
pat = re.compile(r"^(.*?: )(\x27[^\x27]+\x27(?:, \x27[^\x27]+\x27)*) do(?:es)? not match any of the regexes:(.*)$")
for line in open(sys.argv[1]):
	line = line.rstrip("\n")
	m = pat.match(line)
	if not m:
		print(line)
		continue
	prefix, props, tail = m.groups()
	for prop in re.findall(r"\x27[^\x27]+\x27", props):
		print(prefix + prop + " does not match any of the regexes:" + tail)
' "$out" | sort -u > "$out.split"
	local sp=("${PIPESTATUS[@]}")
	if [ "${sp[0]}" -ne 0 ] || [ "${sp[1]}" -ne 0 ]; then
		rm -f "$out" "$out.split"
		echo "apple-dt-check.sh: $tag: splitting property warnings failed" >&2
		filter_failed=1
		return 1
	fi
	mv "$out.split" "$out"
}
warnings() { # rev tag
	local t=$W/src-$2 o=$W/out-$2
	filter_failed=0
	rm -f "$W/$2.warn" "$W/$2.filtered.log"
	git worktree add -q --detach "$t" "$1" || return 1
	make -s -C "$t" O="$o" defconfig >/dev/null || return 1
	local dtbs; dtbs=$(cd "$t/arch/arm64/boot/dts" && ls apple/*.dts | sed 's/dts$/dtb/')
	make -C "$t" O="$o" -j"$(nproc)" -Otarget CHECK_DTBS=y $dtbs > "$W/$2.log" 2>&1
	local rc=$?
	extract_warnings "$t" "$o" "$W/$2.log" "$W/$2.warn" "$2" || return 1
	echo "$2: make rc=$rc, $(wc -l < "$W/$2.warn") warning lines, $(echo $dtbs | wc -w) DTBs"
	return $rc
}
cleanup() {
	git worktree remove --force "$W/src-base" 2>/dev/null || true
}
trap cleanup EXIT
fail=0
filter_failed=0
warnings "$BASE" base; brc=$?
if [ "$filter_failed" -ne 0 ]; then
	echo "apple-dt-check.sh: BASE $BASE warning filtering failed" >&2
	exit 1
fi
if [ "$brc" -ne 0 ]; then
	echo "base build failed (its warnings still count)"
fi
if [ ! -f "$W/base.warn" ]; then
	# No list to compare against: every HEAD warning counts as new.
	echo "BASE $BASE could not be checked out or configured"
	: > "$W/base.warn"
	fail=1
fi
filter_failed=0
warnings "$HEAD" head; hrc=$?
if [ "$filter_failed" -ne 0 ]; then
	echo "apple-dt-check.sh: HEAD $HEAD warning filtering failed" >&2
	exit 1
fi
if [ ! -f "$W/head.warn" ]; then
	echo "apple-dt-check.sh: HEAD $HEAD could not be checked out or configured" >&2
	exit 1
fi
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
exit $fail

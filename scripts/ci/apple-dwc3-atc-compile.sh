#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Compile the real Apple DWC3 consumer with ATC's unreachable-provider stubs.
set -euo pipefail

if [[ $# != 1 || -z ${RUNNER_TEMP:-} ]]; then
	echo "usage: RUNNER_TEMP=<scratch-directory> $0 <profile>" >&2
	exit 2
fi
profile=$1
case "$profile" in
	builtin-no-atc) dwc3=y; atc=n ;;
	modular-no-atc) dwc3=m; atc=n ;;
	builtin-modular-atc) dwc3=y; atc=m ;;
	*) echo "unknown DWC3/ATC profile: $profile" >&2; exit 2 ;;
esac

source_dir=$(git rev-parse --show-toplevel)
cd "$source_dir"
work="$RUNNER_TEMP/dwc3-atc-$profile"
# Refuse reused output: a stale object must never satisfy this compile gate.
mkdir "$work"
output="$work/output"
artifacts="$work/artifacts"
mkdir "$output" "$artifacts"
fragment="$artifacts/requested.config"
compile_exit=NOT_RUN
stage=configuration

finish()
{
	local rc=$?
	trap - EXIT
	{
		printf 'last_stage=%s\ncompile_exit=%s\nscript_exit=%s\n' "$stage" "$compile_exit" "$rc"
		if [[ -f $output/.config ]]; then
			cp "$output/.config" "$artifacts/resolved.config"
			sha256sum "$artifacts/resolved.config"
		fi
	} | tee -a "$artifacts/receipt.txt"
	exit "$rc"
}
trap finish EXIT

{
	printf 'profile=%s\nsource_commit=%s\nsource_tree=%s\n' "$profile" \
		"$(git rev-parse HEAD)" "$(git rev-parse HEAD^{tree})"
	printf 'github_sha=%s\npr_head=%s\npr_base=%s\n' "${GITHUB_SHA:-unset}" \
		"${PR_HEAD_SHA:-unset}" "${PR_BASE_SHA:-unset}"
	printf 'requested_USB_DWC3=%s\nrequested_USB_DWC3_APPLE=%s\nrequested_PHY_APPLE_ATC=%s\n' \
		"$dwc3" "$dwc3" "$atc"
	clang --version
	ld.lld --version
} | tee "$artifacts/receipt.txt"

cat > "$fragment" <<EOF
CONFIG_MODULES=y
CONFIG_ARCH_APPLE=y
CONFIG_OF=y
CONFIG_PM=y
CONFIG_USB_SUPPORT=y
CONFIG_USB=y
CONFIG_USB_GADGET=y
CONFIG_USB_XHCI_HCD=y
CONFIG_TYPEC=y
CONFIG_GENERIC_PHY=y
CONFIG_USB_DWC3=$dwc3
CONFIG_USB_DWC3_APPLE=$dwc3
# CONFIG_USB_DWC3_HOST is not set
# CONFIG_USB_DWC3_GADGET is not set
CONFIG_USB_DWC3_DUAL_ROLE=y
EOF
if [[ $atc == n ]]; then
	echo '# CONFIG_PHY_APPLE_ATC is not set' >> "$fragment"
else
	printf 'CONFIG_PHY_APPLE_ATC=%s\n' "$atc" >> "$fragment"
fi
sha256sum "$fragment" | tee -a "$artifacts/receipt.txt"

export KCONFIG_CONFIG="$output/.config"
kmake=(make -C "$source_dir" O="$output" ARCH=arm64 LLVM=1)
"${kmake[@]}" defconfig 2>&1 | tee "$artifacts/defconfig.log"
scripts/kconfig/merge_config.sh -m -O "$output" "$output/.config" "$fragment" \
	2>&1 | tee "$artifacts/merge-config.log"
"${kmake[@]}" olddefconfig 2>&1 | tee "$artifacts/olddefconfig.log"

assert_config()
{
	local symbol=$1 expected=$2 actual
	actual=$(scripts/config --file "$output/.config" --state "$symbol")
	printf 'resolved_%s=%s\n' "$symbol" "$actual" | tee -a "$artifacts/receipt.txt"
	if [[ $actual != "$expected" ]]; then
		echo "$symbol resolved to $actual; expected $expected" >&2
		exit 1
	fi
}
# Assert the legal configuration after Kconfig resolution, before invoking CC.
assert_config USB_DWC3 "$dwc3"
assert_config USB_DWC3_APPLE "$dwc3"
assert_config PHY_APPLE_ATC "$atc"
for symbol in MODULES ARCH_APPLE OF PM USB_SUPPORT USB USB_GADGET USB_XHCI_HCD \
	TYPEC GENERIC_PHY USB_DWC3_DUAL_ROLE; do
	assert_config "$symbol" y
done
cp "$output/.config" "$artifacts/resolved.config"
sha256sum "$artifacts/resolved.config" | tee -a "$artifacts/receipt.txt"

target=drivers/usb/dwc3/dwc3-apple.o
command=("${kmake[@]}" -j2 V=1 "$target")
printf '%q ' "${command[@]}" | tee "$artifacts/compile-command.txt"
printf '\n' | tee -a "$artifacts/compile-command.txt"
stage=compile
set +e
"${command[@]}" 2>&1 | tee "$artifacts/compile.log"
pipeline_status=("${PIPESTATUS[@]}")
set -e
compile_exit=${pipeline_status[0]}
if [[ $compile_exit != 0 ]]; then
	exit "$compile_exit"
fi
if [[ ${pipeline_status[1]} != 0 ]]; then
	exit "${pipeline_status[1]}"
fi
stage=object-verification
test -s "$output/$target"
sha256sum "$output/$target" | tee -a "$artifacts/receipt.txt"
cp "$output/drivers/usb/dwc3/.dwc3-apple.o.cmd" "$artifacts/kbuild-command.txt"
stage=complete

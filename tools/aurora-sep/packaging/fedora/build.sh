#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Fedora package for Touch ID on the Apple SEP:
#
#  - aurora-sep: the driver loader, its service and the fprintd policy.
#
# libfprint with the aurora driver is built and packaged in
# https://github.com/aurora-silicon/aurora-sep-userspace.
#
# It installs the build dependencies with dnf, so run it as root in a
# throwaway container of the Fedora release you are building for:
#
#   podman run --rm -v <linux tree>:/src:ro,z -v <output dir>:/out:z \
#       registry.fedoraproject.org/fedora:45 /src/<path to this script> /out
#
# The binary and source packages land in the output directory.

set -eu

out=${1:?usage: build.sh <output directory>}
here=$(cd "$(dirname "$0")" && pwd)
sep=$(cd "$here/../.." && pwd)
mkdir -p "$out"

top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT
rpmtop=$top/rpmbuild

dnf -y install rpm-build systemd-rpm-macros

# The loader, its units and the desktop integration, from this tree.
dnf -y builddep "$here/aurora-sep.spec"
rpmbuild --define "_topdir $rpmtop" --define "_sourcedir $sep" \
    -ba "$here/aurora-sep.spec"

cp -p "$rpmtop"/RPMS/*/*.rpm "$rpmtop"/SRPMS/*.rpm "$out/"
ls -l "$out"

#!/usr/bin/env bash
# Builds ghostcon's Arch package (.pkg.tar.zst) from the current working
# tree using packaging/PKGBUILD.
#
# PKGBUILD's source=() is empty on purpose -- this script stages the repo
# itself into makepkg's expected src/ layout instead of having makepkg
# fetch a tarball/VCS ref, so a package can be built straight from
# whatever's checked out locally (including uncommitted changes).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/ghostcon-pkgbuild.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

mkdir -p "$work_dir/src"
rsync -a \
    --exclude='.git/' \
    --exclude='build-*/' \
    --exclude='dist/' \
    "$repo_root/" "$work_dir/src/ghostcon/"

cp "$repo_root/packaging/PKGBUILD" "$work_dir/PKGBUILD"

(
    cd "$work_dir"
    PKGEXT='.pkg.tar.zst' makepkg -f
)

dist_dir="$repo_root/dist"
mkdir -p "$dist_dir"
cp -v "$work_dir"/*.pkg.tar.zst "$dist_dir/"

echo "package(s) written to $dist_dir"

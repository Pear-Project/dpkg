#!/usr/bin/env bash
# Downloads the newest .deb release assets from Deadly-Signal/cachy-kernel-debian
# and rebrands them from "cachyos" to "pearos" (DEBIAN/control fields +
# filename), ready for publish.sh.
#
# Usage: ./cachy-kernel-repack.sh [--tag <release-tag>] [--out <dir>]
#
# Without --tag, uses the upstream repo's latest release.
# Prints the upstream tag it used to stdout on the last line, so callers can
# record it (see .github/workflows/cachy-kernel-watch.yaml).

set -euo pipefail

UPSTREAM_REPO="Deadly-Signal/cachy-kernel-debian"

nc='\033[0m'
red='\033[0;31m'
green='\033[0;32m'
white='\033[1;37m'
ul='\033[4m'

tag=""
out="$(pwd)/cachy-kernel-out"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag) tag="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^#//; s/^ //'
            exit 0
            ;;
        *)
            echo -e "${red}[ERROR]${nc} Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$tag" ]]; then
    tag="$(gh release view --repo "$UPSTREAM_REPO" --json tagName -q .tagName)"
fi

echo -e "${ul}${white}Upstream release:${nc} $UPSTREAM_REPO @ $tag" >&2

rm -rf "$out"
mkdir -p "$out/download" "$out/repacked"

# linux-image-*-dbg is debug symbols, not something we want to publish;
# everything else (image, headers, libc-dev) we do.
gh release download "$tag" --repo "$UPSTREAM_REPO" --dir "$out/download" \
    --pattern 'linux-image-*.deb' \
    --pattern 'linux-headers-*.deb' \
    --pattern 'linux-libc-dev*.deb' \
    --pattern 'BUILD-MANIFEST.txt'

shopt -s nullglob
debs=("$out/download"/*.deb)
if [[ ${#debs[@]} -eq 0 ]]; then
    echo -e "${red}[ERROR]${nc} No .deb assets found in release $tag" >&2
    exit 1
fi

for deb in "${debs[@]}"; do
    base="$(basename "$deb")"
    if [[ "$base" == *-dbg_* ]]; then
        echo "Skipping debug package: $base" >&2
        continue
    fi

    echo -e "\n${ul}${white}Rebranding${nc} $base" >&2

    extract_dir="$out/extract/${base%.deb}"
    mkdir -p "$extract_dir"
    dpkg-deb -R "$deb" "$extract_dir"

    # Rebrand the DEBIAN/control metadata. The kernel's own build (release
    # string baked into vmlinuz, /lib/modules/<uname -r>/, etc.) still says
    # cachyos-debian internally - actually changing that means rebuilding
    # the kernel with a different LOCALVERSION, which this script does not
    # do. This is a packaging-identity rebrand, not a rebuild.
    sed -i \
        -e 's/^Package: linux-\(image\|headers\)-\(.*\)-cachyos-debian$/Package: linux-\1-\2-pearos/' \
        -e 's/^Source: .*/Source: pearos-cachy-kernel/' \
        -e 's/^Maintainer:.*/Maintainer: Alexandru Balan <alxb421@gmail.com>/' \
        -e 's/CachyOS/pearOS/g' \
        -e 's/cachyos/pearos/g' \
        "$extract_dir/DEBIAN/control"

    new_base="${base/-cachyos-debian/-pearos}"
    new_base="${new_base/cachyos/pearos}"

    find "$extract_dir" -mindepth 1 -maxdepth 1 ! -name DEBIAN -exec chmod -R go-w {} +
    dpkg-deb -b "$extract_dir" "$out/repacked/$new_base"
    echo -e "${green}[OK]${nc} $out/repacked/$new_base" >&2
done

echo -e "\n${green}Repacked $(ls "$out/repacked" | wc -l) package(s) from $tag into $out/repacked/${nc}\n" >&2

# Last line: the tag actually used, for the workflow to record.
echo "$tag"

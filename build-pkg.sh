#!/usr/bin/env bash
# Generic .deb builder, played the same role makepkg plays for PKGBUILD.
#
# Usage: ./build-pkg.sh <package-dir>
#
# Reads <package-dir>/DEBBUILD, a bash file declaring:
#   pkgname, pkgver, pkgrel, pkgdesc, arch, url, license, section, priority
#   provides, conflicts, replaces  optional control-file strings, verbatim
#   builddepends=()   packages needed only to build (installed via apt, not
#                      recorded in the .deb)
#   depends=()        extra runtime deps to force in addition to whatever
#                      dpkg-shlibdeps auto-detects from linked libraries
#   depends_exclude=() package names to drop from dpkg-shlibdeps' output
#                      (e.g. a Debian-only lib split not present by that
#                      name on Ubuntu-derivatives); pair with depends=()
#                      to supply a portable replacement
#   build()           runs with $startdir/$srcdir available, produces a build
#   package()         installs the built output into $pkgdir
#   preinst/postinst/prerm/postrm  optional strings; each becomes the literal
#                      contents of the matching DEBIAN maintainer script
#                      (include your own #!/bin/sh and set -e)
#   triggers           optional string, becomes DEBIAN/triggers verbatim
#                      (e.g. "interest-noawait /some/path")
#
# Output: <package-dir>/<pkgname>_<pkgver>-<pkgrel>_<arch>.deb

set -euo pipefail

nc='\033[0m'
red='\033[0;31m'
green='\033[0;32m'
yellow='\033[1;33m'
white='\033[1;37m'
ul='\033[4m'

if [[ $# -ne 1 ]]; then
    echo "Usage: $(basename "$0") <package-dir>" >&2
    exit 1
fi

startdir="$(cd "$1" && pwd)"
debbuild="$startdir/DEBBUILD"

if [[ ! -f "$debbuild" ]]; then
    echo -e "${red}[ERROR]${nc} No DEBBUILD found in $startdir" >&2
    exit 1
fi

# ── Load package metadata ────────────────────────────────────────────────────

builddepends=()
depends=()
depends_exclude=()
arch=()
pkgrel=1
license=""
section="misc"
priority="optional"
url=""
provides=""
conflicts=""
replaces=""

# shellcheck source=/dev/null
source "$debbuild"

: "${pkgname:?DEBBUILD must set pkgname}"
: "${pkgver:?DEBBUILD must set pkgver}"
: "${pkgdesc:?DEBBUILD must set pkgdesc}"

if [[ ${#arch[@]} -eq 0 ]]; then
    arch=("$(dpkg --print-architecture)")
fi

# For a multi-arch DEBBUILD (e.g. arch=('amd64' 'arm64')), the actual output
# architecture is whatever this runner really is, NOT just arch[0] -- CI
# builds each arch on its own matching runner, and mislabeling an arm64
# build as amd64 (or vice versa) would produce a broken .deb. arch=('all')
# stays literally "all" (architecture-independent), the one case where the
# host's real arch doesn't matter.
if [[ "${#arch[@]}" -eq 1 && "${arch[0]}" == "all" ]]; then
    build_arch="all"
else
    build_arch="$(dpkg --print-architecture)"
    match=false
    for a in "${arch[@]}"; do
        [[ "$a" == "$build_arch" ]] && match=true && break
    done
    if ! $match; then
        echo -e "${red}[ERROR]${nc} DEBBUILD declares arch=(${arch[*]}) but this runner is $build_arch" >&2
        exit 1
    fi
fi

builddir="$startdir/.dpkgbuild"
srcdir="$builddir/src"
pkgdir="$builddir/pkg"

rm -rf "$srcdir" "$pkgdir"
mkdir -p "$srcdir" "$pkgdir"

# ── Build dependencies ───────────────────────────────────────────────────────

if [[ ${#builddepends[@]} -gt 0 ]]; then
    echo -e "\n${ul}${white}Checking build dependencies${nc}\n"
    missing=()
    for pkg in "${builddepends[@]}"; do
        if dpkg -s "$pkg" &>/dev/null; then
            echo -e "  ${green}[OK]${nc} $pkg"
        else
            echo -e "  ${yellow}[MISSING]${nc} $pkg"
            missing+=("$pkg")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo -e "\n${yellow}Installing missing build dependencies...${nc}\n"
        if [[ "$EUID" -ne 0 ]]; then
            sudo apt-get update
            sudo apt-get install -y --no-install-recommends "${missing[@]}"
        else
            apt-get update
            apt-get install -y --no-install-recommends "${missing[@]}"
        fi
    fi
fi

# ── build() ───────────────────────────────────────────────────────────────────

echo -e "\n${ul}${white}Building $pkgname $pkgver-$pkgrel ($build_arch)${nc}\n"

# Qt Quick's shader baker (qsb, invoked by qmlcachegen for QtQuick.Effects/
# MultiEffect-based QML) tries to open a real GPU/display context unless
# told otherwise, and hangs indefinitely instead of failing when none is
# available (e.g. this headless CI container) -- offscreen makes it use a
# software fallback instead of blocking forever. No effect on non-Qt builds.
export QT_QPA_PLATFORM=offscreen

type build &>/dev/null || { echo -e "${red}[ERROR]${nc} DEBBUILD has no build() function" >&2; exit 1; }
( cd "$startdir" && build )

# ── package() ─────────────────────────────────────────────────────────────────

type package &>/dev/null || { echo -e "${red}[ERROR]${nc} DEBBUILD has no package() function" >&2; exit 1; }
( cd "$startdir" && package )

mkdir -p "$pkgdir/DEBIAN"

# ── Auto-detect shared library deps ─────────────────────────────────────────

shlibs_depends=""
if command -v dpkg-shlibdeps &>/dev/null && command -v file &>/dev/null; then
    mapfile -t elf_files < <(find "$pkgdir" -type f -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print 2>/dev/null)
    if [[ ${#elf_files[@]} -gt 0 ]]; then
        # dpkg-shlibdeps insists on running from a source root with debian/control
        # present, even in -O (stdout) mode. Fake a minimal one.
        mkdir -p "$builddir/debian"
        {
            echo "Source: $pkgname"
            echo "Priority: $priority"
            echo "Maintainer: ${maintainer:-Alexandru Balan <alxb421@gmail.com>}"
            echo ""
            echo "Package: $pkgname"
            echo "Architecture: $build_arch"
            echo "Description: $pkgdesc"
        } > "$builddir/debian/control"

        shlibs_out="$(cd "$builddir" && dpkg-shlibdeps -O --ignore-missing-info "${elf_files[@]}" 2>&1)" && {
            shlibs_depends="$(printf '%s\n' "$shlibs_out" | sed -n 's/^shlibs:Depends=//p')"
        } || {
            echo -e "${yellow}[WARN]${nc} dpkg-shlibdeps could not resolve all library deps; falling back to manual depends[] only." >&2
            echo "$shlibs_out" >&2
        }
    fi
else
    echo -e "${yellow}[WARN]${nc} dpkg-shlibdeps and/or file not found (install dpkg-dev, file) - only manual depends[] will be recorded." >&2
fi

# Drop auto-detected entries whose package name doesn't actually exist on
# every target distro (e.g. a lib split into its own binary package on
# Debian but bundled into a differently-named package on Ubuntu-derivatives
# like KDE neon/Kubuntu). Replace those via depends[] instead.
if [[ ${#depends_exclude[@]} -gt 0 && -n "$shlibs_depends" ]]; then
    IFS=',' read -ra dep_entries <<< "$shlibs_depends"
    filtered=()
    for entry in "${dep_entries[@]}"; do
        entry="${entry#"${entry%%[![:space:]]*}"}"
        pkg_name="${entry%% *}"
        keep=1
        for ex in "${depends_exclude[@]}"; do
            [[ "$pkg_name" == "$ex" ]] && { keep=0; break; }
        done
        if [[ "$keep" -eq 1 ]]; then
            filtered+=("$entry")
        else
            echo -e "${yellow}[INFO]${nc} Excluding auto-detected dependency: $entry" >&2
        fi
    done
    shlibs_depends=""
    for f in "${filtered[@]}"; do
        if [[ -z "$shlibs_depends" ]]; then
            shlibs_depends="$f"
        else
            shlibs_depends="$shlibs_depends, $f"
        fi
    done
fi

all_depends="$shlibs_depends"
if [[ ${#depends[@]} -gt 0 ]]; then
    manual_depends=""
    for d in "${depends[@]}"; do
        if [[ -z "$manual_depends" ]]; then
            manual_depends="$d"
        else
            manual_depends="$manual_depends, $d"
        fi
    done
    if [[ -n "$all_depends" ]]; then
        all_depends="$all_depends, $manual_depends"
    else
        all_depends="$manual_depends"
    fi
fi

# ── DEBIAN/control ───────────────────────────────────────────────────────────

installed_size="$(du -sk "$pkgdir" --exclude=DEBIAN 2>/dev/null | cut -f1)"

{
    echo "Package: $pkgname"
    echo "Version: $pkgver-$pkgrel"
    echo "Section: $section"
    echo "Priority: $priority"
    echo "Architecture: $build_arch"
    [[ -n "$all_depends" ]] && echo "Depends: $all_depends"
    [[ -n "$provides" ]] && echo "Provides: $provides"
    [[ -n "$conflicts" ]] && echo "Conflicts: $conflicts"
    [[ -n "$replaces" ]] && echo "Replaces: $replaces"
    echo "Installed-Size: ${installed_size:-0}"
    echo "Maintainer: ${maintainer:-Alexandru Balan <alxb421@gmail.com>}"
    [[ -n "$url" ]] && echo "Homepage: $url"
    echo "Description: $pkgdesc"
} > "$pkgdir/DEBIAN/control"

# ── Maintainer scripts ───────────────────────────────────────────────────────

for script_name in preinst postinst prerm postrm; do
    script_content="${!script_name:-}"
    if [[ -n "$script_content" ]]; then
        printf '%s\n' "$script_content" > "$pkgdir/DEBIAN/$script_name"
        chmod 755 "$pkgdir/DEBIAN/$script_name"
    fi
done

# DEBIAN/triggers: e.g. triggers="interest-noawait /some/path" - lets a
# package react when another package touches files under a watched path,
# without that other package needing to know we exist.
if [[ -n "${triggers:-}" ]]; then
    printf '%s\n' "$triggers" > "$pkgdir/DEBIAN/triggers"
fi

# ── Build the .deb ───────────────────────────────────────────────────────────

deb_name="${pkgname}_${pkgver}-${pkgrel}_${build_arch}.deb"
find "$pkgdir" -mindepth 1 -maxdepth 1 ! -name DEBIAN -exec chmod -R go-w {} +
dpkg-deb --build --root-owner-group "$pkgdir" "$startdir/$deb_name"

echo -e "\n${green}Built:${nc} $startdir/$deb_name\n"

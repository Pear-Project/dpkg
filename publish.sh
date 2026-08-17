#!/usr/bin/env bash
# Drop built .deb(s) into a debian-package-repo checkout, regenerate the apt
# metadata for that <arch>/<channel>/<release> directory, sign it, and
# (optionally) commit + push.
#
# This is the apt equivalent of pkgbuilds/sign.sh, but instead of rclone-ing
# individually-signed packages up to R2, it writes into a local checkout of
# the debian-package-repo git repo (../debian-package-repo by default) and
# signs the repo's Release file, since that's how apt trust works.
#
# Usage:
#   ./publish.sh <deb-file-or-dir> [options]
#
# Options:
#   --repo <path>       debian-package-repo checkout (default: ../debian-package-repo)
#   --channel <name>    main | testing                (default: main)
#   --release <name>    latest | pahoe | monterey | ...(default: latest)
#   --push              git add/commit/push in --repo after publishing
#
# Arch is read from each .deb's control file and mapped:
#   amd64 -> x86_64, arm64 -> aarch64 (matches debian-package-repo's dir names)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

nc='\033[0m'
red='\033[0;31m'
green='\033[0;32m'
yellow='\033[1;33m'
white='\033[1;37m'
ul='\033[4m'

repo="$SCRIPT_DIR/../debian-package-repo"
channel="main"
release="latest"
do_push=0
input=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo) repo="$2"; shift 2 ;;
        --channel) channel="$2"; shift 2 ;;
        --release) release="$2"; shift 2 ;;
        --push) do_push=1; shift ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^#//; s/^ //'
            exit 0
            ;;
        *)
            if [[ -n "$input" ]]; then
                echo -e "${red}[ERROR]${nc} Unexpected argument: $1" >&2
                exit 1
            fi
            input="$1"
            shift
            ;;
    esac
done

if [[ -z "$input" ]]; then
    echo -e "${red}[ERROR]${nc} Usage: $(basename "$0") <deb-file-or-dir> [options]" >&2
    exit 1
fi

repo="$(cd "$repo" && pwd)"

mapfile -t debs < <(
    if [[ -d "$input" ]]; then
        find "$input" -maxdepth 1 -name '*.deb'
    else
        printf '%s\n' "$input"
    fi
)

if [[ ${#debs[@]} -eq 0 ]]; then
    echo -e "${red}[ERROR]${nc} No .deb files found at: $input" >&2
    exit 1
fi

# Echoes one repo arch-dir name per line. Architecture: all goes into every
# arch dir this repo serves, since it's installable regardless of target arch
# but our layout keys the apt suite by arch in the URL.
deb_arch_dirs() {
    local deb_arch
    deb_arch="$(dpkg-deb -f "$1" Architecture)"
    case "$deb_arch" in
        amd64) echo "x86_64" ;;
        arm64) echo "aarch64" ;;
        all) printf '%s\n' "x86_64" "aarch64" ;;
        *) echo -e "${red}[ERROR]${nc} Unknown/unsupported Debian arch: $deb_arch" >&2; exit 1 ;;
    esac
}

declare -A touched_dirs=()

for deb in "${debs[@]}"; do
    while IFS= read -r arch_dir; do
        target="$repo/$arch_dir/$channel/$release"
        mkdir -p "$target"
        echo -e "${ul}${white}Publishing${nc} $(basename "$deb") -> ${target#"$repo"/}"
        cp -f "$deb" "$target/"
        touched_dirs["$target"]=1
    done < <(deb_arch_dirs "$deb")
done

# ── Regenerate apt metadata (flat, per-directory repo: "deb ... ./") ────────

if ! command -v apt-ftparchive &>/dev/null; then
    echo -e "${red}[ERROR]${nc} apt-ftparchive not found (install apt-utils)." >&2
    exit 1
fi

GPG_PASSPHRASE_FILE="${GPG_PASSPHRASE_FILE:-$SCRIPT_DIR/.gpg-passphrase}"
[[ -f "$GPG_PASSPHRASE_FILE" ]] || GPG_PASSPHRASE_FILE=""
have_signing_key=0
if gpg --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec'; then
    have_signing_key=1
fi

for dir in "${!touched_dirs[@]}"; do
    echo -e "\n${ul}${white}Rebuilding apt metadata${nc} in ${dir#"$repo"/}\n"
    (
        cd "$dir"
        apt-ftparchive packages . > Packages
        gzip -9c Packages > Packages.gz
        apt-ftparchive release . > Release

        if ((have_signing_key)); then
            if [[ -n "$GPG_PASSPHRASE_FILE" ]]; then
                gpg --batch --yes --pinentry-mode loopback \
                    --passphrase-file "$GPG_PASSPHRASE_FILE" \
                    --clearsign -o InRelease Release
                gpg --batch --yes --pinentry-mode loopback \
                    --passphrase-file "$GPG_PASSPHRASE_FILE" \
                    -abs -o Release.gpg Release
            else
                gpg --batch --yes --use-agent --clearsign -o InRelease Release
                gpg --batch --yes --use-agent -abs -o Release.gpg Release
            fi
            echo -e "${green}[OK]${nc} Signed InRelease / Release.gpg"
        else
            rm -f InRelease Release.gpg
            echo -e "${yellow}[WARN]${nc} No GPG key available - Release left unsigned (InRelease/Release.gpg not written). Clients need [trusted=yes] for this repo until it's signed."
        fi
    )
done

if ((do_push)); then
    echo -e "\n${ul}${white}Committing + pushing in $repo${nc}\n"
    (
        cd "$repo"
        git add -A -- "${!touched_dirs[@]}"
        if git diff --cached --quiet; then
            echo "Nothing to commit."
        else
            git commit -m "Publish $(printf '%s ' "${debs[@]##*/}")"
            # Matrix builds can land here concurrently; rebase-and-retry a few
            # times before giving up.
            for attempt in 1 2 3 4 5; do
                if git push; then
                    break
                fi
                echo -e "${yellow}[WARN]${nc} push failed (attempt $attempt), pulling --rebase and retrying..."
                git pull --rebase
                if [[ "$attempt" == 5 ]]; then
                    echo -e "${red}[ERROR]${nc} Giving up after 5 attempts." >&2
                    exit 1
                fi
            done
        fi
    )
fi

echo -e "\n${green}Done.${nc}\n"

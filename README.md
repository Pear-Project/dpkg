# PearOS dpkg

[PearOS](https://pearos.xyz/) `dpkg` is the Debian/apt counterpart to
[`pearos-pkgbuilds`](https://github.com/pearOS-archlinux/pearos-pkgbuilds):
a collection of build scripts for producing `.deb` packages for pearOS on
Debian-based systems, published to
[apt.pearos.xyz](https://apt.pearos.xyz) (repo:
[`debian-package-repo`](https://github.com/Pear-Project/debian-package-repo)).

## Prerequisites

To run `build-pkg.sh` / `publish.sh` locally (CI installs these automatically):

```sh
sudo apt install build-essential dpkg-dev apt-utils gnupg2 file
```

Plus whatever a given package's `builddepends=()` lists — `build-pkg.sh`
installs those itself via `apt-get` if missing.

## How a package is structured

Each package lives in its own directory, mirroring the PKGBUILD layout but
with a `DEBBUILD` file instead of `PKGBUILD`:

```
<package>/
  DEBBUILD       # metadata + build()/package(), same contract as PKGBUILD
  Makefile       # `make` -> ../build-pkg.sh .
  <source files>
```

A `DEBBUILD` declares:

- `pkgname`, `pkgver`, `pkgrel`, `pkgdesc`, `arch` (Debian arches: `amd64`, `arm64`, ...)
- `url`, `license`, `section`, `priority`, `maintainer`
- `builddepends=()` — apt packages needed only to build (not recorded in the `.deb`)
- `depends=()` — extra runtime deps beyond what `dpkg-shlibdeps` auto-detects
  from the binaries `package()` installs
- `build()` — same as in a PKGBUILD: build the software
- `package()` — install the build output into `$pkgdir` (`$DESTDIR`-style)
- `depends_exclude=()` / `provides` / `conflicts` / `replaces` / `preinst` /
  `postinst` / `prerm` / `postrm` — see `build-pkg.sh`'s header comment

> **KWin effect gotcha:** any package that does `find_package(KWin)` needs
> `libdrm-dev`, `libvulkan-dev` and `pkg-config` in `builddepends` even if
> its own code never touches them directly - `KWinConfig.cmake` pulls them
> in transitively via `find_dependency()`, and CMake fails without them.

## Building a package

```sh
git clone https://github.com/Pear-Project/dpkg.git
cd dpkg/pearos-magiclamp
make                    # or: ../build-pkg.sh .
sudo apt install ./pearos-magiclamp_*.deb
```

`build-pkg.sh` plays the role `makepkg` plays for PKGBUILD: it sources
`DEBBUILD`, installs missing `builddepends` via `apt`, runs `build()` and
`package()`, resolves runtime library dependencies automatically with
`dpkg-shlibdeps`, generates `DEBIAN/control`, and packages everything with
`dpkg-deb`.

## Publishing

`publish.sh` is the apt equivalent of `pkgbuilds/sign.sh`. Where the Arch
side signs each `.pkg.tar.zst` individually and `rclone`s them to R2, apt's
trust model signs the *repository's* `Release` file instead, so this script:

1. Copies the built `.deb`(s) into
   `<debian-package-repo>/<x86_64|aarch64>/<channel>/<release>/`
   (arch is read from the `.deb`, mapped `amd64`→`x86_64`, `arm64`→`aarch64`)
2. Regenerates that directory's apt metadata (`Packages`, `Packages.gz`, `Release`)
   with `apt-ftparchive`
3. Signs `Release` into `InRelease` / `Release.gpg` with the same GPG key
   used to sign the Arch packages
4. Optionally commits and pushes in that checkout (`--push`)

```sh
./publish.sh pearos-magiclamp --repo ../debian-package-repo --channel main --release latest
```

`--repo` just needs to point at a checkout of
[`Pear-Project/debian-package-repo`](https://github.com/Pear-Project/debian-package-repo).
Locally that's a sibling clone (`../debian-package-repo`). In CI it's nested
instead (`debian-package-repo/` inside the `dpkg` checkout) because
`actions/checkout`'s `path:` refuses to point outside `$GITHUB_WORKSPACE` —
see [`build-and-publish.yaml`](.github/workflows/build-and-publish.yaml).

## CI

[`build-and-publish.yaml`](.github/workflows/build-and-publish.yaml) mirrors
`pkgbuilds`' workflow: on push to `main`, it detects which package
directories had a `DEBBUILD` version bump, builds each in a `debian:testing`
container, and publishes + pushes the result to `debian-package-repo`.

It needs these repo secrets:

- `GPG_PRIVATE_KEY` / `GPG_PASSPHRASE` — same signing key as the Arch side
- `DEBIAN_REPO_DEPLOY_TOKEN` — a token (PAT or deploy key) with push access
  to `Pear-Project/debian-package-repo`, since the default `GITHUB_TOKEN`
  can't push cross-repo

Both workflows share the `publish-debian-package-repo` concurrency group so
back-to-back pushes (or a scheduled run landing mid-build) queue instead of
racing to publish at the same time - `publish.sh`'s pull-rebase retry only
survives a clean fast-forward, not two runs regenerating the same
`Packages`/`Release` concurrently.

## Kernel: cachy-kernel-watch.yaml

[`cachy-kernel-watch.yaml`](.github/workflows/cachy-kernel-watch.yaml) polls
[`Deadly-Signal/cachy-kernel-debian`](https://github.com/Deadly-Signal/cachy-kernel-debian)
daily for new releases (that repo has no push/tag trigger of its own to hook
into - it's manual-dispatch-only upstream, so polling is the only option).
On a new release, [`cachy-kernel-repack.sh`](cachy-kernel-repack.sh)
downloads its `.deb` assets and rebrands them (`cachyos` → `pearos` in
`DEBIAN/control` and the filename; skips the `-dbg` package).

`linux-headers-*` and `linux-libc-dev` are small and go through the normal
apt repo (`x86_64/main/latest`, via `publish.sh`). `linux-image-*` is
~130-150MB - over GitHub's 100MB per-file push limit - so it's published as
a [`debian-package-repo` Release](https://github.com/Pear-Project/debian-package-repo/releases)
asset instead, downloaded and installed manually rather than via `apt`.

## Packages

| Package | Description |
|---------|-------------|
| `pearos-magiclamp` | KWin window minimization effect - Pearos Magic Lamp |
| `pearos-window-borders` | pearOS-style KWin window border |
| `pearos-window-tinter` | KWin effect that tints window backgrounds with the wallpaper color behind them (was `pearos-tinter`) |
| `pearos-muternvf` | Mutern Fonts for pearOS |
| `pearos-liquidgel` | Fork of the KWin Blur effect for pearOS with additional features (including force blur) and bug fixes |
| `pearos-filesystem` | pearOS Goldwing branding (os-release, issue, logo) - branding-only port of `pearOS-archlinux/filesystem`; see the package's own note for why |
| `pearos-settings` | Settings for pearOS Goldwing - themes, wallpapers, `/etc/skel`, Plasma/SDDM/Plymouth branding (depends on several packages not yet in this repo) |
| `pearos-calculator` | pearOS Calculator |
| `pearos-calendar` | pearOS Calendar - modern Qt6 calendar application for pearOS |
| `pearos-contacts` | pearOS Contacts - modern Qt6 contacts application for pearOS |
| `pearos-notes` | pearOS Notes - modern Qt6 notes application for pearOS |
| `pearos-todo` | pearOS ToDo - modern Qt6 to-do application for pearOS |
| `pearos-icons` | Icons for pearOS (924MB, `arch=all`) |
| `pearos-dock` | Tahoe-style dock for KDE Plasma 6 |
| `pearos-livecd-desktop` | Default pearOS installer shortcut for the live desktop |
| `linux-image/headers-*-pearos`, `linux-libc-dev` | Rebranded CachyOS kernel, auto-published by `cachy-kernel-watch.yaml` (see above) |
| `pearos-zshconfig` | pearOS NiceC0re ZSH config file |
| `pearos-bootsound` | Boot sound service for pearOS |
| `pearos-notch` | Dynamic notch widget for Linux |
| `pearos-appmenu` | Global Menu applet for Plasma Desktop (pearOS custom build) |

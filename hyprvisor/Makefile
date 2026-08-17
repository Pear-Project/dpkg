PKGNAME  := hyprvisor
BUILDDIR := $(CURDIR)/.dpkgbuild

.PHONY: all clean

all:
	../build-pkg.sh .

clean:
	rm -rf "$(BUILDDIR)" build
	rm -f $(PKGNAME)_*.deb

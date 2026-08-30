#!/bin/sh
# Build cixfan-dkms_<version>_all.deb.  Needs dpkg-deb and fakeroot only.
set -e

NAME=cixfan
VERSION=1.1
DEBREV=2
PKG=${NAME}-dkms

src=$(cd "$(dirname "$0")" && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

root=$stage/$PKG
install -d "$root/DEBIAN"
install -d "$root/usr/src/$NAME-$VERSION/acpi"
install -d "$root/usr/share/doc/$PKG"
install -d "$root/etc/modules-load.d"

install -m 644 "$src/cixfan.c" "$src/Makefile" "$src/dkms.conf" \
	"$root/usr/src/$NAME-$VERSION/"
install -m 644 "$src/acpi/cix-ec-raw.asl" "$src/acpi/cix_ec_raw.hex" \
	"$root/usr/src/$NAME-$VERSION/acpi/"

install -m 644 "$src/packaging/control" "$root/DEBIAN/control"
install -m 644 "$src/packaging/conffiles" "$root/DEBIAN/conffiles"
install -m 755 "$src/packaging/postinst" "$root/DEBIAN/postinst"
install -m 755 "$src/packaging/prerm" "$root/DEBIAN/prerm"

install -m 644 "$src/README.md" "$root/usr/share/doc/$PKG/README.md"
install -m 644 "$src/packaging/copyright" "$root/usr/share/doc/$PKG/copyright"
gzip -9nc "$src/packaging/changelog" \
	> "$root/usr/share/doc/$PKG/changelog.Debian.gz"
chmod 644 "$root/usr/share/doc/$PKG/changelog.Debian.gz"

echo "$NAME" > "$root/etc/modules-load.d/$NAME.conf"
chmod 644 "$root/etc/modules-load.d/$NAME.conf"

out=$src/${PKG}_${VERSION}-${DEBREV}_all.deb
fakeroot dpkg-deb --build --root-owner-group "$root" "$out"
echo "built $out"

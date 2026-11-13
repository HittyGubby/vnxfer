#!/usr/bin/env bash
# Builds a local Arch package and installs it with paru -U.
set -euo pipefail

VER=$(sed -n 's/^pkgver=//p' "$(dirname "$0")/arch/PKGBUILD")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/vnxfer-arch}

rm -rf "$WORK"
mkdir -p "$WORK/vnxfer-$VER"
tar -C "$ROOT" \
  --exclude=build --exclude=.git --exclude='*.pkg.tar.*' \
  -cf - . | tar -C "$WORK/vnxfer-$VER" -xf -
tar -C "$WORK" -czf "$WORK/vnxfer-$VER.tar.gz" "vnxfer-$VER"
cp "$ROOT/packaging/arch/PKGBUILD" "$WORK/"

(cd "$WORK" && makepkg -f --noconfirm)

PKG=$(ls "$WORK"/vnxfer-"$VER"-*.pkg.tar.zst | head -1)
echo "built: $PKG"
if paru -U --noconfirm "$PKG"; then
  echo "installed with paru"
else
  echo "install step failed; run: paru -U $PKG" >&2
  exit 1
fi

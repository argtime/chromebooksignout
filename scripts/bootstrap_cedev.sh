#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${1:-$ROOT_DIR/.cedev}"
VERSION="${CEDEV_VERSION:-v14.1}"
ARCHIVE="CEdev-Linux.tar.gz"
URL="https://github.com/CE-Programming/toolchain/releases/download/${VERSION}/${ARCHIVE}"

mkdir -p "$INSTALL_DIR"
TMP_DIR="$(mktemp -d)"
trap 'rm -r "$TMP_DIR"' EXIT

echo "Downloading CEdev ${VERSION} from ${URL} ..."
wget -q -O "$TMP_DIR/$ARCHIVE" "$URL"
tar -xzf "$TMP_DIR/$ARCHIVE" -C "$TMP_DIR"

if [ -e "$INSTALL_DIR" ]; then
  BACKUP="${INSTALL_DIR}.bak.$(date +%s)"
  mv "$INSTALL_DIR" "$BACKUP"
  echo "Existing install moved to: $BACKUP"
fi
mv "$TMP_DIR/CEdev" "$INSTALL_DIR"

echo "Installed to: $INSTALL_DIR"
echo "Use with: CEDEV=$INSTALL_DIR PATH=$INSTALL_DIR/bin:\$PATH make"

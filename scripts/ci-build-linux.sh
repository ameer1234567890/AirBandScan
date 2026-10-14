#!/usr/bin/env bash
# Build and package AirBandScan on Debian / Raspberry Pi OS.
# CI runs it inside a debian:bookworm container for each architecture:
#   scripts/ci-build-linux.sh <package-name> <version>
# e.g. scripts/ci-build-linux.sh linux-x86_64 1.0.0  ->  dist/airbandscan-1.0.0-linux-x86_64.tar.gz
set -euo pipefail

NAME="${1:?package name, e.g. linux-x86_64}"
VERSION="${2:-dev}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config file ca-certificates \
    librtlsdr-dev libairspy-dev libhackrf-dev >/dev/null

BUILD="build-ci-$NAME"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPP_VERSION="$VERSION"
ninja -C "$BUILD" airbandscan

PKG="airbandscan-$VERSION-$NAME"
mkdir -p "dist/$PKG"
cp "$BUILD/airbandscan" README.md LICENSE airband.conf.example "dist/$PKG/"
strip "dist/$PKG/airbandscan"
cat > "dist/$PKG/INSTALL.txt" <<TXT
AirBandScan $VERSION ($NAME)

1. Install the runtime libraries (Debian / Ubuntu / Raspberry Pi OS):
     sudo apt install librtlsdr0 libairspy0 libhackrf0 libmp3lame0
2. Allow the SDR without root: sudo apt install rtl-sdr   (installs the udev rules)
   and unplug / replug the dongle.
3. Run:  ./airbandscan
   then open http://localhost:8080/  (or http://<this-computer-ip>:8080/)
TXT

file "dist/$PKG/airbandscan"
"dist/$PKG/airbandscan" --help | head -1 || true
tar -C dist -czf "dist/$PKG.tar.gz" "$PKG"
rm -rf "dist/$PKG" "$BUILD"
ls -la dist

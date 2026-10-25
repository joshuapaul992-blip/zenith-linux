#!/usr/bin/env bash
# Install the host packages needed to build Zenith Linux.
# Supports Debian/Ubuntu (apt), Fedora (dnf), and Arch (pacman).
set -euo pipefail

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO="sudo"

if command -v apt-get >/dev/null 2>&1; then
  $SUDO apt-get update
  $SUDO apt-get install -y --no-install-recommends \
    build-essential bc bison flex \
    libelf-dev libssl-dev \
    cpio xz-utils gzip squashfs-tools \
    wget curl ca-certificates \
    xorriso isolinux syslinux-common \
    qemu-system-x86
elif command -v dnf >/dev/null 2>&1; then
  $SUDO dnf install -y \
    gcc gcc-c++ make bc bison flex \
    elfutils-libelf-devel openssl-devel \
    cpio xz gzip squashfs-tools \
    wget curl ca-certificates \
    xorriso syslinux \
    qemu-system-x86
elif command -v pacman >/dev/null 2>&1; then
  $SUDO pacman -Sy --needed --noconfirm \
    base-devel bc bison flex \
    libelf openssl \
    cpio xz gzip squashfs-tools \
    wget curl \
    xorriso syslinux \
    qemu-system-x86
else
  echo "Unsupported package manager. Install manually:" >&2
  echo "  gcc make bc bison flex libelf-dev libssl-dev cpio xz gzip wget curl xorriso syslinux qemu" >&2
  exit 1
fi

echo "Host build dependencies installed."

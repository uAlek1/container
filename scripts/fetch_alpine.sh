#!/usr/bin/env bash
set -euo pipefail

ALPINE_VER="3.19.0"
ARCH="x86_64"
ROOTFS_DIR="${1:-/tmp/alpine}"
TARBALL="alpine-minirootfs-${ALPINE_VER}-${ARCH}.tar.gz"
URL="https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/${ARCH}/${TARBALL}"

echo "==> Preparing Alpine ${ALPINE_VER} rootfs in ${ROOTFS_DIR}"

if [ -x "${ROOTFS_DIR}/bin/sh" ]; then
    echo "==> Rootfs already exists at ${ROOTFS_DIR}/bin/sh, skipping."
    exit 0
fi

mkdir -p "${ROOTFS_DIR}"
cd "${ROOTFS_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "==> Downloading ${URL}"
    if command -v wget >/dev/null 2>&1; then
        wget -q --show-progress "${URL}"
    elif command -v curl >/dev/null 2>&1; then
        curl -L -o "${TARBALL}" "${URL}"
    else
        echo "ERROR: neither wget nor curl found" >&2
        exit 1
    fi
fi

echo "==> Extracting..."
tar -xzf "${TARBALL}"
rm -f "${TARBALL}"

if [ ! -x "${ROOTFS_DIR}/bin/sh" ]; then
    echo "ERROR: ${ROOTFS_DIR}/bin/sh not found or not executable" >&2
    exit 1
fi

echo "==> Done. Rootfs ready at ${ROOTFS_DIR}"
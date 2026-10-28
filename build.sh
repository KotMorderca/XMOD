#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# XMOD's release build: build/xmod.fwsc. Runs on The Firmware Hub's signed build (fm1-build, GitHub
# Ubuntu: JIELI_TOOLCHAIN and AC79_SDK are set there) and in WSL / Linux with the same two.
#
#   ./build.sh [--release VERSION]
set -euo pipefail
cd "$(dirname "$0")"

VERSION=""
if [ "${1:-}" = "--release" ]; then
    VERSION="${2:?--release VERSION}"
fi

tools/get_felucca.sh                 # (Felucca: not in this repository, fetched at its commit)

XMOD_VERSION="$VERSION" python3 tools/build_fw.py
cp build/fw/xmod.fwsc build/xmod.fwsc
echo "build/xmod.fwsc"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Felucca (the hardware layer, the update loader, the installer's web code) into third_party/Felucca,
# at the commit XMOD is built and tested against. It is not in this repository.
set -euo pipefail
cd "$(dirname "$0")/.."
FELUCCA_URL=https://github.com/hugelton/Felucca.git
FELUCCA_COMMIT=440e62ce8a33d9bb285ab6bbca9a0fd50e459648
if [ ! -d third_party/Felucca/.git ]; then
    git init -q third_party/Felucca
    git -C third_party/Felucca remote add origin "$FELUCCA_URL"
    git -C third_party/Felucca fetch -q --depth 1 origin "$FELUCCA_COMMIT"
    git -C third_party/Felucca -c advice.detachedHead=false checkout -q FETCH_HEAD
fi
if [ "$(git -C third_party/Felucca rev-parse HEAD)" != "$FELUCCA_COMMIT" ]; then
    echo "third_party/Felucca is not at $FELUCCA_COMMIT" >&2
    exit 1
fi

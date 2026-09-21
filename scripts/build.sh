#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
"${repo_root}/scripts/test-model.sh"
"${repo_root}/scripts/prepare-firmware.sh"
cd "${repo_root}/.work/ai-passport"
./tools/validate.sh --static
./tools/validate.sh --firmware
printf '\nFirmware: %s\n' "${PWD}/build/FoloToy-AI-Passport-full.bin"

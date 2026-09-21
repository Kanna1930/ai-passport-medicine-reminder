#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp /tmp/medicine-model.XXXXXX)"
trap 'rm -f -- "${out}"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -I"${repo_root}/overlay/main" \
  "${repo_root}/overlay/tests/test_medicine_model.c" \
  "${repo_root}/overlay/main/medicine_model.c" \
  -o "${out}"
"${out}"

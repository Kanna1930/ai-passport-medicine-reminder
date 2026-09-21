#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${repo_root}/.work"
firmware_root="${work_root}/ai-passport"
upstream_repo="https://github.com/FoloToy/ai-passport.git"
upstream_commit="1051209d807fb26f943236b7e02281f13d39bc90"

mkdir -p "${work_root}"
if [[ ! -d "${firmware_root}/.git" ]]; then
    git clone "${upstream_repo}" "${firmware_root}"
fi

git -C "${firmware_root}" fetch origin "${upstream_commit}"
git -C "${firmware_root}" checkout --detach "${upstream_commit}"
git -C "${firmware_root}" switch -C feature/medicine-reminder-v2

cp -a "${repo_root}/overlay/main/." "${firmware_root}/main/"
cp -a "${repo_root}/overlay/tests/." "${firmware_root}/tests/"
cat "${repo_root}/overlay/sdkconfig.defaults.append" >> "${firmware_root}/sdkconfig.defaults"

printf 'Prepared complete firmware checkout at %s\n' "${firmware_root}"
printf 'Baseline commit: %s\n' "${upstream_commit}"

#!/usr/bin/env bash
set -euo pipefail

skill_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd "${skill_dir}/../../.." && pwd)"
build_dir="${TIER_TEST_BUILD_DIR:-${repo_dir}/build}"

required_vars=(
  TIER_TEST_S3_ENDPOINT
  TIER_TEST_S3_ACCESS_KEY
  TIER_TEST_S3_SECRET_KEY
)

for name in "${required_vars[@]}"; do
  if [[ -z "${!name:-}" ]]; then
    echo "Missing required environment variable: ${name}" >&2
    exit 2
  fi
done

if ! curl --silent --show-error --fail --max-time 5 \
  "${TIER_TEST_S3_ENDPOINT%/}/minio/health/live" >/dev/null; then
  echo "MinIO healthcheck failed" >&2
  exit 3
fi

cmake -S "${repo_dir}" -B "${build_dir}" \
  -DENABLE_TESTS=ON \
  -DENABLE_TIER_STORAGE=ON \
  -DENABLE_AWS_SDK=ON
cmake --build "${build_dir}" --target unit_storage integration_tier_storage_minio integration_tier_storage_manager -j2
ctest --test-dir "${build_dir}" -R '^unit\.storage_all$' --output-on-failure
ctest --test-dir "${build_dir}" -R '^integration\.tier-manager\.' --output-on-failure
ctest --test-dir "${build_dir}" -R '^integration\.tier_storage_minio_all$' --output-on-failure

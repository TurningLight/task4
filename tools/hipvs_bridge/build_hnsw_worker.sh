#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
OUTPUT=${1:-$HERE/seekdb-hipvs-hnsw-build}
[[ -x "$HIPCC" ]] || { echo "Missing executable hipcc: $HIPCC (offline build; no download)" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/neighbors/cagra.h" && -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || {
  echo "Missing preinstalled hipVS headers or libcuvs_c.so under $HIPVS_ROOT" >&2; exit 2;
}
mkdir -p "$(dirname "$OUTPUT")"
"$HIPCC" -x hip -std=c++17 -O2 \
  -I"$HIPVS_ROOT/include" -I"$HIPVS_ROOT/include/cuvs" \
  "$HERE/hnsw_build_worker.cpp" -L"$HIPVS_ROOT/lib" -lcuvs_c \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" -o "$OUTPUT"
echo "Built $OUTPUT"

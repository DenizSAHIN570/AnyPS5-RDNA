#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build="${NATIVE_RDNA_BUILD:-/tmp/native-rdna}"
flags="-Wall -Wextra -Wno-missing-field-initializers"

configure_and_test() {
    local name="$1"
    cmake -S "$root/tools/native-rdna/$name" -B "$build/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$flags" > /dev/null
    cmake --build "$build/$name"
    if [ -f "$build/$name/CTestTestfile.cmake" ]; then
        ctest --test-dir "$build/$name" --output-on-failure
    fi
}

echo "== shader census"
configure_and_test census
echo "== vram_bench"
configure_and_test vram-bench
"$build/vram-bench/vram_bench" access --unplaced --size-mib 16
echo "== drm_dispatch"
configure_and_test drm-dispatch

echo "== python tools"
python3 "$root/tools/native-rdna/tests/test_host_check.py"
python3 "$root/tools/native-rdna/tests/test_census_report.py"
python3 "$root/tools/native-rdna/tests/test_baseline.py"
python3 "$root/tools/hw-oracle/test_hw_oracle.py"
echo "all native-rdna checks passed"

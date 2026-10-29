# Build native HNSW snapshots with hipVS

This implementation accelerates the snapshot construction used by `CREATE VECTOR INDEX` for dense float32 L2 HNSW indexes declared with `lib=cuvs`. It buffers the snapshot input, builds the expensive base graph with hipVS on an AMD GPU, creates a small CPU routing hierarchy, and imports the completed graph into the existing VSAG index. Normal SQL queries, filtering, ID lookup and persistence continue to use VSAG's native representation.

The CPU `lib=vsag` path is preserved. This is a build optimization, not a batch-query benchmark. Dataset preparation/import and environment startup are measured separately from index construction.

## Algorithm and integration

The reference `lib=cuvs` implementation constructs the CPU HNSW and may additionally build a GPU CAGRA graph during queries. This implementation enables a separate bulk path for supported snapshots when the build worker is installed next to the running seekdb executable, or when an explicit `SEEKDB_HIPVS_BUILD_WORKER` override is supplied. It never searches the current directory or PATH. An explicitly empty override disables bulk construction.

1. `ObPluginVectorIndexAdaptor::add_snap_index` selects bulk staging for the snapshot. Incremental/delta ingestion retains the reference implementation.
2. `HipvsBulkBuilder` stores every input vector and its original 64-bit ID. Buffer capacity grows geometrically, including when the storage layer sends individual rows. Duplicate IDs, nonfinite values and mismatched dimensions are rejected.
3. The build worker receives a private regular file through inherited standard input. No shell command is evaluated. hipVS CAGRA uses GPU NN-descent (20 iterations, intermediate degree `4*M`) below 262,144 rows, and IVF-PQ candidate generation (intermediate degree `8*M`, refinement rate 4) at larger cardinalities. Both produce a graph of degree `2*M` and synchronize before returning it to the CPU. IVF-PQ is an internal candidate-generation technique; the saved index retains every original float32 vector, and queries use the same L2 metric and precision. The requested strategy is logged. These settings were tuned on the development datasets; they are not an official evaluation configuration.
4. A graph traversal identifies strongly connected components. Every component gets a routing entry, supplemented by a deterministic sample using seed 42. A small CPU HNSW connects these entries. Its routing edges are also added to selected base vertices within the original `2*M` base-degree budget; the original GPU neighbors are retained within that budget. This avoids losing whole clusters that are disconnected in a pure kNN graph.
5. The worker verifies that the actual entry point can reach every vector. It serializes a full HNSW hierarchy, retaining float32 vectors. The parent validates dimensions, degree bounds, edge ranges, record lengths, vector bytes and the row-label permutation, then replaces row offsets with original IDs.
6. VSAG deserializes the graph without repeating a full CPU HNSW construction. The input-completion hook finalizes it **before** the snapshot key is published. Query entry points also enforce finalization rather than querying a half-built index. When memory-saving renewal replaces a shared index with an empty one, matching cached prefixes no longer suppress the required reload. This preserves correct first-query behavior under the background snapshot scheduler.

The worker is a separate process because hipVS's hnswlib and VSAG contain overlapping C++ implementation symbols. C++ objects never cross this boundary. The output is a checked native graph. The worker has a ten-minute timeout; its failure is reported and is not counted as a successful GPU build.

The implementation currently targets Linux x86_64 with the hipVS/hnswlib layout in the competition image. Bulk staging is limited to dense L2 HNSW without extra-info columns and `4 <= M <= 128`. Unsupported configurations retain the reference path and log a fallback. Snapshots below 256 vectors, or no larger than `4*M`, explicitly use the CPU path. Such rounds must not appear in GPU performance claims.

## Tested environment and source base

- AMD Radeon Cloud competition template 3102 (`seekdb-hipvs`), SeekDB 1.4.0.0.
- Reference source commit: `de17949d57b9cd2240f141f8116d29bcd495d8d9`, from the public hipVS integration in [PR 1314](https://github.com/oceanbase/seekdb/pull/1314).
- GPU architecture: `gfx1100`; ROCm 7.2.4. The image's hipVS build contains gfx1100 code objects and is not a generic wheel for arbitrary GPUs.
- The image supplies `/opt/hipvs`, `/seekdb_workspace/bridge/libseekdb_cuvs_bridge.so`, `/seekdb_workspace/datasets` and a seekdb development-dependency cache under `/opt/seekdb-deps-cache`.
- Python validation used Python 3.12, NumPy 2.4.6 and PyMySQL 1.1.2. The supplied synthetic dataset manifest records its generation version separately.

The reference image and source have their own licenses. The build worker links the image's hipVS C API and uses its provided hnswlib headers; those dependencies retain their upstream licenses. No model, embedding API, API key or model download is required.

The reference hipVS integration is an unmerged prerequisite. This change is based on the exact image revision above; the new implementation files and the experiment report identify the additional work separately. It must not be described as implementing the reference bridge or reproducing the Notebook's batch-query speedup.

## Build

Check out the submitted source revision. In the competition image, the ordinary command is sufficient:

```bash
./build.sh release --init --make -j8
```

`build.sh` reuses `/opt/seekdb-deps-cache/<manifest-md5>/3rd` only when the dependency manifest matches and its completion markers exist; an existing `deps/3rd` is not replaced. CMake detects preinstalled hipcc, hipVS headers/libraries and `/seekdb_workspace/bridge/libseekdb_cuvs_bridge.so`, enables `OB_BUILD_CUVS`, and builds the worker as a dependency of `seekdb`. The worker appears at `build_release/src/observer/seekdb-hipvs-hnsw-build` and is included in the server installation's `bin/` directory.

The Rust workspace now commits `Cargo.lock`; CMake always uses `--locked`. On the detected competition image it also uses `--offline` and `CARGO_NET_OFFLINE=true`. The lock was resolved against the image's existing registry cache with Rust 1.97.1. Missing cached crates cause an explicit failure rather than an automatic download. Other environments still need the documented C++/Rust dependencies; this does not make an empty machine self-contained.

For a custom preinstalled layout, set `SEEKDB_HIPVS_ROOT`, `SEEKDB_ROCM_ROOT`, `SEEKDB_HIPCC`, and `CUVS_BRIDGE_LIB` as CMake options. Explicit `-DOB_BUILD_CUVS=OFF` retains the CPU build, even in the image. Explicit GPU enablement with a missing bridge fails during configuration. `-DSEEKDB_OFFLINE_BUILD=ON` enables the locked offline Rust build outside the image as well.

When copying the server manually, copy its worker alongside it. Copying only the seekdb executable does not copy the separate GPU worker. Normal CMake server installation includes both. The evaluator's build/copy/run contract is not public; these default-build checks do not by themselves prove compatibility with an undisclosed evaluator command.

The worker build supports `HIPVS_ROOT` and `ROCM_ROOT`; defaults are `/opt/hipvs` and `/opt/rocm`. No system library is replaced. A CPU-only seekdb build leaves `OB_BUILD_CUVS` disabled and does not need the worker.

## Start an isolated candidate

Use a new data directory and an unused port. Do not overwrite an existing instance or run the demonstration Notebook's startup cell against an instance managed by these tests.

```bash
mkdir -p run/hipvs-candidate
export OMP_NUM_THREADS=8
export TZ=UTC

build_release/src/observer/seekdb \
  --base-dir="$PWD/run/hipvs-candidate" --port=2883 --nodaemon \
  --parameter _memory_budget=16G --parameter memory_limit=16G \
  --parameter cpu_count=8 --parameter datafile_size=8G \
  --parameter datafile_maxsize=24G --parameter log_disk_size=4G \
  --log-level INFO
```

The example uses neither `SEEKDB_HIPVS_BUILD_WORKER` nor `LD_LIBRARY_PATH`: the
adjacent worker is found automatically and the tested image already configures
its runtime libraries. Start from a shell without an old worker override. For a
separately built worker, an explicit absolute-path override remains supported.

Use the original image binary for the CPU baseline in a separate new directory, with the same resource parameters and a different port (the examples use 2881). Do not set the worker variable on the baseline process. Run CPU and GPU measurements sequentially on the same machine. Stop unrelated compilers, benchmarks and GPU jobs before measurement.

The example validation clients connect inside the instance. If a database password is configured, supply it with `SEEKDB_PASSWORD`; no credential belongs in a command argument, DSL, report or repository.

In containers whose PID 1 does not reap children, a stopped database can remain as a zombie. A stale `run/seekdb.pid` may then prevent restart. Verify that no live process is using the exact test data directory before removing only its stale PID and socket files; do not remove the data directory. This is also handled by the image's supplied startup script.

## SQL behavior

```sql
CREATE TABLE vectors (id BIGINT PRIMARY KEY, v VECTOR(128));
-- Import all original vectors before starting the build timer.

CREATE VECTOR INDEX ann ON vectors(v)
WITH (distance=l2,type=hnsw,lib=cuvs,m=32,ef_construction=200,ef_search=1000);

-- Use the actual query vector in place of the example variable.
SELECT id, l2_distance(v, @query)
FROM vectors
ORDER BY l2_distance(v, @query) APPROXIMATE LIMIT 10;
```

For the CPU baseline, use `lib=vsag` with the same dataset and parameters. No special batch-query procedure is needed. A GPU-built snapshot is serialized in native format; loading it does not require rerunning GPU construction. Changed/deleted rows continue to use seekdb's existing bitmap, delta and filtering machinery.

## Validation and measurement

The self-contained tools are under `unittest/oblib/vector/hipvs_validation`:

```bash
cd unittest/oblib/vector/hipvs_validation
/opt/venv/bin/python -m unittest -v test_offline
/opt/venv/bin/python preflight.py --home /seekdb_workspace --out runs/environment.json
/opt/venv/bin/python make_dataset.py --out datasets/dev100k --n 100000 --dim 128 --queries 100 --k 10
```

The image provides NumPy 2.4.6. SQL clients use the committed, SHA-256-checked
PyMySQL 1.1.2 pure-Python wheel under `vendor/`; no pip installation or download
is performed. Its license and source notice are included. This is a test-client
dependency, not a seekdb server dependency. `requirements.txt` remains available
for provisioning a different development environment in advance.

The synthetic data is for reproducible development measurements, not a substitute for an organizer's dataset. Its ground truth is exhaustive float64 L2 over the exact float32 inputs, with unique noncontiguous 64-bit document IDs. Dataset hashes, random seed, dimensions and numeric precision are saved. When official data/targets become available, run the same procedure with those inputs and rules.

```bash
python compare_sql.py \
  --dataset datasets/dev100k --out runs/paired-100k \
  --cpu-port 2881 --gpu-port 2883 \
  --cpu-log /absolute/cpu-data/log/seekdb.log \
  --gpu-log /absolute/gpu-data/log/seekdb.log \
  --cpu-binary /seekdb_workspace/bin/seekdb \
  --gpu-binary /absolute/checkout/build_release/src/observer/seekdb \
  --worker /absolute/checkout/build_release/src/observer/seekdb-hipvs-hnsw-build \
  --rounds 3 --m 32 --ef-search 1000 --min-recall 0.95
```

The script creates isolated random test databases and fresh tables for every round. It imports and checks all vectors before construction, then times `CREATE VECTOR INDEX` through the first real query. That first query must return a valid complete Top-K. The GPU worker and native-import logs must each confirm the entire row count, with no fallback, and the native-ready timestamp must be inside the timed interval. Subsequent validation queries must return valid IDs and meet the configured quality floor. The script rejects incomplete, duplicate, unknown or nonfinite results rather than changing the denominator.

CPU/GPU order alternates between repetitions; all raw round files are retained. The reported acceleration is the ratio of CPU/GPU **median end-to-end construction times**, not the ratio of kernel times. Import, exact-ground-truth generation and later validation-query time remain separate. The default 0.95 recall floor is an explicitly chosen local check, not a claimed official threshold.

For a larger development dataset, change `--n` and use new output directories. Keep memory limits, data/metric/precision and index parameters comparable. Do not reuse an existing graph, silently reduce input size or count a CPU fallback as GPU acceleration.

## CPU and GPU regression tests

From the checkout root, build `oblib_tests` using the CMake path in the dependency toolchain:

```bash
deps/3rd/usr/local/oceanbase/devtools/bin/cmake \
  --build build_release --target oblib_tests -j8
```

Run the existing vector adapter tests and new staging checks:

```bash
build_release/unittest/oblib/oblib_tests \
  --gtest_filter='TestVsagAdaptor.*:HipvsBulkBuilder.*'
```

The hardware test is disabled by default so normal CPU CI does not require an AMD GPU. Enable it explicitly:

```bash
SEEKDB_HIPVS_TEST_WORKER="$PWD/build_release/src/observer/seekdb-hipvs-hnsw-build" \
SEEKDB_HIPVS_TEST_DATASET=/seekdb_workspace/datasets \
build_release/unittest/oblib/oblib_tests \
  --gtest_also_run_disabled_tests \
  --gtest_filter='HipvsBulkImport.DISABLED_gpu_native_roundtrip'
```

It checks original-ID mapping, immutable optimization, iterative search, filtered search, native serialize/reload, subsequent inserts, invalid input and failed workers. On the tested image the unit-test binary's toolchain RUNPATH selects an older libstdc++; use the image's compatible system `libstdc++.so.6` for **that test process** if needed, while retaining the ROCm library paths. The production database must use compatible ROCm libraries; do not prepend an unrelated GPU runtime to its loader path.

The provided wrapper applies the tested runtime settings without modifying system libraries:

```bash
bash tools/hipvs_bridge/run_vector_tests.sh
bash tools/hipvs_bridge/run_vector_tests.sh --gpu \
  "$PWD/build_release/src/observer/seekdb-hipvs-hnsw-build" /seekdb_workspace/datasets
```

The SQL persistence check creates a separate fixture and exercises negative/zero/64-bit primary keys, filtering, inserts, updates, deletes and the small-snapshot fallback:

```bash
python sql_regressions.py --phase prepare --state runs/restart-state.json --port 2883
python sql_regressions.py --phase prepare --library vsag --state runs/cpu-state.json --port 2883
# Gracefully stop and restart only this test instance with the same data directory.
# To verify the saved native format, set SEEKDB_HIPVS_BUILD_WORKER to the empty
# string (disables automatic discovery) and HIP_VISIBLE_DEVICES=-1 for the
# restarted process, then run:
python sql_regressions.py --phase verify --state runs/restart-state.json --port 2883
```

Restore normal GPU visibility on the process used for subsequent GPU build benchmarks. Do not run profiling or regression workloads concurrently with timed CPU/GPU comparisons.

The additional Linux build-integration checks can be run without a GPU:

```bash
python -m unittest -v test_build_integration
```

These check CPU defaults, explicit disablement, missing-dependency errors,
worker target dependencies/installation and real C++ executable-relative
discovery. The build-wiring fixture uses a fake hipcc and is explicitly not GPU
execution evidence. Real SQL and profiler records provide that evidence.

## GPU execution evidence and backups

Generate a regular-file worker input and profile the same worker binary used by the SQL runs:

```bash
python make_worker_input.py --dataset datasets/dev100k --out runs/profile-input.bin --m 32
OMP_NUM_THREADS=8 rocprofv3 --kernel-trace --memory-copy-trace \
  --output-format csv --output-directory runs/rocprof \
  -- /absolute/checkout/build_release/src/observer/seekdb-hipvs-hnsw-build \
  < runs/profile-input.bin > runs/profile-index.bin 2> runs/rocprof.log
```

Profiling overhead is not used in the speed comparison. Preserve kernel dispatches, memory-copy traces, agent information, worker logs and binary hashes alongside the SQL construction records.

```bash
python pack_results.py runs/paired-100k runs/paired-100k.tar.gz
```

Copy the archive and its `.sha256.json` to local storage and verify the checksum before the cloud instance is released. A cloud-side archive alone is not a backup. The contest's credited instance is ephemeral; no required source, data or result should exist only there.

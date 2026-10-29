# Build as a dependency of seekdb, including `make seekdb` (not only ALL).
# Keep hipVS's hnswlib in its own executable to avoid VSAG symbol collisions.
if(OB_BUILD_HIPVS_WORKER)
  set(SEEKDB_HIPVS_WORKER_OUTPUT
      "${CMAKE_BINARY_DIR}/src/observer/seekdb-hipvs-hnsw-build")
  add_custom_command(
    OUTPUT "${SEEKDB_HIPVS_WORKER_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "HIPVS_ROOT=${SEEKDB_HIPVS_ROOT}"
      "ROCM_ROOT=${SEEKDB_ROCM_ROOT}"
      "HIPCC=${SEEKDB_HIPCC}"
      bash "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/build_hnsw_worker.sh"
      "${SEEKDB_HIPVS_WORKER_OUTPUT}"
    DEPENDS
      "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/build_hnsw_worker.sh"
      "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/hnsw_build_worker.cpp"
      "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge/hnsw_hierarchy.h"
    COMMENT "Building native HNSW hipVS worker from preinstalled libraries"
    VERBATIM)
  add_custom_target(seekdb_hipvs_worker DEPENDS "${SEEKDB_HIPVS_WORKER_OUTPUT}")
  add_dependencies(seekdb seekdb_hipvs_worker)
  install(PROGRAMS "${SEEKDB_HIPVS_WORKER_OUTPUT}" DESTINATION bin COMPONENT server)
endif()

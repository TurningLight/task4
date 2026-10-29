# Detect only preinstalled dependencies. Configuration never downloads a GPU
# toolkit or a library, and an explicit OB_BUILD_CUVS=OFF always wins.
if(NOT DEFINED SEEKDB_HIPVS_ROOT)
  if(DEFINED ENV{HIPVS_ROOT} AND NOT "$ENV{HIPVS_ROOT}" STREQUAL "")
    set(SEEKDB_HIPVS_ROOT "$ENV{HIPVS_ROOT}")
  else()
    set(SEEKDB_HIPVS_ROOT "/opt/hipvs")
  endif()
endif()
if(NOT DEFINED SEEKDB_ROCM_ROOT)
  if(DEFINED ENV{ROCM_ROOT} AND NOT "$ENV{ROCM_ROOT}" STREQUAL "")
    set(SEEKDB_ROCM_ROOT "$ENV{ROCM_ROOT}")
  else()
    set(SEEKDB_ROCM_ROOT "/opt/rocm")
  endif()
endif()
ob_define(SEEKDB_HIPCC "${SEEKDB_ROCM_ROOT}/bin/hipcc")
if(NOT DEFINED CUVS_BRIDGE_LIB OR "${CUVS_BRIDGE_LIB}" STREQUAL "")
  set(CUVS_BRIDGE_LIB "/seekdb_workspace/bridge/libseekdb_cuvs_bridge.so")
endif()

set(SEEKDB_HIPVS_DEPS_PRESENT OFF)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT OB_ANDROID AND
   EXISTS "${SEEKDB_HIPCC}" AND
   EXISTS "${SEEKDB_HIPVS_ROOT}/include/cuvs/core/c_api.h" AND
   EXISTS "${SEEKDB_HIPVS_ROOT}/include/cuvs/neighbors/cagra.h" AND
   EXISTS "${SEEKDB_HIPVS_ROOT}/include/hnswlib/hnswlib.h" AND
   EXISTS "${SEEKDB_HIPVS_ROOT}/lib/libcuvs_c.so" AND
   EXISTS "${CUVS_BRIDGE_LIB}")
  set(SEEKDB_HIPVS_DEPS_PRESENT ON)
endif()
ob_define(OB_BUILD_CUVS ${SEEKDB_HIPVS_DEPS_PRESENT})
set(_hipvs_worker_default OFF)
if(OB_BUILD_CUVS AND SEEKDB_HIPVS_DEPS_PRESENT)
  set(_hipvs_worker_default ON)
endif()
ob_define(OB_BUILD_HIPVS_WORKER ${_hipvs_worker_default})
ob_define(SEEKDB_OFFLINE_BUILD ${_hipvs_worker_default})
if(OB_BUILD_CUVS AND NOT EXISTS "${CUVS_BRIDGE_LIB}")
  message(FATAL_ERROR "OB_BUILD_CUVS requires an existing CUVS_BRIDGE_LIB; no library will be downloaded")
endif()
if(OB_BUILD_HIPVS_WORKER AND (NOT OB_BUILD_CUVS OR NOT SEEKDB_HIPVS_DEPS_PRESENT))
  message(FATAL_ERROR "OB_BUILD_HIPVS_WORKER requires OB_BUILD_CUVS and preinstalled hipcc, hipVS headers/libraries and bridge")
endif()
message(STATUS "[hipVS] preinstalled=${SEEKDB_HIPVS_DEPS_PRESENT}; enabled=${OB_BUILD_CUVS}; worker=${OB_BUILD_HIPVS_WORKER}; offline=${SEEKDB_OFFLINE_BUILD}")

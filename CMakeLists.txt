cmake_minimum_required(VERSION 3.5...3.29)

# Extension name
set(TARGET_NAME lake_formation)
project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# The lake_formation extension compiles against the duckdb-iceberg public headers (for example
# catalog/rest/iceberg_access_delegation.hpp) but does NOT link the iceberg binary: iceberg is a
# separate loadable extension, loaded at runtime (see extension_config.cmake, DONT_LINK).
#
# ICEBERG_SRC_DIR must point at a duckdb-iceberg checkout pinned to the commit that contains the
# access-delegation hooks. For local development a sibling checkout is assumed; override with
#   cmake ... -DICEBERG_SRC_DIR=/path/to/duckdb-iceberg
set(ICEBERG_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../duckdb-iceberg" CACHE PATH
    "Path to a duckdb-iceberg checkout providing iceberg public headers")
if(NOT EXISTS "${ICEBERG_SRC_DIR}/src/include/catalog/rest/iceberg_access_delegation.hpp")
  message(FATAL_ERROR
      "ICEBERG_SRC_DIR='${ICEBERG_SRC_DIR}' does not contain the iceberg access-delegation hook header. "
      "Point -DICEBERG_SRC_DIR at a duckdb-iceberg checkout that includes the access-delegation hooks.")
endif()

include_directories(src/include)
include_directories(${ICEBERG_SRC_DIR}/src/include)

set(EXTENSION_SOURCES
    src/lake_formation_extension.cpp
    src/lakeformation/lake_formation_provider.cpp
    src/lakeformation/lf_client.cpp
    src/lakeformation/lf_credentials.cpp
    src/lakeformation/lf_credential_cache.cpp
    src/lakeformation/lf_filter_parser.cpp
    src/lakeformation/lf_table_metadata.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  # AWS SDK (Glue + Lake Formation + STS) lives entirely in this extension, never in duckdb-iceberg.
  find_package(AWSSDK REQUIRED COMPONENTS core sso sts identity-management glue lakeformation)

  target_include_directories(${EXTENSION_NAME} PUBLIC $<BUILD_INTERFACE:${AWSSDK_INCLUDE_DIRS}>)
  target_link_libraries(${EXTENSION_NAME} PUBLIC ${AWSSDK_LINK_LIBRARIES})
  target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE $<BUILD_INTERFACE:${AWSSDK_INCLUDE_DIRS}>)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ${AWSSDK_LINK_LIBRARIES})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

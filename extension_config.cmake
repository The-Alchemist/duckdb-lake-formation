# This file is included by DuckDB's build system. It specifies which extensions to build.

# The extension from this repo.
duckdb_extension_load(lake_formation
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Runtime dependency: duckdb-iceberg. We compile against its headers but do NOT link it (DONT_LINK):
# iceberg is loaded as its own extension at runtime, and the two share state through DuckDB core only.
#
# TODO: pin GIT_TAG to the duckdb-iceberg commit that merges the access-delegation hooks. Until then,
# point ICEBERG_SRC_DIR (see CMakeLists.txt) at a local checkout containing the hooks.
duckdb_extension_load(iceberg
    GIT_URL https://github.com/duckdb/duckdb-iceberg
    GIT_TAG REPLACE_WITH_PINNED_HOOK_COMMIT
    DONT_LINK
)

# Object storage + base AWS credential chain used to call Glue / Lake Formation.
if (NOT EMSCRIPTEN)
  duckdb_extension_load(httpfs)
  if (NOT MINGW)
    duckdb_extension_load(aws
        GIT_URL https://github.com/duckdb/duckdb-aws
        GIT_TAG f15081e8708b78715a11391f33aea0c28b8c8d1a
    )
  endif()
endif()

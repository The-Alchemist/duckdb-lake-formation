PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=lake_formation
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Extensions that we need for testing
CORE_EXTENSIONS='httpfs;parquet;aws'

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

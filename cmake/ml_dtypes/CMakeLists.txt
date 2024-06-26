project(ml_dtypes CXX)

add_library(ml_dtypes INTERFACE)

target_include_directories(ml_dtypes INTERFACE
  "${ML_DTYPES_SOURCE_DIR}"
  "${ML_DTYPES_SOURCE_DIR}/ml_dtypes"
)

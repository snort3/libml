project(gemmlowp CXX)

option(GEMMLOWP_ADD_HEADERS_TO_TARGETS OFF)

file(GLOB GEMMLOWP_EIGHTBITINT_HEADERS
    "${GEMMLOWP_SOURCE_DIR}/eight_bit_int_gemm/*.h"
)

file(GLOB GEMMLOWP_EIGHTBITINT_SOURCES
    "${GEMMLOWP_SOURCE_DIR}/eight_bit_int_gemm/*.cc"
)
file(GLOB GEMMLOWP_FIXEDPOINT_HEADERS "${GEMMLOWP_SOURCE_DIR}/fixedpoint/*.h")
file(GLOB GEMMLOWP_INTERNAL_HEADERS "${GEMMLOWP_SOURCE_DIR}/internal/*.h")
file(GLOB GEMMLOWP_META_HEADERS "${GEMMLOWP_SOURCE_DIR}/meta/*.h")
file(GLOB GEMMLOWP_PROFILING_HEADERS "${GEMMLOWP_SOURCE_DIR}/profiling/*.h")
file(GLOB GEMMLOWP_PUBLIC_HEADERS "${GEMMLOWP_SOURCE_DIR}/public/*.h")

set(GEMMLOWP_PRIVATE_HEADERS "")
list(APPEND GEMMLOWP_PRIVATE_HEADERS ${GEMMLOWP_FIXEDPOINT_HEADERS})
list(APPEND GEMMLOWP_PRIVATE_HEADERS ${GEMMLOWP_INTERNAL_HEADERS})

add_library(gemmlowp_private INTERFACE)

if(GEMMLOWP_ADD_HEADERS_TO_TARGETS)
    target_sources(gemmlowp_private INTERFACE ${GEMMLOWP_PRIVATE_HEADERS})
endif()

target_include_directories(gemmlowp_private INTERFACE
    "$<BUILD_INTERFACE:${GEMMLOWP_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gemmlowp>"
)

add_library(gemmlowp INTERFACE)

if(GEMMLOWP_ADD_HEADERS_TO_TARGETS)
    target_sources(gemmlowp INTERFACE ${GEMMLOWP_PUBLIC_HEADERS})
endif()

target_include_directories(gemmlowp INTERFACE
    "$<BUILD_INTERFACE:${GEMMLOWP_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gemmlowp>"
)

target_link_libraries(gemmlowp INTERFACE gemmlowp_private)

add_library(gemmlowp_eight_bit_int_gemm
    ${GEMMLOWP_EIGHTBITINT_SOURCES}
    ${GEMMLOWP_EIGHTBITINT_HEADERS}
)

target_include_directories(gemmlowp_eight_bit_int_gemm PUBLIC
    "${GEMMLOWP_SOURCE_DIR}/eight_bit_int_gemm"
)

add_library(gemmlowp_fixedpoint INTERFACE)

if(GEMMLOWP_ADD_HEADERS_TO_TARGETS)
    target_sources(gemmlowp_fixedpoint INTERFACE ${GEMMLOWP_FIXEDPOINT_HEADERS})
endif()

target_include_directories(gemmlowp_fixedpoint INTERFACE
    "${GEMMLOWP_SOURCE_DIR}/fixedpoint"
)

target_link_libraries(gemmlowp_fixedpoint INTERFACE gemmlowp_private)

add_library(gemmlowp_profiler INTERFACE)

if(GEMMLOWP_ADD_HEADERS_TO_TARGETS)
    target_sources(gemmlowp_profiler INTERFACE ${GEMMLOWP_PROFILING_HEADERS})
endif()

target_include_directories(gemmlowp_profiler INTERFACE
    "${GEMMLOWP_SOURCE_DIR}/profiling"
)

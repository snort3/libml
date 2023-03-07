set(VENDOR_DIR "${PROJECT_SOURCE_DIR}/vendor" CACHE PATH "")
set(FETCHCONTENT_QUIET OFF CACHE BOOL "")

if(NOT MSVC)
    add_compile_options(
        -Wno-deprecated-declarations
        -Wno-maybe-uninitialized
        -Wno-unknown-pragmas
        -Wno-comment
        -Wno-ignored-attributes
    )
endif()

##########
# abseil #
##########

set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "")
add_subdirectory("${VENDOR_DIR}/abseil" EXCLUDE_FROM_ALL)

########
# clog #
########

set(CLOG_BUILD_TESTS OFF CACHE BOOL "")
set(CLOG_SOURCE_DIR "${VENDOR_DIR}/cpuinfo/deps/clog" CACHE PATH "")
add_subdirectory("${CLOG_SOURCE_DIR}" EXCLUDE_FROM_ALL)

###########
# cpuinfo #
###########

set(CPUINFO_BUILD_UNIT_TESTS OFF CACHE BOOL "")
set(CPUINFO_BUILD_MOCK_TESTS OFF CACHE BOOL "")
set(CPUINFO_BUILD_BENCHMARKS OFF CACHE BOOL "")
set(CPUINFO_SOURCE_DIR "${VENDOR_DIR}/cpuinfo" CACHE PATH "")
add_subdirectory("${CPUINFO_SOURCE_DIR}" EXCLUDE_FROM_ALL)

#########
# eigen #
#########

set(EIGEN_TEST_NOQT ON CACHE BOOL "")
set(EIGEN_BUILD_DOC OFF CACHE BOOL "")
add_subdirectory("${VENDOR_DIR}/eigen" EXCLUDE_FROM_ALL)

############
# farmhash #
############

set(FARMHASH_SOURCE_DIR "${VENDOR_DIR}/farmhash" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/farmhash" EXCLUDE_FROM_ALL)

#########
# fft2d #
#########

set(FFT2D_SOURCE_DIR "${VENDOR_DIR}/fft2d" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/fft2d" EXCLUDE_FROM_ALL)

###############
# flatbuffers #
###############

set(FLATBUFFERS_SOURCE_DIR "${VENDOR_DIR}/flatbuffers" CACHE PATH "")
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "")

add_definitions(-DNOMINMAX=1)
add_subdirectory("${FLATBUFFERS_SOURCE_DIR}" EXCLUDE_FROM_ALL)
remove_definitions(-DNOMINMAX)

get_target_property(FLATBUFFERS_INCLUDE_DIRS flatbuffers INCLUDE_DIRECTORIES)
add_library(flatbuffers::flatbuffers ALIAS flatbuffers)
set(FLATBUFFERS_LIBRARIES flatbuffers)
set(FLATBUFFERS_PROJECT_DIR "${FLATBUFFERS_SOURCE_DIR}" CACHE STRING "")

############
# gemmlowp #
############

set(GEMMLOWP_SOURCE_DIR "${VENDOR_DIR}/gemmlowp" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/gemmlowp" EXCLUDE_FROM_ALL)

#######
# ruy #
#######

add_subdirectory("${VENDOR_DIR}/ruy" EXCLUDE_FROM_ALL)

#########
# psimd #
#########

set(PSIMD_SOURCE_DIR "${VENDOR_DIR}/psimd" CACHE PATH "")
add_subdirectory("${PSIMD_SOURCE_DIR}" EXCLUDE_FROM_ALL)

########
# FP16 #
########

set(FP16_BUILD_TESTS OFF CACHE BOOL "")
set(FP16_BUILD_BENCHMARKS OFF CACHE BOOL "")
set(FP16_SOURCE_DIR "${VENDOR_DIR}/FP16" CACHE PATH "")
add_subdirectory("${FP16_SOURCE_DIR}" EXCLUDE_FROM_ALL)

#########
# FXdiv #
#########

set(FXDIV_BUILD_TESTS OFF CACHE BOOL "")
set(FXDIV_BUILD_BENCHMARKS OFF CACHE BOOL "")
set(FXDIV_SOURCE_DIR "${VENDOR_DIR}/FXdiv" CACHE PATH "")
add_subdirectory("${FXDIV_SOURCE_DIR}" EXCLUDE_FROM_ALL)

###############
# pthreadpool #
###############

set(PTHREADPOOL_BUILD_TESTS OFF CACHE BOOL "")
set(PTHREADPOOL_BUILD_BENCHMARKS OFF CACHE BOOL "")
set(PTHREADPOOL_SOURCE_DIR "${VENDOR_DIR}/pthreadpool" CACHE PATH "")
add_subdirectory("${PTHREADPOOL_SOURCE_DIR}" EXCLUDE_FROM_ALL)

############
# neon2sse #
############

add_subdirectory("${VENDOR_DIR}/neon2sse" EXCLUDE_FROM_ALL)
set(neon2sse_POPULATED ON CACHE BOOL "")
get_target_property(NEON2SSE_INCLUDE_DIRS NEON_2_SSE INTERFACE_DIRECTORIES)
add_library(NEON_2_SSE::NEON_2_SSE ALIAS NEON_2_SSE)
set(NEON2SSE_LIBRARIES NEON_2_SSE)

###########
# XNNPACK #
###########

set(XNNPACK_BUILD_TESTS OFF CACHE BOOL "")
set(XNNPACK_BUILD_BENCHMARKS OFF CACHE BOOL "")
add_subdirectory("${VENDOR_DIR}/XNNPACK" EXCLUDE_FROM_ALL)
set(xnnpack_POPULATED ON CACHE BOOL "")

include_directories(
  AFTER
   "${PTHREADPOOL_SOURCE_DIR}/include"
   "${FP16_SOURCE_DIR}/include"
   "${XNNPACK_SOURCE_DIR}/include"
   "${CPUINFO_SOURCE_DIR}/"
)

###################
# tensorflow-lite #
###################

add_subdirectory(
    "${VENDOR_DIR}/tensorflow/tensorflow/lite"
    EXCLUDE_FROM_ALL
)

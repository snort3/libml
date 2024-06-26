set(VENDOR_DIR "${PROJECT_SOURCE_DIR}/vendor" CACHE PATH "")
set(FETCHCONTENT_QUIET OFF CACHE BOOL "")
set(CMAKE_EXPORT_PACKAGE_REGISTRY OFF)

if(NOT MSVC)
    add_compile_options(
        -Wno-comment
        -Wno-conversion
        -Wno-deprecated-declarations
        -Wno-ignored-attributes
        -Wno-uninitialized
        -Wno-unknown-pragmas
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        add_compile_options(-Wno-maybe-uninitialized -Wno-stringop-overflow -Wno-return-type)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-Wno-unknown-attributes -Wno-deprecated-builtins)
    endif()
endif()

if(APPLE)
    set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
endif()

##########
# abseil #
##########

set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "")
add_subdirectory("${VENDOR_DIR}/abseil" EXCLUDE_FROM_ALL)

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

if(APPLE)
    set(FLATBUFFERS_OSX_BUILD_UNIVERSAL OFF CACHE BOOL "")
endif()

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

set(BUILD_TESTING_TMP ${BUILD_TESTING})
set(BUILD_TESTING OFF)

set(gemmlowp_SOURCE_DIR "${VENDOR_DIR}/gemmlowp/contrib" CACHE PATH "")
add_subdirectory("${gemmlowp_SOURCE_DIR}" EXCLUDE_FROM_ALL)
set(gemmlowp_POPULATED ON CACHE BOOL "")

get_target_property(GEMMLOWP_INCLUDE_DIRS gemmlowp INTERFACE_DIRECTORIES)
add_library(gemmlowp::gemmlowp ALIAS gemmlowp)
set(GEMMLOWP_LIBRARIES gemmlowp)

set(BUILD_TESTING ${BUILD_TESTING_TMP})

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

#############
# ml_dtypes #
#############

set(ML_DTYPES_SOURCE_DIR "${VENDOR_DIR}/ml_dtypes" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/ml_dtypes" EXCLUDE_FROM_ALL)

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

if(ENABLE_TESTS)
    enable_testing()
    find_package(CppUTest REQUIRED)
endif()

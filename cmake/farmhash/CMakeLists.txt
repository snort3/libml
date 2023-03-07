project(farmhash CXX)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "int main(int argc, char* argv[]) { return (int)__builtin_expect(0, 0); }"
    FARMHASH_HAS_BUILTIN_EXPECT
)

add_library(farmhash
    "${FARMHASH_SOURCE_DIR}/src/farmhash.cc"
    "${FARMHASH_SOURCE_DIR}/src/farmhash.h"
)

set_target_properties(farmhash PROPERTIES
    PUBLIC_HEADER "${FARMHASH_SOURCE_DIR}/src/farmhash.h"
)

target_include_directories(farmhash PUBLIC
    "$<BUILD_INTERFACE:${FARMHASH_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

if(NOT FARMHASH_HAS_BUILTIN_EXPECT)
    target_compile_definitions(farmhash PUBLIC
        -DFARMHASH_NO_BUILTIN_EXPECT
    )
endif()

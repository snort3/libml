include(BundleStaticLibrary)

configure_file(version.h.in version.h)
configure_file(libml.pc.in libml.pc @ONLY)
configure_file(libml_static.pc.in libml_static.pc @ONLY)

set(LIBML_API_SOURCES
    libml.cc
    util.cc
)

add_library(libml_api_shared SHARED
    "${LIBML_API_SOURCES}"
)

add_library(libml_api_static STATIC
    "${LIBML_API_SOURCES}"
)

set(LIBML_INCLUDE_DIRS
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src"
)

target_include_directories(libml_api_shared PUBLIC
    "${LIBML_INCLUDE_DIRS}"
)

target_include_directories(libml_api_static PUBLIC
    "${LIBML_INCLUDE_DIRS}"
)

target_link_libraries(libml_api_shared PUBLIC
    tensorflow-lite
)

target_link_libraries(libml_api_static PUBLIC
    tensorflow-lite
)

set_target_properties(libml_api_shared PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libml"
    PUBLIC_HEADER "libml.h"
)

set_target_properties(libml_api_static PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libml"
)

bundle_static_library(libml_api_static ml_static)

set(BUNDLE_NAME "libml_static${CMAKE_STATIC_LIBRARY_SUFFIX}")

install(
    TARGETS libml_api_shared
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    FILES "${CMAKE_BINARY_DIR}/${BUNDLE_NAME}"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libml.pc"
        "${CMAKE_CURRENT_BINARY_DIR}/libml_static.pc"
    DESTINATION
        "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

add_subdirectory(test)

cmake_minimum_required(VERSION 3.16)

project(libml C CXX)

set(VERSION_MAJOR 2)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

set(CMAKE_EXPORT_COMPILE_COMMANDS True)
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)

include(GNUInstallDirs)

include(BuildType)
include(CompilerFlags)
include(Libraries)
include(Macros)
include(InstallPath)

add_subdirectory(src)
add_subdirectory(examples)

message("
-------------------------------------------------------
${CMAKE_PROJECT_NAME} version ${VERSION}

Build options:
    type:    ${CMAKE_BUILD_TYPE}
    prefix:  ${CMAKE_INSTALL_PREFIX}

Compiler options:
    CXX:     ${CMAKE_CXX_COMPILER}
-------------------------------------------------------\n")

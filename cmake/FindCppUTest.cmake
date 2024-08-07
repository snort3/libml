find_package(PkgConfig)
pkg_check_modules(PC_CPPUTEST cpputest)

find_library(CPPUTEST_LIBRARY NAMES CppUTest
    HINTS ${PC_CPPUTEST_LIBDIR} ${PC_CPPUTEST_LIBRARY_DIRS})

find_library(CPPUTEST_EXT_LIBRARY NAMES CppUTestExt
    HINTS ${PC_CPPUTEST_LIBDIR} ${PC_CPPUTEST_LIBRARY_DIRS})

find_path(CPPUTEST_INCLUDE_DIRS CppUTest/TestHarness.h
    HINTS ${PC_CPPUTEST_INCLUDEDIR} ${PC_CPPUTEST_INCLUDE_DIRS})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(CppUTest
    DEFAULT_MSG
    CPPUTEST_LIBRARY
    CPPUTEST_EXT_LIBRARY
    CPPUTEST_INCLUDE_DIRS
)

if(CPPUTEST_LIBRARY AND CPPUTEST_EXT_LIBRARY)
    set(CPPUTEST_LIBRARIES ${CPPUTEST_LIBRARY} ${CPPUTEST_EXT_LIBRARY})
endif()

mark_as_advanced(
    CPPUTEST_LIBRARY
    CPPUTEST_EXT_LIBRARY
    CPPUTEST_INCLUDE_DIRS
)

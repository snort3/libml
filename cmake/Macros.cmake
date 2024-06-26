function(add_cpputest testname)
    if(ENABLE_TESTS)
        set(multiValueArgs SOURCES INCLUDES LIBS)
        cmake_parse_arguments(utest "" "" "${multiValueArgs}" "${ARGN}")

        add_executable("${testname}" EXCLUDE_FROM_ALL
            "${testname}.cc" "${utest_SOURCES}")

        target_include_directories("${testname}" PRIVATE
            "${CPPUTEST_INCLUDE_DIRS}" "${utest_INCLUDES}")

        target_link_libraries("${testname}" PRIVATE
            "${CPPUTEST_LIBRARIES}" "${utest_LIBS}")

        add_test("${testname}_build"
            "${CMAKE_COMMAND}"
                --build "${CMAKE_BINARY_DIR}"
                --target "${testname}"
        )

        set_tests_properties("${testname}_build" PROPERTIES
            FIXTURES_SETUP "${testname}_fixture")

        add_test(NAME "${testname}" COMMAND ${testname})

        set_tests_properties("${testname}" PROPERTIES
            FIXTURES_REQUIRED "${testname}_fixture")
    endif()
endfunction()

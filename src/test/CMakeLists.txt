add_cpputest(binary_classifier_test
    SOURCES
        "${PROJECT_SOURCE_DIR}/src/util.cc"
    INCLUDES
        "${PROJECT_SOURCE_DIR}/src"
    LIBS
        libml_api_static
)

file(
    COPY models
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)

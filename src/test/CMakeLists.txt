add_cpputest(binary_classifier_test
    INCLUDES
        "${PROJECT_SOURCE_DIR}/src"
    LIBS
        libml_api_static
)

file(
    COPY test.model
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(classifier
    classifier.cc
)

target_include_directories(classifier PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(classifier PRIVATE
    libml_api_static
)

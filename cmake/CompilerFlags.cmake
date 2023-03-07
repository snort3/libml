set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    #add_compile_options(/W4)
else()
    #add_compile_options(
    #    -Wall -Wextra -Wshadow
    #    -Wconversion -Wsign-conversion
    #    -pedantic
    #)
endif()
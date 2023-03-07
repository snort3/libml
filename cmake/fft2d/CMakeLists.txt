project(fft2d C)

add_library(fft2d_alloc
  "${FFT2D_SOURCE_DIR}/alloc.c"
  "${FFT2D_SOURCE_DIR}/alloc.h"
)
target_include_directories(fft2d_alloc PUBLIC "${FFT2D_SOURCE_DIR}")

add_library(fft2d_fft4f2d "${FFT2D_SOURCE_DIR}/fft4f2d.c")
target_include_directories(fft2d_fft4f2d PRIVATE "${FFT2D_SOURCE_DIR}")

add_library(fft2d_fftsg "${FFT2D_SOURCE_DIR}/fftsg.c")

if(NOT CMAKE_SYSTEM_NAME STREQUAL Windows)
    target_link_libraries(fft2d_fftsg m)
endif()

add_library(fft2d_fftsg2d "${FFT2D_SOURCE_DIR}/fftsg2d.c")
target_link_libraries(fft2d_fftsg2d fft2d_fftsg)
target_include_directories(fft2d_fftsg2d PRIVATE "${FFT2D_SOURCE_DIR}")

add_library(fft2d_fftsg3d "${FFT2D_SOURCE_DIR}/fftsg3d.c")
target_link_libraries(fft2d_fftsg3d fft2d_fftsg)
target_include_directories(fft2d_fftsg3d PRIVATE "${FFT2D_SOURCE_DIR}")

add_library(fft2d_shrtdct "${FFT2D_SOURCE_DIR}/shrtdct.c")

add_library(fft2d ALIAS fft2d_fftsg2d)

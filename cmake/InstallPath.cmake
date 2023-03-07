if(UNIX AND NOT APPLE)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "/usr" CACHE
            PATH "Install Path" FORCE)
    endif()
endif()

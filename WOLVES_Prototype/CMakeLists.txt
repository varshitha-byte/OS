cmake_minimum_required(VERSION 3.10)
project(WOLVES_Prototype)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src)

add_executable(wolves_sim 
    src/main.cpp
    src/pmem.cpp
    src/wofs.cpp
)

if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE REQUIRED fuse)
    
    add_executable(wolves_fuse 
        src/fuse_main.cpp
        src/pmem.cpp
        src/wofs.cpp
    )
    target_compile_definitions(wolves_fuse PRIVATE _FILE_OFFSET_BITS=64)
    target_include_directories(wolves_fuse PRIVATE ${FUSE_INCLUDE_DIRS})
    target_link_libraries(wolves_fuse ${FUSE_LIBRARIES})
endif()

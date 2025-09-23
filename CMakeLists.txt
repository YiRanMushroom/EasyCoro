cmake_minimum_required(VERSION 4.0)
project(EasyCoro)

set(CMAKE_CXX_STANDARD 23)

add_executable(EasyCoro src/main.cpp)

target_sources(EasyCoro PUBLIC
        FILE_SET top_module
        TYPE CXX_MODULES
        FILES
        src/EasyCoro.ixx)

file(GLOB_RECURSE interface_modules src/interface/*.ixx)
target_sources(EasyCoro PUBLIC
        FILE_SET interface_modules
        TYPE CXX_MODULES
        FILES
        ${interface_modules})

file(GLOB_RECURSE implementation_modules src/impl/*.ixx)
target_sources(EasyCoro PRIVATE
        FILE_SET implementation_modules
        TYPE CXX_MODULES
        FILES
        ${implementation_modules})

file(GLOB_RECURSE source_files src/*.cpp)
target_sources(EasyCoro PRIVATE ${source_files})
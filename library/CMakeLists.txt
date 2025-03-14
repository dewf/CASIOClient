cmake_minimum_required(VERSION 3.30)
project(CASIOClient)

set(CMAKE_CXX_STANDARD 20)

add_library(CASIOClient SHARED
        source/dllmain.cpp
        source/CASIOClient.cpp
        source/util/unicodestuff.cpp
)

add_compile_definitions(CASIOCLIENT_EXPORTS)

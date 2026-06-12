cmake_minimum_required(VERSION 3.18)

project(LetMeCraftMods)

set(RE_UE4SS_DIR "${CMAKE_CURRENT_LIST_DIR}/RE-UE4SS" CACHE PATH "Path to a RE-UE4SS checkout")

if(NOT EXISTS "${RE_UE4SS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "RE-UE4SS was not found. Clone it next to this file as cpp/RE-UE4SS "
        "or configure with -DRE_UE4SS_DIR=<path-to-RE-UE4SS>.")
endif()

add_subdirectory("${RE_UE4SS_DIR}" "${CMAKE_BINARY_DIR}/RE-UE4SS")
add_subdirectory(LetMeCraft)

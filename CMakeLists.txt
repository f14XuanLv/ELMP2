cmake_minimum_required(VERSION 3.22)
project(ELMP CXX C)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ELMP supports only MSVC x64. Use build.bat.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Offline builds: ELMP_FETCH_CACHE = directory of "<name>-src" FetchContent trees fetched for the same
# RE-UE4SS commit (devconfig "fetchCache"). Each one is used instead of downloading.
set(ELMP_FETCH_CACHE "$ENV{ELMP_FETCH_CACHE}" CACHE PATH "Directory with <name>-src FetchContent sources")
if(ELMP_FETCH_CACHE)
    file(GLOB cached LIST_DIRECTORIES true "${ELMP_FETCH_CACHE}/*-src")
    foreach(dir ${cached})
        get_filename_component(name "${dir}" NAME)
        string(REGEX REPLACE "-src$" "" name "${name}")
        string(TOUPPER "${name}" name)
        set(FETCHCONTENT_SOURCE_DIR_${name} "${dir}")
    endforeach()
endif()

# Pinned RE-UE4SS submodule (32d8a381): the UE4SS target exports the C++ mod API, PolyHook_2 and ImGui.
add_subdirectory(RE-UE4SS)

# elmp.json is the single source of version, API / protocol versions and supported game binaries (C1.5).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/elmp.json" ELMP_MANIFEST)
string(JSON ELMP_VERSION GET "${ELMP_MANIFEST}" version)
string(JSON ELMP_API_CORE GET "${ELMP_MANIFEST}" api "elmp.core")
string(JSON ELMP_PROTOCOL GET "${ELMP_MANIFEST}" protocol)
string(JSON ELMP_GAME_EXE GET "${ELMP_MANIFEST}" game exe)
string(JSON ELMP_UE4SS GET "${ELMP_MANIFEST}" ue4ss)
string(JSON hash_count LENGTH "${ELMP_MANIFEST}" game sha256)
set(ELMP_GAME_SHA256_LIST "")
math(EXPR hash_last "${hash_count} - 1")
foreach(i RANGE ${hash_last})
    string(JSON hash GET "${ELMP_MANIFEST}" game sha256 ${i})
    string(TOLOWER "${hash}" hash)
    string(APPEND ELMP_GAME_SHA256_LIST "\"${hash}\", ")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/elmp.json")
set(ELMP_GENERATED_DIR "${CMAKE_BINARY_DIR}/elmp_generated")
configure_file(native/build_info.h.in "${ELMP_GENERATED_DIR}/elmp/build_info.h" @ONLY)

include(CTest)
add_subdirectory(native)

# libmain.so (Win64 runtime + lifted Mewgenics.exe + GLSL translator) and libSDL3.so for
# arm64-v8a. Built with tools/build_android_so.sh; the results go to app/src/main/jniLibs and
# are never committed (derived from the game).
cmake_minimum_required(VERSION 3.22)
project(mewrecomp C CXX)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(GEN_DIR "" CACHE PATH "generated recomp_*.c")
set(GUEST_OPT "-O1" CACHE STRING "optimisation for generated code")

# SDL3 (shared: SDLActivity loads libSDL3.so itself)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/third_party/SDL3-3.2.12 sdl3)

# shader translation: glslang (desktop GLSL -> SPIR-V) and SPIRV-Cross (SPIR-V -> ESSL)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/third_party/glslang-14.3.0 glslang)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/third_party/SPIRV-Cross-vulkan-sdk-1.4.304.0 spvc)

# lifted game code
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN_DIR}/recomp_*.c)
add_library(mew_guest OBJECT ${GEN_SOURCES})
target_include_directories(mew_guest PRIVATE ${ROOT}/runtime/include ${GEN_DIR})
target_compile_options(mew_guest PRIVATE ${GUEST_OPT} -w -fwrapv -fno-strict-aliasing -ffp-contract=off -g0
    -fvisibility=hidden -fno-unwind-tables -fno-asynchronous-unwind-tables)
set_property(TARGET mew_guest PROPERTY POSITION_INDEPENDENT_CODE ON)
if(EXISTS ${ROOT}/config/o0_units.txt)
  file(STRINGS ${ROOT}/config/o0_units.txt O0_UNITS REGEX "^[a-z]")
  foreach(u ${O0_UNITS})
    string(STRIP "${u}" u)
    set_source_files_properties(${GEN_DIR}/${u}.c PROPERTIES COMPILE_OPTIONS "-O0")
  endforeach()
endif()

file(GLOB RT_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/src/*.c)
add_library(main SHARED ${CMAKE_CURRENT_SOURCE_DIR}/android_main.c ${RT_SOURCES} ${ROOT}/runtime/include/eh64.c
    ${ROOT}/runtime/src/glsl_xlate.cpp $<TARGET_OBJECTS:mew_guest>)
target_include_directories(main PRIVATE ${ROOT}/runtime/include ${ROOT}/runtime/src ${GEN_DIR}
    ${ROOT}/third_party/glslang-14.3.0 ${ROOT}/third_party/SPIRV-Cross-vulkan-sdk-1.4.304.0)
# -fsigned-char: plain char is unsigned on arm64 but signed on the x86 host the runtime is developed on.
target_compile_options(main PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -fwrapv -fno-strict-aliasing -ffp-contract=off -fsigned-char -Wno-unused-function>
    $<$<COMPILE_LANGUAGE:CXX>:-O2 -std=c++17>)
target_link_libraries(main PRIVATE SDL3::SDL3-shared glslang glslang-default-resource-limits SPIRV spirv-cross-glsl spirv-cross-core log android m)
target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384")
set_target_properties(main PROPERTIES LINKER_LANGUAGE CXX)

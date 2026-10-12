cmake_minimum_required(VERSION 3.20)
project(RenderAlphaFix VERSION 1.3.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "RenderAlphaFix must be built as a 32-bit Windows DLL")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "RenderAlphaFix requires the MSVC x86 compiler")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(third_party/kthook EXCLUDE_FROM_ALL)

add_library(RenderAlphaFix SHARED src/main.cpp)
target_link_libraries(RenderAlphaFix PRIVATE kthook)
target_compile_definitions(RenderAlphaFix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RenderAlphaFix PRIVATE /W4 /permissive- /EHsc /guard:cf-)
target_link_options(RenderAlphaFix PRIVATE /MACHINE:X86 /SUBSYSTEM:WINDOWS)

set_target_properties(RenderAlphaFix PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "RenderAlphaFix")

option(RENDER_ALPHA_FIX_BUILD_TESTS "Build the standalone mock GTA host" OFF)
if(RENDER_ALPHA_FIX_BUILD_TESTS)
    include(CTest)
    add_executable(RenderAlphaFixMock tests/mock_host.cpp)
    target_compile_options(RenderAlphaFixMock PRIVATE /W4 /permissive- /EHsc)
    target_link_options(RenderAlphaFixMock PRIVATE
        /MACHINE:X86 /SUBSYSTEM:CONSOLE /BASE:0x00400000 /FIXED
        /DYNAMICBASE:NO /SECTION:.mock,RWE)
    set_target_properties(RenderAlphaFixMock PROPERTIES OUTPUT_NAME "mock_host")
    add_test(NAME RenderAlphaFixMock
        COMMAND RenderAlphaFixMock $<TARGET_FILE:RenderAlphaFix>)
    set_tests_properties(RenderAlphaFixMock PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:RenderAlphaFixMock>)
endif()

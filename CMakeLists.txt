cmake_minimum_required(VERSION 3.20)
project(RenderAlphaFix VERSION 1.0.0 LANGUAGES C CXX)

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

add_custom_command(TARGET RenderAlphaFix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/RenderAlphaFix.ini"
            "$<TARGET_FILE_DIR:RenderAlphaFix>/RenderAlphaFix.ini")

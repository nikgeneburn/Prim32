cmake_minimum_required(VERSION 3.16)

project(Prim32 VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Prim32 currently requires Windows and Direct3D 12.")
endif()

set(PRIM32_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PRIM32_IS_TOP_LEVEL ON)
endif()

option(PRIM32_BUILD_EXAMPLES "Build the Prim32 D3D12 demo" ${PRIM32_IS_TOP_LEVEL})

set(PRIM32_PUBLIC_HEADERS
    include/prim32/prim32.h
    include/prim32/prim32_dx12.h
    include/prim32/p32prof.h
)

set(PRIM32_SOURCES
    src/prim32.cpp
    src/prim32_resources.cpp
    src/prim32_freetype.cpp
    src/prim32_internal.h
    src/backends/prim32_dx12.cpp
    src/profiler/p32prof.cpp
    src/profiler/p32prof_ui.cpp
)

# Optional FreeType rasterizer: covers every Unicode plane (the default GDI
# path covers the Basic Multilingual Plane with zero dependencies).
#   vcpkg install freetype && cmake -DPRIM32_WITH_FREETYPE=ON ...
option(PRIM32_WITH_FREETYPE "Rasterize fonts with FreeType instead of GDI" OFF)

add_library(prim32 STATIC)
add_library(Prim32::Prim32 ALIAS prim32)

target_sources(prim32 PRIVATE ${PRIM32_PUBLIC_HEADERS} ${PRIM32_SOURCES})
target_compile_features(prim32 PUBLIC cxx_std_17)
target_include_directories(prim32
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(prim32 PRIVATE UNICODE _UNICODE)
if(PRIM32_WITH_FREETYPE)
    find_package(Freetype REQUIRED)
    target_compile_definitions(prim32 PRIVATE PRIM32_HAS_FREETYPE)
    target_link_libraries(prim32 PRIVATE Freetype::Freetype)
endif()
target_link_libraries(prim32
    PUBLIC
        d3d12
        dxgi
        d3dcompiler
        gdi32
        user32
        psapi
        ole32
        windowscodecs
)

if(MSVC)
    target_compile_options(prim32 PRIVATE /O2 /Oi /fp:fast /EHs-c- /GR-)
else()
    target_compile_options(prim32 PRIVATE -O2 -ffast-math -fno-exceptions -fno-rtti)
endif()

set_target_properties(prim32 PROPERTIES
    OUTPUT_NAME Prim32
    EXPORT_NAME Prim32
    DEBUG_POSTFIX d
    FOLDER Prim32
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    ${PRIM32_PUBLIC_HEADERS}
    ${PRIM32_SOURCES}
)

if(PRIM32_BUILD_EXAMPLES)
    add_executable(prim32_demo WIN32
        examples/d3d12_demo/main.cpp
        examples/d3d12_demo/embedded_png.h
    )
    target_link_libraries(prim32_demo PRIVATE Prim32::Prim32)
    target_compile_features(prim32_demo PRIVATE cxx_std_17)
    target_compile_definitions(prim32_demo PRIVATE UNICODE _UNICODE)
    set_target_properties(prim32_demo PROPERTIES
        OUTPUT_NAME Prim32Demo
        FOLDER Prim32/Examples
    )
endif()

include(GNUInstallDirs)

install(TARGETS prim32
    EXPORT Prim32Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT Prim32Targets
    FILE Prim32Targets.cmake
    NAMESPACE Prim32::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Prim32
)

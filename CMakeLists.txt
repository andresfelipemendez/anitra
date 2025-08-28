cmake_minimum_required(VERSION 4.1)

project(AnitraEngine)

# Build speed optimizations
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Enable parallel compilation on Windows
if(WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
endif()

# Use precompiled headers for faster compilation
set(CMAKE_PCH_INSTANTIATE_TEMPLATES ON)

# Enable ccache if available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()
include(FetchContent)
include_directories(include)

set(SRC_FILES src/main.cpp)

if(WIN32)
  list(APPEND SRC_FILES src/core/loadlibrary_windows.cpp)
elseif(UNIX)
  list(APPEND SRC_FILES src/core/loadlibrary_linux.cpp)
endif()

add_executable(${PROJECT_NAME} ${SRC_FILES})
set_target_properties(
  ${PROJECT_NAME} 
  PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY 
  "${CMAKE_SOURCE_DIR}"
)

find_package(OpenGL REQUIRED)

set(GLFW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/lib/glfw/include")
set(GLFW_LIB_PATH "${CMAKE_SOURCE_DIR}/lib/glfw/lib/glfw3.lib")
set(GLAD_DIR "${CMAKE_SOURCE_DIR}/lib/glad") 
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/lib/imgui-1.90.9")
set(TRACY_DIR "${CMAKE_SOURCE_DIR}/lib/tracy")

target_include_directories(
  ${PROJECT_NAME} 
  PRIVATE 
  "${CMAKE_SOURCE_DIR}/include" 
  "${CMAKE_SOURCE_DIR}/src" 
  "${CMAKE_SOURCE_DIR}/src/core"
  "${OPENGL_INCLUDE_DIR}"
  "${GLFW_INCLUDE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH})

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE gdi32 user32 shell32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_STATIC)

option(TRACY_ENABLE "" ON)
option(TRACY_ON_DEMAND "" ON)
add_subdirectory(lib/tracy)

add_library(glad_loader SHARED ${GLAD_DIR}/glad.c)
target_compile_definitions(glad_loader PUBLIC GLAD_GLAPI_EXPORT)
target_include_directories(glad_loader PUBLIC ${GLAD_DIR})
target_link_libraries(glad_loader PUBLIC ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH})

set(EXTERNALS_SRC_FILES
    src/externals/externals.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

add_library(externals SHARED ${EXTERNALS_SRC_FILES})
set_target_properties(externals PROPERTIES CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Add precompiled header for externals (standard library headers only)
target_precompile_headers(externals PRIVATE 
  <iostream>
  <vector>
  <string>
  <memory>
  <algorithm>
  <cstdint>
  <cstring>
)
target_include_directories(
  externals
  PUBLIC 
  "${CMAKE_SOURCE_DIR}/src/engine"
  "${CMAKE_SOURCE_DIR}/src/core"
  "${CMAKE_SOURCE_DIR}/src/externals"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${GLAD_DIR}"
  "${OPENGL_INCLUDE_DIR}"
  "${GLFW_INCLUDE_DIR}"
)
target_compile_definitions(externals PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD GLAD_GLAPI_IMPORT TRACY_ENABLE)
target_link_libraries(externals PRIVATE glad_loader ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH} TracyClient)

set(CORE_SRC_FILES src/core/core.cpp)
if(WIN32)
  list(APPEND CORE_SRC_FILES src/core/loadlibrary_windows.cpp)
elseif(UNIX)
  list(APPEND CORE_SRC_FILES src/core/loadlibrary_linux.cpp)
endif()
add_library(core SHARED ${CORE_SRC_FILES})
set_target_properties(core PROPERTIES CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(
  core
  PRIVATE 
  "${CMAKE_SOURCE_DIR}/src/engine"
  "${CMAKE_SOURCE_DIR}/src/core"
  "${CMAKE_SOURCE_DIR}/src/externals"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${GLAD_DIR}"
  "${OPENGL_INCLUDE_DIR}"
  "${GLFW_INCLUDE_DIR}"
)
target_link_libraries(
  core 
  PRIVATE 
  ${OPENGL_LIBRARIES} 
  ${GLFW_LIB_PATH}
  externals
  TracyClient
)
target_compile_definitions(core PRIVATE TRACY_ENABLE)

file(GLOB ENGINE_SRC_FILES src/engine/*.cpp)
add_library(engine SHARED ${ENGINE_SRC_FILES})
target_include_directories(
  engine
  PRIVATE 
  "${CMAKE_SOURCE_DIR}/src/engine"
  "${CMAKE_SOURCE_DIR}/src/core"
  "${CMAKE_SOURCE_DIR}/src/externals"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${GLAD_DIR}"
  "${OPENGL_INCLUDE_DIR}"
  "${GLFW_INCLUDE_DIR}"
)
target_link_libraries(engine PRIVATE 
  ${OPENGL_LIBRARIES} 
  ${GLFW_LIB_PATH}
  externals 
  glad_loader 
  TracyClient
)

target_compile_definitions(engine PRIVATE GLFW_STATIC GLAD_GLAPI_IMPORT TRACY_ENABLE)

# Fix hotreloading PDB file locking issue by using random PDB names (inspired by Handmade Hero)
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  string(RANDOM LENGTH 8 ALPHABET "0123456789ABCDEF" RANDOM_PDB_SUFFIX_ENGINE)
  set_target_properties(engine PROPERTIES 
    LINK_FLAGS "/PDB:${CMAKE_CURRENT_BINARY_DIR}/Debug/engine_${RANDOM_PDB_SUFFIX_ENGINE}.pdb"
  )
  
  string(RANDOM LENGTH 8 ALPHABET "0123456789ABCDEF" RANDOM_PDB_SUFFIX_EXTERNALS)
  set_target_properties(externals PROPERTIES 
    LINK_FLAGS "/PDB:${CMAKE_CURRENT_BINARY_DIR}/Debug/externals_${RANDOM_PDB_SUFFIX_EXTERNALS}.pdb"
  )
endif()
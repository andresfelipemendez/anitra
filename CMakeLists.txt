cmake_minimum_required(VERSION 3.12)

project(AnitraEngine)

# Set C99 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# Source files for the main executable
set(SRC_FILES src/main.cpp)

# Add platform-specific source files
if(WIN32)
  list(APPEND SRC_FILES src/core/loadlibrary_windows.cpp)
elseif(UNIX)
  list(APPEND SRC_FILES src/core/loadlibrary_linux.cpp)
endif()

add_executable(${PROJECT_NAME} ${SRC_FILES})

# Find OpenGL package
find_package(OpenGL REQUIRED)

# Set the paths for GLFW
set(GLFW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/lib/glfw/include")
set(GLFW_LIB_PATH "${CMAKE_SOURCE_DIR}/lib/glfw/lib/glfw3.lib")

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/lib/imgui-1.90.9")
set(IMGUI_DIR_INCLUDE "${CMAKE_SOURCE_DIR}/lib/imgui")

# Include directories
target_include_directories(
  ${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/include" "${OPENGL_INCLUDE_DIR}"
                          "${GLFW_INCLUDE_DIR}")

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES}
                                              ${GLFW_LIB_PATH})

# Add GLFW-specific libraries for Windows
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE gdi32 user32 shell32)
endif()

# Add any compile definitions if needed
target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_STATIC)

set(EXTERNALS_SRC_FILES
    src/externals/externals.cpp
    lib/imgui-1.90.9/imgui.cpp
    lib/imgui-1.90.9/imgui_demo.cpp
    lib/imgui-1.90.9/imgui_draw.cpp
    lib/imgui-1.90.9/imgui_tables.cpp
    lib/imgui-1.90.9/imgui_widgets.cpp
    lib/imgui-1.90.9/backends/imgui_impl_glfw.cpp
    lib/imgui-1.90.9/backends/imgui_impl_opengl3.cpp)

add_library(externals SHARED ${EXTERNALS_SRC_FILES})
target_include_directories(
  externals
  PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/include"
          "${CMAKE_SOURCE_DIR}/src" "${IMGUI_DIR_INCLUDE}"
          "${OPENGL_INCLUDE_DIR}" "${GLFW_INCLUDE_DIR}")
target_link_libraries(externals PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH}
                                        imgui)

set(CORE_SRC_FILES src/core/core.cpp)
if(WIN32)
  list(APPEND CORE_SRC_FILES src/core/loadlibrary_windows.cpp)
elseif(UNIX)
  list(APPED CORE_SRC_FILES src/core/loadlibrary_linux.cpp)
endif()

# Create a shared library for the core
add_library(core SHARED ${CORE_SRC_FILES})

target_include_directories(
  core
  PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src"
          "${IMGUI_DIR_INCLUDE}" "${OPENGL_INCLUDE_DIR}" "${GLFW_INCLUDE_DIR}")

target_link_libraries(core PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH}
                                   externals imgui)

set(ENGINE_SRC_FILES src/engine/engine.cpp)

add_library(engine SHARED ${ENGINE_SRC_FILES})
target_include_directories(
  engine
  PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src"
          "${IMGUI_DIR_INCLUDE}" "${OPENGL_INCLUDE_DIR}" "${GLFW_INCLUDE_DIR}")
target_link_libraries(engine PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH}
                                     externals imgui)
target_compile_definitions(engine PRIVATE GLFW_STATIC)

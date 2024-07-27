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

find_package(OpenGL REQUIRED)

set(GLFW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/lib/glfw/include")
set(GLFW_LIB_PATH "${CMAKE_SOURCE_DIR}/lib/glfw/lib/glfw3.lib")

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/lib/imgui-1.90.9")

target_include_directories(
  ${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/include" "${OPENGL_INCLUDE_DIR}"
                          "${GLFW_INCLUDE_DIR}")

target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES}
                                              ${GLFW_LIB_PATH})

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE gdi32 user32 shell32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_STATIC)

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

target_include_directories(
  externals
  PRIVATE "${CMAKE_SOURCE_DIR}/src/engine"
          "${CMAKE_SOURCE_DIR}/src/core"
          "${CMAKE_SOURCE_DIR}/src/externals"
          "${CMAKE_SOURCE_DIR}/src"
          "${CMAKE_SOURCE_DIR}/include"
          "${IMGUI_DIR}"
          "${IMGUI_DIR}/backends"
          "${OPENGL_INCLUDE_DIR}"
          "${GLFW_INCLUDE_DIR}")

target_link_libraries(externals PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH})

set(CORE_SRC_FILES src/core/core.cpp)
if(WIN32)
  list(APPEND CORE_SRC_FILES src/core/loadlibrary_windows.cpp)
elseif(UNIX)
  list(APPED CORE_SRC_FILES src/core/loadlibrary_linux.cpp)
endif()

add_library(core SHARED ${CORE_SRC_FILES})

target_include_directories(
  core
  PRIVATE "${CMAKE_SOURCE_DIR}/src/engine"
          "${CMAKE_SOURCE_DIR}/src/core"
          "${CMAKE_SOURCE_DIR}/src/externals"
          "${CMAKE_SOURCE_DIR}/src"
          "${CMAKE_SOURCE_DIR}/include"
          "${IMGUI_DIR}"
          "${IMGUI_DIR}/backends"
          "${OPENGL_INCLUDE_DIR}"
          "${GLFW_INCLUDE_DIR}")

target_link_libraries(core PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH}
                                   externals)

set(ENGINE_SRC_FILES src/engine/engine.cpp)

add_library(engine SHARED ${ENGINE_SRC_FILES})

target_include_directories(
  engine
  PRIVATE "${CMAKE_SOURCE_DIR}/src/engine"
          "${CMAKE_SOURCE_DIR}/src/core"
          "${CMAKE_SOURCE_DIR}/src/externals"
          "${CMAKE_SOURCE_DIR}/src"
          "${CMAKE_SOURCE_DIR}/include"
          "${IMGUI_DIR}"
          "${IMGUI_DIR}/backends"
          "${OPENGL_INCLUDE_DIR}"
          "${GLFW_INCLUDE_DIR}")

target_link_libraries(engine PRIVATE ${OPENGL_LIBRARIES} ${GLFW_LIB_PATH}
                                     externals)

target_compile_definitions(engine PRIVATE GLFW_STATIC)

cmake_minimum_required(VERSION 3.12)

set(CMAKE_GENERATOR_PLATFORM x64)

project(GameEngine C)

# Set C99 standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add your source files
file(GLOB_RECURSE SOURCES "src/*.c")

# Create the executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Find OpenGL package
find_package(OpenGL REQUIRED)

# Set the paths for GLFW
set(GLFW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/lib/glfw/include")
set(GLFW_LIB_PATH "${CMAKE_SOURCE_DIR}/lib/glfw/lib/glfw3.lib")

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${CMAKE_SOURCE_DIR}/include"
    "${OPENGL_INCLUDE_DIR}"
    "${GLFW_INCLUDE_DIR}"
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE 
    ${OPENGL_LIBRARIES}
    ${GLFW_LIB_PATH}
)

# Add GLFW-specific libraries for Windows
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE 
        gdi32
        user32
        shell32
    )
endif()

# Add any compile definitions if needed
target_compile_definitions(${PROJECT_NAME} PRIVATE
    GLFW_STATIC
)

# Set additional compiler flags if needed
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()
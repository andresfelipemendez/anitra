cmake_minimum_required(VERSION 3.10)

project(sqlite3 C)

add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1)

if(UNIX AND NOT APPLE)
    target_link_libraries(sqlite3 PRIVATE dl)
endif()
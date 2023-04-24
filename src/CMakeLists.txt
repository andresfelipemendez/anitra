
project(anitra_src)

set(DB_SOURCES
        systems/database_system.cpp
        systems/logging_system.cpp
        systems/settings_system.cpp)

add_executable(anitra main.cpp ${DB_SOURCES})

target_include_directories(anitra PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(anitra PRIVATE sqlite3)
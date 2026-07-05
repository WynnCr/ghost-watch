cmake_minimum_required(VERSION 3.10)
project(ghost-watch)

set(CMAKE_CXX_STANDARD 20)

find_package(SQLite3 REQUIRED)

# Compatibility block: Fedora 43 and older environments may not define
# the SQLite3::SQLite3 target, so we define a fallback manually.
if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
  target_link_libraries(SQLite3::SQLite3 INTERFACE sqlite3)
endif()

include(FetchContent)
FetchContent_Declare(ftxui GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI GIT_TAG v7.0.0)
FetchContent_MakeAvailable(ftxui)

add_executable(ghost-watch-tui src/dashboard.cpp src/db.cpp src/utils.cpp src/components.cpp)
target_link_libraries(ghost-watch-tui PRIVATE ftxui::ftxui SQLite3::SQLite3)

add_executable(ghost-watch-daemon src/main.cpp)
target_link_libraries(ghost-watch-daemon PRIVATE SQLite3::SQLite3)

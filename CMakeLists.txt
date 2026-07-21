cmake_minimum_required(VERSION 4.3.0)

project(chess-api LANGUAGES CXX)

add_executable(chess-api main.cpp)
target_compile_features(chess-api PRIVATE cxx_std_17)
target_include_directories(chess-api PRIVATE include)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
target_link_libraries(chess-api PRIVATE Threads::Threads SQLite3::SQLite3)

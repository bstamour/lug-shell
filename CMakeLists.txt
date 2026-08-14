cmake_minimum_required(VERSION 3.30)

project(lug-shell LANGUAGES C)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

add_executable(lug-shell src/lugshell.c)

find_package(Readline)

target_link_libraries(lug-shell readline)

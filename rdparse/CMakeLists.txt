cmake_minimum_required(VERSION 2.8)
project(rdparse)

include_directories(include)

file(GLOB SOURCES "src/*.c")
file(GLOB HEADERS "include/rdparse/*.h")

add_library(rdparse SHARED ${SOURCES})
set_target_properties(rdparse PROPERTIES PUBLIC_HEADER "${HEADERS}")

install(TARGETS rdparse
  LIBRARY DESTINATION lib
  PUBLIC_HEADER DESTINATION include/rdparse
)

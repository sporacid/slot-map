cmake_minimum_required(VERSION 3.20)

option(SPORE_WITH_TESTS "Whether to build tests" ON)

project(spore.slot-map)

set(CMAKE_CXX_STANDARD 20)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_SOURCE_DIR}/include)

add_library(spore::slot-map ALIAS ${PROJECT_NAME})

if (SPORE_WITH_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif ()

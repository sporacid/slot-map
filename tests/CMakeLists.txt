set(TARGET_NAME ${PROJECT_NAME}.tests)

list(APPEND TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/t_slot_map.cpp)

add_executable(${TARGET_NAME} ${TARGET_FILES})

target_link_libraries(
  ${TARGET_NAME} PRIVATE
  ${PROJECT_NAME}
  Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_BINARY_DIR}/content/catch2/extras)

find_package(Catch2 CONFIG REQUIRED)

include(Catch)

catch_discover_tests(${TARGET_NAME})
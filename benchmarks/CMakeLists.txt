macro(add_benchmark TARGET_NAME TARGET_FILES)
  add_executable(${TARGET_NAME} ${TARGET_FILES})
  target_include_directories(
    ${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/thirdparty
    # ${PLF_COLONY_INCLUDE_DIRS}
  )
  target_link_libraries(
    ${TARGET_NAME} PRIVATE
    ${PROJECT_NAME}
  )
  target_compile_options(

  )
endmacro()

# find_path(PLF_COLONY_INCLUDE_DIRS "plf_colony.h")

add_benchmark(${PROJECT_NAME}.benchmarks.st ${CMAKE_CURRENT_SOURCE_DIR}/bench_st.cpp)
add_benchmark(${PROJECT_NAME}.benchmarks.mt ${CMAKE_CURRENT_SOURCE_DIR}/bench_mt.cpp)

cmake_minimum_required(VERSION 3.20)
project(nuget_graph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(nuget-graph src/main.cpp src/repository.cpp)
if (WIN32)
  target_link_libraries(nuget-graph PRIVATE winhttp)
endif()

include(CTest)
if (BUILD_TESTING AND UNIX)
  add_test(NAME graph_scenarios
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:nuget-graph>)
  find_package(Python3 COMPONENTS Interpreter)
  if (Python3_Interpreter_FOUND)
    add_test(NAME nuget_registration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_nuget.py
        $<TARGET_FILE:nuget-graph>)
  endif()
endif()

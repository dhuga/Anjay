# Copyright 2017-2018 AVSystem <avsystem@avsystem.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if(NOT "${CMAKE_C_COMPILER}" MATCHES "^(.*/)?afl-gcc$")
    message(FATAL_ERROR "AFL fuzz tests require compiling with afl-gcc, but CMAKE_C_COMPILER is set to ${CMAKE_C_COMPILER}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

set(FUZZ_TEST_NAMES "" PARENT_SCOPE)
set(FUZZ_TEST_ARG_LISTS "" PARENT_SCOPE)
set(FUZZ_TEST_CASE_DIRS "" PARENT_SCOPE)
set(FUZZ_TEST_FINDINGS_DIRS "" PARENT_SCOPE)

macro(add_fuzz_target NAME EXECUTABLE)
    get_filename_component(EXECUTABLE "${EXECUTABLE}" ABSOLUTE)

    list(APPEND FUZZ_TEST_NAMES ${NAME})
    list(APPEND FUZZ_TEST_ARG_LISTS "FUZZ_TEST_ARG_LIST_${NAME}")
    list(APPEND FUZZ_TEST_CASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/test_cases/${NAME}")
    list(APPEND FUZZ_TEST_FINDINGS_DIRS "${CMAKE_CURRENT_BINARY_DIR}/findings/${NAME}")

    set("FUZZ_TEST_ARG_LIST_${NAME}" "${EXECUTABLE}" ${ARGN} PARENT_SCOPE)

    set(FUZZ_TEST_NAMES "${FUZZ_TEST_NAMES}" PARENT_SCOPE)
    set(FUZZ_TEST_ARG_LISTS "${FUZZ_TEST_ARG_LISTS}" PARENT_SCOPE)
    set(FUZZ_TEST_CASE_DIRS "${FUZZ_TEST_CASE_DIRS}" PARENT_SCOPE)
    set(FUZZ_TEST_FINDINGS_DIRS "${FUZZ_TEST_FINDINGS_DIRS}" PARENT_SCOPE)
endmacro()

file(GLOB_RECURSE FUZZ_TEST_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c)

foreach(FUZZ_TEST_SOURCE ${FUZZ_TEST_SOURCES})
    get_filename_component(FUZZ_TEST_DIR "${FUZZ_TEST_SOURCE}" DIRECTORY)
    get_filename_component(FUZZ_TEST_OUTPUT "${FUZZ_TEST_SOURCE}" NAME_WE)
    set(FUZZ_TEST_OUTPUT "${FUZZ_TEST_DIR}/${FUZZ_TEST_OUTPUT}")
    string(REPLACE / _ FUZZ_TEST_NAME ${FUZZ_TEST_OUTPUT})

    add_executable(${FUZZ_TEST_NAME} ${FUZZ_TEST_SOURCE} ${ABSOLUTE_SOURCES})
    target_link_libraries(${FUZZ_TEST_NAME} ${PROJECT_NAME})
    set_property(TARGET ${FUZZ_TEST_NAME} APPEND PROPERTY COMPILE_DEFINITIONS
                 ANJAY_FUZZ_TEST)

    add_fuzz_target(${FUZZ_TEST_NAME} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${FUZZ_TEST_NAME}")
endforeach()

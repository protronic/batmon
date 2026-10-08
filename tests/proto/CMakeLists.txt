cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(batmon_proto_test LANGUAGES C)

set(BATMON_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_include_directories(app PRIVATE src ${BATMON_ROOT}/src/proto ${BATMON_ROOT}/tests/common)
target_sources(app PRIVATE
  src/main.c
  ${BATMON_ROOT}/src/proto/bm_proto.c
  ${BATMON_ROOT}/src/proto/bm_json.c
  ${BATMON_ROOT}/src/proto/bm_ydt.c
  ${BATMON_ROOT}/src/proto/bm_serial.c
  ${BATMON_ROOT}/tests/common/test_cases.c
  ${BATMON_ROOT}/tests/common/test_serial.c
)

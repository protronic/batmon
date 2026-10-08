cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(batmon LANGUAGES C VERSION 0.1.0)

target_include_directories(app PRIVATE src src/proto)

target_sources(app PRIVATE
  src/main.c
  src/can_rx.c
  src/proto/bm_proto.c
  src/proto/bm_json.c
  src/proto/bm_ydt.c
  src/proto/bm_serial.c
)

target_sources_ifdef(CONFIG_BATMON_RS485 app PRIVATE src/rs485.c)
target_sources_ifdef(CONFIG_BATMON_MQTT app PRIVATE src/net_mqtt.c)
target_sources_ifdef(CONFIG_BATMON_UI app PRIVATE src/ui.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/shell_cmds.c)

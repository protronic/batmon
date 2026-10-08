# Host build of the protocol core + shared test cases. No Zephyr required.
#   make -C tests/host        build and run
#   make -C tests/host clean
ROOT  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC    ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g
CPPFLAGS := -I$(ROOT)/src/proto -I$(ROOT)/tests/common -I$(ROOT)/tests/host
SRC := $(ROOT)/src/proto/bm_proto.c $(ROOT)/src/proto/bm_json.c \
       $(ROOT)/tests/common/test_cases.c $(ROOT)/tests/host/main.c
OUT := $(ROOT)/build-host/test_proto

all: run

$(OUT): $(SRC) $(wildcard $(ROOT)/src/proto/*.h) $(wildcard $(ROOT)/tests/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SRC) -o $@

run: $(OUT)
	$(OUT)

clean:
	rm -rf $(ROOT)/build-host

.PHONY: all run clean

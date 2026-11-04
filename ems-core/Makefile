# 交叉编译：make CROSS=aarch64-linux-gnu-（需要目标平台的 libmodbus/libmosquitto/sqlite3）
# 调试：make asan   单元测试：make test
CROSS   ?=
CC      := $(CROSS)gcc
BUILD   ?= build
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -pthread
LDLIBS  := -lmodbus -lsqlite3 -lm -pthread
INC     := -I/usr/include/modbus

COMMON  := src/util/log.c src/config/config.c src/storage/storage.c src/rtdb/rtdb.c \
           src/alarm/event.c third_party/cjson/cJSON.c
CORE    := src/main.c src/acq/acq.c src/control/control.c src/control/algo.c src/cmd/cmd.c src/cmd/cmd_path.c
SYNC    := src/sync/sync_main.c src/cmd/cmd_path.c

obj = $(patsubst %.c,$(BUILD)/%.o,$(1))

all: $(BUILD)/ems-core $(BUILD)/ems-sync

$(BUILD)/ems-core: $(call obj,$(COMMON) $(CORE))
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/ems-sync: $(call obj,$(COMMON) $(SYNC))
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -lmosquitto

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c -o $@ $<

$(BUILD)/test_algo: $(call obj,tests/test_algo.c src/control/algo.c)
	$(CC) $(CFLAGS) -o $@ $^ -lm

test: $(BUILD)/test_algo
	$(BUILD)/test_algo

test-asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

clean:
	rm -rf build build-asan

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
.PHONY: all asan clean test test-asan

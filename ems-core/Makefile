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
CORE    := src/main.c src/acq/acq.c src/control/control.c src/control/algo.c src/cmd/cmd.c src/cmd/cmd_path.c \
           src/rules/rules.c src/rules/rule_core.c third_party/tinyexpr/tinyexpr.c
SYNC    := src/sync/sync_main.c src/cmd/cmd_path.c

obj = $(patsubst %.c,$(BUILD)/%.o,$(1))

all: $(BUILD)/ems-core $(BUILD)/ems-sync

$(BUILD)/ems-core: $(call obj,$(COMMON) $(CORE))
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/ems-sync: $(call obj,$(COMMON) $(SYNC))
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -lmosquitto

# 第三方源码不改，关掉它的告警
$(BUILD)/third_party/tinyexpr/tinyexpr.o: CFLAGS += -w

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c -o $@ $<

$(BUILD)/test_algo: $(call obj,tests/test_algo.c src/control/algo.c)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(BUILD)/test_codec: $(call obj,tests/test_codec.c src/config/config.c src/util/log.c third_party/cjson/cJSON.c)
	$(CC) $(CFLAGS) -o $@ $^ -lm -pthread

$(BUILD)/test_rules: $(call obj,tests/test_rules.c src/rules/rule_core.c third_party/tinyexpr/tinyexpr.c)
	$(CC) $(CFLAGS) -o $@ $^ -lm

TESTS := $(BUILD)/test_algo $(BUILD)/test_codec $(BUILD)/test_rules

# 部分内核（vm.mmap_rnd_bits=32）下 ASan 会偶发死循环，用 setarch -R 关掉地址随机化运行
TEST_RUN ?=

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $(TEST_RUN) $$t || exit 1; done

test-asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        TEST_RUN="$$(command -v setarch >/dev/null && echo setarch -R)" test

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

clean:
	rm -rf build build-asan

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
.PHONY: all asan clean test test-asan

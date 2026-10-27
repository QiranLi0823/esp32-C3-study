# 主机端单元测试 (DHT11 解码器)
#
#   make        编译并运行
#   make clean  清理
#
# 需要 gcc。在 ESP-IDF 的终端里或 Git-Bash / WSL 里都可以跑。

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra
INC     := -I stubs -I ../components/DHT11/include
TARGET  := test_decode

.PHONY: all run clean

all: run

$(TARGET): test_decode.c ../components/DHT11/dht11.c
	$(CC) $(CFLAGS) $(INC) -o $@ test_decode.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

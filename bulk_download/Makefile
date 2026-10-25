CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread

TARGET := bin/bulk_download
SOURCE := src/bulk_download.c
OUTPUT ?= ../word_index

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(SOURCE)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LDLIBS)

run: $(TARGET)
	@./$(TARGET) --output "$(OUTPUT)" $(ARGS)

test: $(TARGET)
	./test/test.sh

clean:
	rm -f $(TARGET)

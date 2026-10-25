CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl libxml-2.0)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl libxml-2.0)

TARGET := bin/extract_word
SOURCE := src/extract_word.c

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(SOURCE)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LDLIBS)

run: $(TARGET)
	@if [ -z "$(WORD)" ]; then \
		echo "usage: make run WORD=<word>" >&2; \
		exit 2; \
	fi
	@./$(TARGET) "$(WORD)"

test: $(TARGET)
	../extract_word/test/test.sh

clean:
	rm -f $(TARGET)

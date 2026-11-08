BREW   ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
CFLAGS += -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -I$(BREW)/include
LDLIBS += -L$(BREW)/lib -lusb-1.0 -lm

all: build/eie-test

build/eie-test: tools/eie-test.c src/eie_usb.c src/eie_usb.h | build
	$(CC) $(CFLAGS) -o $@ tools/eie-test.c src/eie_usb.c $(LDLIBS)

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean

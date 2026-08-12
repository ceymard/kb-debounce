CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
LDFLAGS ?=

TARGET = kb-debounce
SRC    = kb-debounce.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

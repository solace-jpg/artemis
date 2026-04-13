CC = gcc
CFLAGS = -O3 -Wall
TARGET = artemis
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): artemis.c
	$(CC) $(CFLAGS) artemis.c -o $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean

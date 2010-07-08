CFLAGS=-Wall $(shell libmpq-config --cflags) -D_FILE_OFFSET_BITS=64 -ggdb
LDFLAGS=-lfuse $(shell libmpq-config --libs)
DESTDIR=/usr/local
TARGET=mpqfuse

all: $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)/bin
	install -m 755 mpqfuse $(DESTDIR)/bin

clean:
	rm -rf *.o $(TARGET)

.PHONY: all install clean

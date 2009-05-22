CFLAGS=-Wall $(shell libmpq-config --cflags)
LDFLAGS=-lfuse $(shell libmpq-config --libs)
DESTDIR=/usr/local

all: mpqfuse

install: mpqfuse
	install -d $(DESTDIR)/bin
	install -m 755 mpqfuse $(DESTDIR)/bin

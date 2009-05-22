CFLAGS=-Wall $(shell libmpq-config --cflags)
LDFLAGS=-lfuse $(shell libmpq-config --libs)

all: mpqfuse

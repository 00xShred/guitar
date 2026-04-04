CC      = gcc
CFLAGS  = -O2 -Wall
LIBS    = -ljack -lpthread -lm $(shell pkg-config --cflags --libs raylib)
TARGET  = bitcrusher
SRC     = crusher.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean

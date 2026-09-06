CC = clang

CFLAGS += -march=native -O3 -fPIC -Iinclude
LDFLAGS += -shared

TARGET  = libmemrw.so
SRCS    = src/memrw.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

APP = "aesdsocket"
CC ?= $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Werror -g
LDFLAGS ?=

all: $(APP)

$(APP): aesdsocket.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(APP)
	rm -f valgrind-out.txt

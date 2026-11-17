
APP="aesdsocket"
CC=$(CROSS_COMPILE)gcc

CFLAGS += -Wall -Werror -g

all: $(APP)

$(APP): aesdsocket.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(APP)
	rm -f valgrind-out.txt

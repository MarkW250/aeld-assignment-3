
APP="writer"
CC=$(CROSS_COMPILE)gcc

CFLAGS += -Wall -Werror

all: $(APP)

$(APP): writer.c
	$(CC) -o $@ $<

clean:
	rm -f $(APP)

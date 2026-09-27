CC = cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror $(shell pkg-config --cflags portaudio-2.0)
LDLIBS = $(shell pkg-config --libs portaudio-2.0) -lpthread -framework CoreAudio -framework CoreFoundation

all: waveshare-receiver

waveshare-receiver: receiver.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f waveshare-receiver

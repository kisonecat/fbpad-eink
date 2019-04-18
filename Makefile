CC = arm-linux-gnueabihf-cc
CFLAGS = -Wall -O2
LDFLAGS = -lutil # for openpty

all: fbpad

%.o: %.c conf.h
	$(CC) -c $(CFLAGS) $<

fbpad: fbpad.o term.o pad.o draw.o font.o isdw.o scrsnap.o refresh.o
	$(CC) -o $@ $^ $(LDFLAGS)
	patchelf --set-interpreter /lib/ld-linux-armhf.so.3 fbpad

clean:
	rm -f *.o fbpad

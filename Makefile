CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lwinusb -lsetupapi

all: candump.exe cangen.exe wincan.exe cansend.exe

candump.exe: candump.c wincan.c wincan.h gs_usb.h
	$(CC) $(CFLAGS) -o $@ candump.c wincan.c $(LDFLAGS)

cangen.exe: cangen.c wincan.c wincan.h gs_usb.h
	$(CC) $(CFLAGS) -o $@ cangen.c wincan.c $(LDFLAGS)

wincan.exe: wincan_main.c wincan.c wincan.h gs_usb.h
	$(CC) $(CFLAGS) -o $@ wincan_main.c wincan.c $(LDFLAGS)

cansend.exe: cansend.c wincan.c wincan.h gs_usb.h
	$(CC) $(CFLAGS) -o $@ cansend.c wincan.c $(LDFLAGS)

clean:
	del /Q candump.exe cangen.exe wincan.exe cansend.exe 2>NUL & exit 0

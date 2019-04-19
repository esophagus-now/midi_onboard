midi:	main.c midi.c midi.h
	arm-linux-gnueabihf-gcc -g -o midi -Wall -fno-diagnostics-show-caret *.c -lrt

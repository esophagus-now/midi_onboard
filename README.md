### Files

main.c contains a driver function which reads MIDI events and writes them to the correct registers in MMIO.

The real good stuff is in midi.h and midi.c

### Using the Library

The basic objects here are a MIDI event and a MIDI context struct. The definitions are as follows (this is just copy-pasted from the MIDI.h file):

```c
typedef enum {
	NOTE_ON,
	NOTE_OFF,
	PROGRAM_CHANGE,
	CONTROL_CHANGE,
	SYSEX,
	TEMPO_CHANGE,
	AFTERTOUCH,
	PITCH_WHEEL
} MIDI_ev_type;

typedef struct {
	MIDI_ev_type type;
	unsigned char channel;
	unsigned char data[4]; //Note: may need to make this bigger if we want to use sysex messages to configure the voices
} MIDI_ev;

typedef struct {
	unsigned format;
	unsigned divisions; //ticks per quarter note
	unsigned tracks;
	
	//...
    //A bunch of stuff used internally. For example, there are things like the
    //state variables for each track being played
} MIDI;
```

When writing a program to read a MIDI file, you first initialize a MIDI context struct by pointing it at a file (this syntax is inspired by the FILE I/O functions in the standard C library):

```c
#include "midi.h"
MIDI *m = midi_open("songs/cotb.mid");
//your code here...	
midi_close(m);
```

The MIDI context struct keeps a list of pending MIDI events. You can access them one by one using the `getEvent` function:

```c
MIDI *m = midi_open("something");

//...
step_ticks(m, 1); //Explained below
//...

MIDI_ev *ev;
while(getEvent(m, &ev) != 0) {
	//use ev->type, ev->channel, and ev->data to do things...
}

//...

midi_close(m);

```

When you first initialize a MIDI context struct, it has no events stored in its queue. In order to get it to read events from the file, you need to tell it to step time. The `step_ticks` function does two things:

1. Discards all unread events in the queue
2. Fills the queue with any events that are scanned during the next stretch of time

For example, say you have a MIDI file with two NOTE_ON events at time 10, a NOTE_OFF event at time 20, and another NOTE_OFF event at time 30. Calling `step_ticks(m,25)` causes the two NOTE_ON events and the first NOTE_OFF event to be stored in `m`'s internal event queue. If you only call `getEvent(m, &ev)` twice, there will still be one event left over in `m`'s internal queue. If you then call `step_ticks(m,10)`, that event is discarded, and the second NOTE_OFF event is added to the internal queue.



### Other details

The `step_ticks` function returns an integer:
- If it is negative, an error occurred, and it is probably time to abort
- If it is zero, that means we have finished reading the file
- If it is positive, there was no error, but there are still more events left

Because of what I'm doing with this code, I added a "feature" where, within the event queue, NOTE_OFF events are always placed at the start.

SYSEX events are completely ignored. In addition, all meta-events (except the tempo change event) are also ignored.

The data field of a MIDI event is filled in the same order that the bytes appear in the MIDI file

#ifndef MIDI_H
#define MIDI_H 1

#include <stdio.h>

#define MAX_TRACKS 128
#define MAX_EVENTS 128


#define EVENT_TYPES X(NOTE_ON), X(NOTE_OFF), X(PROGRAM_CHANGE), X(CONTROL_CHANGE), X(SYSEX), X(TEMPO_CHANGE), X(AFTERTOUCH), X(PITCH_WHEEL)

typedef enum {
#define X(x) x
EVENT_TYPES
#undef X
} MIDI_ev_type;

extern char *event_names[];

typedef struct {
	MIDI_ev_type type;
	unsigned char channel;
	unsigned char data[4]; //Note: may need to make this bigger if we want to use sysex messages to configure the voices
} MIDI_ev;

typedef struct {
	char *data; //Data read in from file
	int len; //Bytes in data
	
	unsigned format;
	unsigned divisions; //us per quarter note
	unsigned tracks;
	
	//For each track keep track of our position in the data,
	//the length of the data in the track, and the time left
	//until the next event
	//int num_tracks;
	int track_pos[MAX_TRACKS];
	int track_end[MAX_TRACKS];
	int deltas[MAX_TRACKS];
	unsigned char running_status[MAX_TRACKS];
	
	//Internal queue of events
	int num_ev; //Number of events in the array
	int ev_pos; //User's "cursor" into the events array
	MIDI_ev events[MAX_EVENTS];
} MIDI;

//Allocates and initializes a MIDI struct on success.
//Returns NULL on failure
MIDI *midi_open(char *filename);
void midi_close(MIDI *m);

//Tells MIDI object how many ticks have elapsed. Although I don't plan to use
//this technique, this would allow more complex user code to gracefulyl handle
//the case when a tick signal is missed
int step_ticks(MIDI *m, int ticks);

//Returns number of events read (i.e. 0 or 1)
int getEvent(MIDI *m, MIDI_ev **ev);
//Should enable you to write while(getEvent(m, &ev)) {do_something_with(ev);}

#endif

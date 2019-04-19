#include "midi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char *event_names[] = {
#define X(x) #x
	EVENT_TYPES
#undef X
};

//These are helper functions to read big-endian values from files
//Is ARM little or big endian?
static unsigned endiannize(unsigned in) {
	union {
		unsigned res;
		char byte[4];
	} un;
	
	un.res = in;
	char tmp = un.byte[0];
	un.byte[0] = un.byte[3];
	un.byte[3] = tmp;
	
	tmp = un.byte[1];
	un.byte[1] = un.byte[2];
	un.byte[2] = tmp;
	
	return un.res;
}

static int readUint32(FILE *fp, unsigned *result) {
	unsigned len;
	fread(&len, sizeof(unsigned), 1, fp);		
	if (feof(fp)) {
		puts("Invalid format (A)");
		return -1;
	}
	
	//Fix that damned endianness
	*result = endiannize(len);
	return 0;
}

static int readUint16(FILE *fp, unsigned *result) {	
	unsigned tmp = 0;
	void *hacky_ptr = (void *)(((char *)&tmp)+2);
	fread(hacky_ptr, 2, 1, fp);
	if (feof(fp)) {
		puts("Invalid format (B)");
		return -1;
	}
	*result = endiannize(tmp); //Hack: can use same endiannize function for
	//16 bit ints as well.
	return 0;
}

//Reads a variable length quantity (as defined by the midi standard) into the
//value pointed at by res
//It's not used anymore, but the old version of this function returned the
//number of bytes read. I saw no reason to remove it
static int readVarLength(MIDI *m, int track_no, unsigned *res) {
	char c;
	*res = 0;
	
	int bytes = 0;
	while (1) {
		if (m->track_pos[track_no] == m->len) {
			puts("Unexpected EOF");
			return -1;
		}
		c = m->data[m->track_pos[track_no]++];
		
		char toAdd = c & 0b01111111;
		*res <<= 7;
		*res |= toAdd;
		bytes++;
		if (bytes == 5) {
			puts("Error: variable length quantity is too long");
			return -1;
		}
		
		if (toAdd == c) break; //Done reading number
	}
	
	return bytes;
}


//Read MIDI format number, number of tracks, and divisions per quarter note
//into correct places in MIDI struct. Returns negative on error
static int parseHeader(FILE *fp, MIDI *m) {	
	m->data = NULL;
	
	unsigned len;
	if (readUint32(fp, &len) < 0) goto parseHeader_failed;
	
	//printf("\tsize = %u\n", len);
	
	if (readUint16(fp, &(m->format)) < 0) goto parseHeader_failed;
	//printf("\tGot format %x\n", format);
	
	if (readUint16(fp, &(m->tracks)) < 0) goto parseHeader_failed;
	//printf("\tGot tracks %u\n", tracks);
	
	if (readUint16(fp, &(m->divisions)) < 0) goto parseHeader_failed;
	//printf("\tGot divisions %u\n", divisions);
	
	if (len != 6) {
		fseek(fp, len - 6, SEEK_CUR);
	}
	
	//Now: read in the rest of the file
	int pos = ftell(fp);
	fseek(fp, 0, SEEK_END);
	int end = ftell(fp);
	
	int bytes = (end - pos); //Number of bytes left in the file
	m->len = bytes;
	fseek(fp, pos, SEEK_SET); //Reset to where we were before
	
	m->data = malloc(bytes);
	fread(m->data, 1, bytes, fp);
	
	return 0;
	
	parseHeader_failed:
	if (m->data != NULL) free(m->data);
	puts("Header could not be parsed");
	return -1;
}

//Enters the correct values into the MIDI struct m for this track number
//Returns negative on error
//Notably, sets the position into the file's data
static int parseTrack(MIDI *m, int track_no, int *pos) {	
	if (*pos > m->len - 4) goto parseTrack_failed;
	
	//This could be more efficient, but whatever
	union {
		char byte[4];
		unsigned res;
	} un;
	
	//Read in little-endian order
	un.byte[3] = m->data[(*pos)++];
	un.byte[2] = m->data[(*pos)++];
	un.byte[1] = m->data[(*pos)++];
	un.byte[0] = m->data[(*pos)++];
	
	printf("Track %d len = %u\n", track_no, un.res);
	
	m->track_end[track_no] = un.res + *pos;
	m->track_pos[track_no] = *pos;
	
	//Initialize the first track delta
	unsigned dt;
	int tmp = readVarLength(m, track_no, &dt);
	if (tmp < 0) goto parseTrack_failed;
	
	m->deltas[track_no] = dt;
	
	*pos += un.res;
	
	return 0;
	
	parseTrack_failed:
	puts("Could not parse track");
	return -1;
}

//Parses a MIDI meta-event. Returns negative on error
static int readFFmessage(MIDI *m, int track_no) {
	//FF message types to care about:
	//type = 
	//	0x51: Tempo. Three bytes, representing microseconds per quarter note
	//	Also remember the divisions member of the header, which is number of ticks per quarter note
	
	unsigned char type = m->data[m->track_pos[track_no]++];
	
	unsigned len;
	int tmp = readVarLength(m, track_no, &len);
	if (tmp < 0) {
		puts("Invalid meta-event");
		return -1;
	}
	
	if (m->track_pos[track_no] + len > m->track_end[track_no]) {
		puts("Unexpected EOF while reading meta-event");
		return -1;
	}
	
	if (type == 0x51) {
		//This is the only meta-event we care about (I think)
		MIDI_ev *ev = m->events + m->num_ev++;
		ev->type = TEMPO_CHANGE;
		//Now read the three-byte tempo quantity, and fix endianness as we go
		ev->data[3] = m->data[m->track_pos[track_no]++];
		ev->data[2] = m->data[m->track_pos[track_no]++];
		ev->data[1] = m->data[m->track_pos[track_no]++];
		ev->data[0] = 0; 
		//HACK: By doing it like this, someone reading the event can just treat
		//the first four bytes of the event data as an unsigned.
	} else {
		printf("\tSkipping %d byte FF message of type %x ", len, +type & 0xFF);
		while (len --> 0) {
			unsigned char c;
			c = m->data[m->track_pos[track_no]++];
			printf("%x ", (+c)&0xFF);
		}
		printf("\n");
	}
	
	return 0;
}

//TODO: finish conversion to new paradigm
//Only thing left is to decide how to add these events into the event queue.
//For now I'll keep printing them out until I'm ready
static int readEvent(MIDI *m, int track_no) {
	unsigned char status;
	
	if (m->track_pos[track_no] == m->track_end[track_no]) {
		puts("Unexpected EOF while reading event status code");
		return -1;
	}
	status = m->data[m->track_pos[track_no]++];
	
	//Handle MIDI running status rule
	if ((status & 0x80) == 0) {
		status = m->running_status[track_no];
		m->track_pos[track_no]--;
	}
	
	
	//Compiler should be smart enough to remove redundant *(&x) construct.
	//Even if it isn't who cares? We're not really limited by speed
	#define readByte(x) if (m->track_pos[track_no] == m->track_end[track_no]) {\
		puts("Unexpected EOF while reading event");\
		return -1;\
	}\
	*(x) = (unsigned char) m->data[m->track_pos[track_no]++]
	
	switch(status >> 4) {
		case 8: {
			//printf("\tNote OFF on channel %d", status &0xF);
			//unsigned key;
			//unsigned vel;
			//readByte(&key);
			//readByte(&vel);
			//printf(", key = %u, vel = %u\n", key, vel);
			MIDI_ev *ev = m->events + m->num_ev++;
			ev->type = NOTE_OFF;
			ev->channel = status &0xF;
			readByte(ev->data); //key
			readByte(ev->data + 1); //vel
			break;
		}
		case 9: {
			//printf("\tNote ON on channel %d", status &0xF);
			//unsigned key;
			//unsigned vel;
			//readByte(&key);
			//readByte(&vel);
			//printf(", key = %u, vel = %u\n", key, vel);
			MIDI_ev *ev = m->events + m->num_ev++;
			unsigned char key;
			unsigned char vel; //Special case: some MIDI files write
			readByte(&key);
			readByte(&vel); //a vel of 0 to indiciate NOTE OFF
			ev->type = (vel != 0) ? NOTE_ON : NOTE_OFF;
			ev->channel = status &0xF;
			ev->data[0] = key;
			ev->data[1] = vel;
			break;
		}
		case 10: {
			//printf("\tNote AFTERTOUCH(?) on channel %d", status &0xF);
			//unsigned key;
			//unsigned vel;
			//readByte(&key);
			//readByte(&vel);
			//printf(", key = %u, vel = %u\n", key, vel);
			MIDI_ev *ev = m->events + m->num_ev++;
			ev->type = AFTERTOUCH;
			ev->channel = status &0xF;
			readByte(ev->data); //key
			readByte(ev->data + 1); //vel
			break;
		}
		case 11: {
			//printf("\tCONTROL CHANGE on channel %d", status &0xF);
			//unsigned ctl;
			//unsigned val;
			//readByte(&ctl);
			//readByte(&val);
			//printf(", ctl = %u, val = %u\n", ctl, val);
			MIDI_ev *ev = m->events + m->num_ev++;
			ev->type = CONTROL_CHANGE;
			ev->channel = status &0xF;
			readByte(ev->data); //ctl
			readByte(ev->data + 1); //val
			break;
		}
		case 12: {
			//printf("\tPROGRAM CHANGE on channel %d", status &0xF);
			//unsigned prog;
			//readByte(&prog);
			//printf(", prog = %u\n", prog);
			MIDI_ev *ev = m->events + m->num_ev++;
			ev->type = PROGRAM_CHANGE;
			ev->channel = status &0xF;
			readByte(ev->data); //prog
			break;
		}
		case 13: {
			//printf("\tNote AFTERTOUCH(?) on channel %d", status &0xF);
			//unsigned vel;
			//readByte(&vel);
			//printf(", vel = %u\n", vel);
			MIDI_ev *ev = m->events + m->num_ev++;
			ev->type = AFTERTOUCH;
			ev->channel = status &0xF;
			readByte(ev->data); //vel
			break;
		}
		case 14: {
			//printf("\tPITCH WHEEL CHANGE on channel %d", status &0xF);
			//unsigned lo;
			//unsigned hi;
			//readByte(&lo);
			//readByte(&hi);
			//printf(", lo = %u, hi = %u\n", lo, hi);
			MIDI_ev *ev = m->events + m->num_ev++;
			ev->type = PITCH_WHEEL;
			ev->channel = status &0xF;
			readByte(ev->data); //lo
			readByte(ev->data + 1); //hi
			break;
		}
		case 15: {
			int canary = 0;
			switch(status & 0xF) {
				case 15: {
					int tmp = readFFmessage(m, track_no);
					if (tmp < 0) {
						puts("Could not parse event");
						return -1;
					}
					break;
				}
				case 1:
				case 4:
				case 5:
					printf("\tUndefined code [%u]\n", +status & 0xFF);
					break;
				case 2: {
					unsigned lo;
					unsigned hi;
					readByte(&lo);
					readByte(&hi);
					printf("\tSong position pointer, lo = %u, hi = %u (ignored)\n", hi, lo);
					break;
				}
				case 3: {
					unsigned song;
					readByte(&song);
					printf("\tSong select, song = %u (ignored)\n", song);
					break;
				}
				case 0: {
					//TODO: Maybe use these inside MIDI files to configure the synth?
					printf("\tSysex message (ignored)\n");
					unsigned code = 0xF7;
					do {
						readByte(&code);
					} while (code != 0xF7);
					break;
				}
				default:
					//Something weird happened
					canary = 1;
			}
			if (canary == 0) break;
		}
		default:
			//This should never happen
			printf("Unknown status code (%x)\n", (+status)&0xFF);
			return 0;
	}
	
	m->running_status[track_no] = status; //For implementing running status rule
	return 0;
}

//Skip over an unrecognized chunk type. Returns negative on error
static int skipChunk(MIDI *m, int *pos) {	
	puts("Unrecognized chunk; skipping...");
	if (*pos > m->len - 4) {
		return -1;
	}
	
	//This could be more efficient, but whatever
	union {
		char byte[4];
		unsigned res;
	} un;
	
	//Read in little-endian order
	un.byte[3] = m->data[(*pos)++];
	un.byte[2] = m->data[(*pos)++];
	un.byte[1] = m->data[(*pos)++];
	un.byte[0] = m->data[(*pos)++];
	
	printf("Skpping %u bytes...\n", un.res);
	*pos += un.res;
	
	return 0;
}

MIDI *midi_open(char *filename) {
	char type[5];
	type[4] = 0; //NUL-terminate
	MIDI *ret = NULL;
	
	FILE *fp = NULL;
	fp = fopen(filename, "rb");
	if (fp == NULL) {
		char line[80];
		sprintf(line, "Could not open file %s", filename);
		perror(line);
		goto midi_open_failed;
	}
	
	fread(type, sizeof(char), 4, fp);
	if (feof(fp)) {
		goto midi_open_failed;
	}
	//printf("Got type %s\n", type);
		
	if(!strcmp(type, "MThd")) {
		ret = malloc(sizeof(MIDI));
		if (parseHeader(fp, ret) < 0) {
			puts("Bad header");
			goto midi_open_failed;
		}
	} else {
		puts("Bad MIDI file format (could not find header)");
		goto midi_open_failed;
	}
	
	//By now we're done with the file
	fclose(fp);
	fp = NULL;
	
	//Now: set up all the tracks
	int i;
	int pos = 0;
	for (i = 0; i < ret->tracks; i++) {
		int j;
		for (j = 0; j < 4; j++) {
			type[j] = ret->data[pos++];
		}
		if (!strcmp(type, "MTrk")) {
			if (parseTrack(ret, i, &pos) < 0) {
				puts("Bad track");
				goto midi_open_failed;
			}
		} else {
			if (skipChunk(ret, &pos) < 0) {
				puts("This file is totally uninterpretable by this program!");
				goto midi_open_failed;
			} else {
				i--;
			}
		}
	}
	
	return ret;
	
	midi_open_failed:
	if (ret != NULL) free(ret);
	if (fp != NULL) fclose(fp);
	return NULL;
}

void midi_close(MIDI *m) {
	if (m == NULL) return;
	free(m->data);
	free(m);
}

//Steps all the deltas. If any go zero or negative, that means we need to add
//this event to the internal queue. At the end we'll reorder the queue so that
//note OFF events always occur first
//Returns positive if the file is not finished
//Returns 0 if file is finished
//Returns negative on error
int step_ticks(MIDI *m, int ticks) {
	m->num_ev = 0; //Empty the internal event queue.
	m->ev_pos = 0;
	//If the user has unread events, too bad! By stepping time they have
	//declared that they want to ignore them
	
	int finished = m->tracks;
	
	int i;
	for (i = 0; i < m->tracks; i++) {
		if (m->track_pos[i] >= m->track_end[i]) {
			finished--; //This track is finished
			continue;
		}
		m->deltas[i] -= ticks;
		if (m->deltas[i] <= 0) {
			while (m->track_pos[i] < m->track_end[i]) {
				int tmp = readEvent(m, i);
				if (tmp < 0) {
					printf("Error reading event in track %d\n", i);
					return -1;
				}
				
				//At this point, we have reached the end of the track
				//and there is no dt left to read. We will exit this
				//function gracefully if that is the case
				
				if (m->track_pos[i] >= m->track_end[i]) {
					finished--;
					break;
				}
				
				unsigned dt;
				tmp = readVarLength(m, i, &dt);
				if (tmp < 0) {
					printf("Error reading dt in track %d\n", i);
					return -1;
				}
				//printf("dt = %4u\n", dt);
				
				if (dt != 0) { //Done reading events at this time
					m->deltas[i] += dt;
					break;
				}
			}
		}
	}
	
	//TODO: reorder event queue so NOTE_OFF events happen first
	return finished;
}

//Returns number of events read (i.e. 0 or 1)
//Should enable you to write while(getEvent(m, &ev)) {do_something_with(ev);}
//Returns negative on error
int getEvent(MIDI *m, MIDI_ev **ev) {
	if (m->ev_pos == m->num_ev) {
		m->ev_pos = 0;
		return 0;
	}
	
	*ev = m->events + m->ev_pos++;
	return 1;
}

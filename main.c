#include <fcntl.h> //open
#include <unistd.h> //close
#include <stdio.h> 
#include <string.h> //perror
#include <errno.h> 
#include <sys/mman.h> //mmap
#include <time.h>
#include <signal.h>
#include "midi.h" //midi stuff

#define FLOAT_AT(x) *(float *)(((char *) map_base) + (x))

int fd = -1;
void *map_base = MAP_FAILED;

int setupMap() {	
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd == -1) {
		perror("Could not open /dev/mem");
		return -1;
	}
	
	map_base = mmap(0, 0x80000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80000000);
	if (map_base == MAP_FAILED) {
		perror("Could not perform mmap");
		return -1;
	}
	
	return 0;
}

volatile int calls = 0;

void signalHandler(int s) {
	calls++;
	return;
}


timer_t mytimer;
int timerset = 0;
struct itimerspec interval = {
	.it_interval = {.tv_sec = 0, .tv_nsec = 1953125},
	.it_value = {.tv_sec = 0, .tv_nsec = 1953125}
};
struct itimerspec stop = {
	.it_interval = {.tv_sec = 0, .tv_nsec = 0},
	.it_value = {.tv_sec = 0, .tv_nsec = 0}
};

int startTiming() {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	
	struct sigaction act = {
		.sa_flags = 0,
		.sa_mask = set,
		.sa_handler = signalHandler
	};
	
	if (sigaction(SIGALRM, &act, NULL) < 0) {
		perror("Could not register handler");
		return -1;
	}
	
	
	if (timer_create(CLOCK_MONOTONIC, NULL, &mytimer) < 0) {
		perror("Could not init timer");
		return -1;
	}
	
	if(timer_settime(mytimer, 0, &interval, NULL) < 0) {
		perror("Could not start timer");
		return -1;
	}
	
	timerset = 1;
	return 0;
}

int endTiming() {
	timer_settime(mytimer, 0, &stop, NULL);
	return 0;
}

float note_freqs[] = {
	#include "freqs.txt"
};

unsigned saw_offs[] = {
	0x1000,
	0x4000,
	0x5000,
	0x6000
};
int notes[4] = {-1, -1, -1, -1};
int hist[4] = {0, 0, 0, 0};

void doEvent(MIDI_ev *ev) {
	/*printf("Event type = %s\n", event_names[ev->type]);
	if (ev->type == NOTE_ON || ev->type == NOTE_OFF) {
		printf("\tkey = %u, vel = %u\n", +ev->data[0] & 0xFF, + ev->data[1] & 0xFF);
	}*/
	if (ev->type == NOTE_ON) {
		//First check if this note is already on, while also scanning for 
		//first free voice
		unsigned char key = ev->data[0];
		float freq = note_freqs[key];
		float vel = (float) (+ev->data[1]&0xFF) / 255.0f;
		
		int i;
		int ind = -1;
		for (i = 0; i < 4; i++) {
			if (notes[i] == key) {
				//Note is already on. This is probably a change in velocity
				printf("\tChanging vel of active note on saw %d. Offset = %x\n", i, saw_offs[i] + 0x18);
				FLOAT_AT(saw_offs[i] + 0x18) = vel;
				return;
			} else if (notes[i] == -1) ind = i;
		}
		
		//At this point, we know we have to allocate a voice for this note.
		//If ind is negative, we'll delete the oldest note
		if (ind < 0) ind = hist[3]; //Note: if ind is negative, there's
		//no way hist[3] has -1 in it
		
		notes[ind] = key;
		printf("\tStarting new note on saw %d. Offset = %x\n", ind, saw_offs[ind] + 0x10);
		FLOAT_AT(saw_offs[ind] + 0x10) = freq;
		FLOAT_AT(saw_offs[ind] + 0x18) = vel;
		//Update the history (so we'll always know which note is the oldest)
		hist[3] = hist[2];
		hist[2] = hist[1];
		hist[1] = hist[0];
		hist[0] = ind;
	} else if (ev->type == NOTE_OFF) {
		unsigned char key = ev->data[0];
		//Find the note which has this key pressed and turn it off
		int i;
		for (i = 0; i < 4; i++) {
			if (notes[i] == key) {
				printf("\tTurning off note on saw %d. Offset = %x\n", i, saw_offs[i] + 0x18);
				FLOAT_AT(saw_offs[i] + 0x18) = 0.0f;
				notes[i] = -1;
				return;
			}
		}
	}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		puts("Usage: midi FILE NANOSECONDS_PER_TICK");
		goto cleanup;
	}
	
	char file[80];
	int tmp = sscanf(argv[1], "%s", file);
	if (tmp < 1) {
		printf("Could not parse string value [%s]\n", argv[1]);
		goto cleanup;
	}
	
	int nsec;
	tmp = sscanf(argv[2], "%d", &nsec);
	if (tmp < 1) {
		printf("Could not parse integer value [%s]\n", argv[2]);
		goto cleanup;
	}
	
	if(setupMap() < 0) {
		puts("Aborting...");
		goto cleanup;
	}
	
	interval.it_interval.tv_nsec = nsec;
	interval.it_value.tv_nsec = nsec;
	
	startTiming();
	puts("Timing started");
	
	MIDI *m = midi_open(file);
	//int i = 0;
	if (m != NULL) {
		//TODO: fix addresses in address editor so I can more easily
		//automatically handle the mixer in this program	
		
		printf("Format = %d\n", m->format);
		while (1) {
			pause();
			tmp = step_ticks(m, 1);
			if (tmp < 0) {
				puts("An error occurred while stepping through the MIDI file");
				break;
			} else if (tmp == 0) {
				puts("File finished reading succesfully");
				break;
			}
			
			MIDI_ev *ev;
			while (getEvent(m, &ev)) {
				doEvent(ev);
			}
		}
	} else {
		puts("MIDI file not read");
	}
	midi_close(m);
	
	puts("Success");
	
	cleanup:
	printf("%d calls to handler\n", calls);
	puts("Cleaning up...");
	if (fd != -1) close(fd);
	if (map_base != MAP_FAILED) munmap(map_base, 0x80000);
	if (timerset) endTiming();
}

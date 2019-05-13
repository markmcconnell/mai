#include "mai.h"
#include <samplerate.h>

/* ######################################################################## */
static jack_client_t	 *jack_client;		// jack client handle
static jack_port_t	 *jack_port[8];		// jack port handles
static char              *jack_name[8];		// jack port names (in client:name format)

static int64_t		  jack_error = 0;	// clock error: -=jack too fast, +=jack too slow

/* ######################################################################## */
static int jack_bias(uint32_t frames) {
	static       uint32_t counter = 0;
	static const uint32_t trigger = 10000;
	
	int bias = 0;
	
	if ((counter += frames) >= trigger) {
		counter -= trigger;
		
		     if (jack_error < 0) bias = -1;
		else if (jack_error > 0) bias =  1;
		
		MAI_STAT_ADD(audio.drift, bias);
		__sync_fetch_and_sub(&jack_error, bias);
	}
	
	return(bias);
}

/* ######################################################################## */
static int jack_send(jack_nframes_t frames, void *arg __attribute__((__unused__))) {
	const size_t channels = mai.args.channels;			// channels
	const size_t buflen   = (frames+1) * channels * sizeof(float);	// length of interleaved samples
	
	float *input, *output, *buffer = alloca(buflen);		// interleaved samples
	
	// match network clock rate
	int bias = jack_bias(frames);
	
	for (uint32_t ch=channels; ch--; ) {				// for all ports/channels:
		if ((input = jack_port_get_buffer(jack_port[ch], frames)) == NULL)
			continue;
			
		const float * const end = input + frames;
		
		output = &buffer[ch];					// offset interleave output pointer
			
		if (bias == 1) {					// too slow: add a sample to frame
			*output  = input[0];				// output[0] = input[0]
			 output += channels;
			*output  = (input[0]+input[1])/2;		// output[1] = avg(input[0..1])
			 output += channels;
			 
			 input  += 1;					// output[2] = input[1], ...
			 
		} else if (bias == -1) {				// too fast: drop a sample from frame
			*output  = (input[0]+input[1])/2;		// output[0] = avg(input[0..1])
			 output += channels;
			 
			 input  += 2;					// output[1] = input[2], ...
		}
		
		for (; input < end; output += channels)			// for all samples in interleave
			*output = *input++;				// copy sample
	}
	
	mai_audio_write(buffer, frames+bias);				// send audio to RTP
	return(0);
}

/* ######################################################################## */
static int jack_recv(jack_nframes_t frames, void *arg __attribute__((__unused__))) {
	const size_t channels = mai.args.channels;			// channels
	const size_t buflen   = (frames+1) * channels * sizeof(float);	// length of interleaved samplesn
	
	float *input, *output, *buffer = alloca(buflen);		// interleaved samples
	
	// match network clock rate
	int bias = jack_bias(frames);
	
	mai_audio_read(buffer, frames+bias);				// try to get samples from buffer
		
	for (uint32_t ch=channels; ch--; ) {				// for all ports/channels:
		input = &buffer[ch];					// offset interleave input pointer
		
		if ((output = jack_port_get_buffer(jack_port[ch], frames)) == NULL)
			continue;
			
		const float * const end = output + frames;
			
		if (bias == 1) {					// too slow: drop a sample from frame
			*output  = (input[0] + input[channels])/2;	// output[0] = avg(deinterleave(input[0..1]))
			 output += 1;
			
			input += (channels * 2);			// make output[1] = input[2], ...
			
		} else if (bias == -1) {				// too fast: add a sample to frame
			*output  = input[0];				// make output[0] = input[1], ...
			 output += 1;
			 
			*output  = (input[0] + input[channels])/2;	// output[1] = avg(deinterleave(input[0..1]))
			 output += 1;
			 
			input += channels;				// make output[2] = input[1], ...
		}
		
		for (; output < end; input += channels)			// for all samples in interleave
			*output++ = *input;				// copy sample
	}
	return(0);
}

/* ######################################################################## */
void mai_jack_clock(int64_t ptp_now) {
	static int64_t jack_last = 0;
	static int64_t ptp_last  = 0;
	
	int64_t jack_now  = jack_frame_time(jack_client);
	
	int64_t jack_diff = jack_now - jack_last;
	int64_t ptp_diff  = ptp_now  - ptp_last;
	
	jack_last = jack_now;
	ptp_last  = ptp_now;
	
	// ptp=100, jack=101, diff=-1 -- jack too fast
	// ptp=101, jack=100, diff=+1 -- jack too slow
	int64_t error = ptp_diff - jack_diff;
	
	// during XRUNs and other NTP/PTP events the jack or PTP sample
	// clock can have large non-linear jumps;  since we're only
	// interested in preventing small sample rate drift and because RTP
	// has it's own correction mechanism,  we filter large errors out
	if ((error < -16) || (error > 16))
		return;
		
	// jack_error is shared with the process callback threads
	__sync_fetch_and_add(&jack_error, error);
}

/* ######################################################################## */
int mai_jack_init(void) {
	// set realtime scheduling with highest priority
	struct sched_param p = (struct sched_param){ .sched_priority = 99 };
	
	if (sched_setscheduler(0, SCHED_RR, &p))
		return(mai_error("could not set realtime scheduler: %m\n"));
		
	// change process privileges
	if (mai.args.gid && (setgid(mai.args.gid) || setegid(mai.args.gid)))
		return(mai_error("could not change to group(%d): %m\n", mai.args.gid));
	
	if (mai.args.uid && (setuid(mai.args.uid) || seteuid(mai.args.uid)))
		return(mai_error("could not change to user(%d): %m\n", mai.args.uid));
		
	// setup jack and jack process buffer
        if ((jack_client = jack_client_open(mai.args.client, JackNoStartServer, NULL)) == NULL)
        	return(mai_error("could not connect to jack server.\n"));
        	
	// initialize the audio and clock system with jack sample rate all at once
	if (mai_audio_init(mai_ptp_rate(jack_get_sample_rate(jack_client))))
		return(-1);
        	
	mai_audio_size(jack_get_buffer_size(jack_client));
	
	// setup ports
	mai.args.client = jack_get_client_name(jack_client);
	
	const size_t noff  = strlen(mai.args.client) + 1;
	const long   flags = MAI_SENDER ? JackPortIsInput : JackPortIsOutput;
	
	for (uint32_t ch=0; ch < mai.args.channels; ch++) {
		// generate full system:port name
		if (asprintf(&jack_name[ch], "%s:%s_%d", mai.args.client, (MAI_SENDER ? "in" : "out"), ch+1) <= 0)
			return(mai_error("could not allocate jack port name: %m\n"));
			
		// register only the port name with jack (name + off)
		if ((jack_port[ch] = jack_port_register(jack_client, (jack_name[ch]+noff), JACK_DEFAULT_AUDIO_TYPE, flags, 0)) == NULL)
			return(mai_error("could not create jack port: %s\n", jack_name[ch]));
	}
		
	// set process callback and activate it
	if (jack_set_process_callback(jack_client, MAI_SENDER ? jack_send : jack_recv, NULL))
		return(mai_error("could not set jack process callback.\n"));
		
	// activate the client
	jack_activate(jack_client);
	
	// connect ports specified on command line
	const char *pair = mai.args.ports;
	
	for (uint32_t ch=0; pair && *pair && (ch < mai.args.channels); ch++) {
		char *end = strchrnul(pair, ',');		// get separation char or ptr to '\0'

		if (*end) *end++ = 0;				// if separator, change to '\0' and advance ptr
		
		if (pair[0] && (pair[0] != '-')) {
			int rc;
			
			if (MAI_SENDER) rc = jack_connect(jack_client, pair, jack_name[ch]);
			else            rc = jack_connect(jack_client, jack_name[ch], pair);
			
			if (rc)
				mai_error("failed to connect %s to specified port %s!\n", jack_name[ch], pair);
				
			mai_debug("Connected: %s <-> %s\n", jack_name[ch], pair);
		}
		pair = end;					// point to next pair or '\0'
	}

	return(mai_debug("Started: %s (%d channels)\n", mai.args.client, mai.args.channels));
}

/* ######################################################################## */

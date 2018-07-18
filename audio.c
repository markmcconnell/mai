#include "mai.h"
#include <samplerate.h>

/* ######################################################################## */
static jack_ringbuffer_t	 *buf;			// rtp/jack ipc audio buffer
static size_t			  buf_frames;		// frames in buffer
static size_t			  buf_stride;		// channels * sizeof(float)

static pthread_cond_t 		  buf_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t		  buf_lock = PTHREAD_MUTEX_INITIALIZER;

static SRC_STATE		 *src	    = NULL;	// sample rate converter
static double			  src_ratio = 1.0;	// output / input ratio
static int			  src_mult  = 1;	// integer ratio for buffer scaling

static float 			  cvt_min;		// minimum integer sample value
static float 			  cvt_max;		// maximum integer sample value
static float			  cvt_scale;		// dither random noise scale
static float			  cvt_dither[8][4];	// per channel dither values

static size_t			  cvt_unit;		// output bytes (bits / 8)

static float 			(*cvt_int)(const uint8_t *);
static void 			(*cvt_float)(uint8_t *, int32_t);

/* ######################################################################## */
static float cvt_int32(const uint8_t *in) {
	int32_t raw = (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];
	return(raw / cvt_max);
}

static float cvt_int24(const uint8_t *in) {
	int32_t raw = (in[0] << 16) | (in[1] << 8) | in[2];
	return(((raw > 8388608) ? (raw - 16777216) : raw) / cvt_max);
}

static float cvt_int16(const uint8_t *in) {
	int32_t raw = (in[0] << 8) | in[1];
        return(((raw > 32768) ? (raw - 65536) : raw) / cvt_max);
}

static inline float cvt_int_clip(const char *data) {
	float in = (*cvt_int)((const uint8_t *)data);
	return((in > 1.0f) ? 1.0f : ((in < -1.0f) ? -1.0f : in));
}

/* ######################################################################## */
static void cvt_float32(uint8_t *out, int32_t raw) {
	out[3] = raw & 0xFF; raw >>= 8;
	out[2] = raw & 0xFF; raw >>= 8;
	out[1] = raw & 0xFF; raw >>= 8;
	out[0] = raw & 0xFF;
}

static void cvt_float24(uint8_t *out, int32_t raw) {
	out[2] = raw & 0xFF; raw >>= 8;
	out[1] = raw & 0xFF; raw >>= 8;
	out[0] = raw & 0xFF;
}

static void cvt_float16(uint8_t *out, int32_t raw) {
	out[1] = raw & 0xFF; raw >>= 8;
	out[0] = raw & 0xFF;
}

/* ######################################################################## */
size_t mai_audio_write(const void *data, size_t frames) {
	// resample: ensure we consume all input frames in this process
        if (src) {
                const size_t oframes = frames * src_mult;
                
                SRC_DATA d = (SRC_DATA){
                        .input_frames  = frames,  .data_in   = (void *)data,     
                        .output_frames = oframes, .data_out  = alloca(oframes * buf_stride),
                        .end_of_input  = 0,       .src_ratio = src_ratio
                };
                
                if (src_process(src, &d))
                        return(0);
                        
		data   = d.data_out;
		frames = d.output_frames_gen;
	}
	
	// write as many whole frames as we can to the buffer
	size_t bytes = jack_ringbuffer_write_space(buf);
	
	if ((bytes -= bytes % buf_stride) == 0) {
		MAI_STAT_INC(audio.overrun);
		return(0);
	}
		
	// convert frame count to bytes, then limit check
	if ((frames *= buf_stride) < bytes)
		bytes = frames;
	
	bytes = jack_ringbuffer_write(buf, data, bytes);
	
	pthread_cond_signal(&buf_cond);
	return(bytes);
}

size_t mai_audio_write_int(const char *data, size_t bytes) {
	size_t samples = bytes / cvt_unit;
	size_t frames  = samples / mai.args.channels;
	
	float *out = alloca(samples * sizeof(float));
	float *ptr = out;
	
	for (size_t lp=samples; lp--; data += cvt_unit)
		*ptr++ = cvt_int_clip(data);
		
	return(mai_audio_write(out, frames));
}

/* ######################################################################## */
size_t mai_audio_read(void *data, size_t frames) {
	// read as many whole frames as we can from the buffer
	size_t avail = jack_ringbuffer_read_space(buf);
	size_t bytes = frames * buf_stride;
	
	if (avail < bytes) {
		MAI_STAT_INC(audio.underrun);
		
		memset(data, 0, bytes);
		return(0);
	}
	
	return(jack_ringbuffer_read(buf, data, bytes));
}

size_t mai_audio_read_int(char *data, size_t bytes) {
	size_t samples = bytes / cvt_unit;
	size_t buflen  = samples * sizeof(float);
	
	while (jack_ringbuffer_read_space(buf) < buflen) {
		pthread_mutex_lock(&buf_lock);
		pthread_cond_wait(&buf_cond, &buf_lock);
		pthread_mutex_unlock(&buf_lock);
	}
	
	float *in = alloca(buflen);
	jack_ringbuffer_read(buf, (void *)in, buflen);
	
	int32_t quant;
	float raw, rand, samp, *dither;
	
	for (size_t lp=0; lp < samples; data += cvt_unit) {
		dither = cvt_dither[lp++ % mai.args.channels];
	
		// scale then do noise shaping
		raw = (*in++ * cvt_max) + dither[0] - dither[1] + dither[2];
		
		// bias and dither
		rand = (drand48() - 0.5) * cvt_scale;
		samp = (raw + 0.5f) + (rand - dither[3]);
		
		// clip
		if (((samp > cvt_max) && (raw > (samp = cvt_max))) || ((samp < cvt_min) && (raw < (samp = cvt_min))))
			raw = samp;
		
		// update dither noise and error feedback
		dither[3] = rand;
		dither[2] = dither[1];
		dither[1] = dither[0] / 2;
		dither[0] = raw - (quant = nearbyintf(samp));
		
		(*cvt_float)((uint8_t *)data, quant);
	}
	return(bytes);
}

/* ######################################################################## */
size_t mai_audio_size(size_t frames) {
	// always use larger of double the rtp/jack frame sizes
	if ((frames *= 2) > buf_frames)
		buf_frames = frames;
		
	return(buf_frames);
}

/* ######################################################################## */
int mai_audio_init(size_t rate) {
	// setup resampler if rates don't match
	if (rate != mai.args.rate) {
		// ratio is output / input
		if (MAI_SENDER) src_ratio = ((double)mai.args.rate) / ((double)rate);
		else            src_ratio = ((double)rate) / ((double)mai.args.rate);
		
		// integer ratio size multipler
		src_mult = ceil(src_ratio);
		
		if ((src = src_new(SRC_SINC_FASTEST, mai.args.channels, NULL)) == NULL)
			return(mai_error("failed to create resample engine!"));
	}
	
	// create the audio ringbuffer
	buf_stride = mai.args.channels * sizeof(float);

	if ((buf = jack_ringbuffer_create(buf_stride * buf_frames)) == NULL)
		return(mai_error("failed to create audio ringbuffer!"));
		
	// ensure dither is zero
	memset(cvt_dither, 0, sizeof(cvt_dither));
	
	// minimum and maximum converted sample values
	cvt_max  = powf(2, (mai.args.bits - 1)) - 1.0f;
	cvt_min  = -cvt_max;
	
	// random dither scale factor (random input is -0.5 .. 0.5)
	cvt_scale = 4.0 / cvt_max;
	
	// converter stride factor
	cvt_unit   = mai.args.bits / 8;
	
	// choose coverters based upon bit depth
	if (mai.args.bits == 16) {
		cvt_float = cvt_float16;
		cvt_int   = cvt_int16;
	} else if (mai.args.bits == 24) {
		cvt_float = cvt_float24;
		cvt_int   = cvt_int24;
	} else {
		cvt_float = cvt_float32;
		cvt_int   = cvt_int32;
	}
	
	return(mai_debug("Format: %u-bit signed-integer\n", mai.args.bits));
}

/* ######################################################################## */

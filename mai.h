#ifndef __MAI_H
#define __MAI_H

#include "version.h"

#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <signal.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/if.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

extern struct mai {
	struct {
		const char		*client;	// jack client name
		const char		*ports;		// jack port connections
		
		const char		*session;	// sdp session name
		const char		*title;		// sdp session title
		
		const char		*addr;		// multicast address
		uint16_t		 port;		// multicast port
		
		int			 mode;		// 's' or 'r' for send|recv mode
		uint32_t		 bits;		// net audio: bits/sample
		uint32_t		 channels;	// net audio: channels/stream
		uint32_t		 rate;		// net audio: samples/second
		uint32_t		 ptime;		// net audio: microseconds/packet
			
		int			 uid;		// userid to switch to
		int			 gid;		// groupid to switch to
		
		int			 verbose;	// verbose output
	} args;
	
	struct {
		struct {
			ssize_t			drift;			// total sample clock drift
			size_t			overrun;		// buffer overrun
			size_t			underrun;		// buffer underrun
		} audio;
		
		struct {
			size_t			resynced;		// total rtp clock resyncs
			size_t			packets;		// total packets sent/recv
			size_t			reordered;		// packets received out of order
			size_t			skipped;		// packets we stopped waiting for
		} rtp;
		
		struct {
			size_t			masters;		// total ptp master clock changes
			size_t			requests;		// total ptp delay requests
			size_t			general;		// total ptp general messages
			size_t			event;			// total ptp event messages
		} ptp;
	} stat;
} mai;

/* ######################################################################## */
#define MAI_SENDER (mai.args.mode == 's')

#define MAI_STAT_GET(t)   (mai.stat.t)
#define MAI_STAT_ADD(t,v) (mai.stat.t += v)

#define MAI_STAT_INC(t) MAI_STAT_ADD(t,  1)
#define MAI_STAT_DEC(t) MAI_STAT_ADD(t, -1)

/* ######################################################################## */
#define mai_log(l,f, ...) fprintf(stderr, "[%-5s] %-20s " f, l, __func__ , ##__VA_ARGS__)

#define mai_debug(f, ...)    ({ if (mai.args.verbose) mai_log("DEBUG", f , ##__VA_ARGS__); 0; })

#define mai_info(f, ...)  ({ mai_log("INFO",  f , ##__VA_ARGS__);  0; })
#define mai_error(f, ...) ({ mai_log("ERROR", f , ##__VA_ARGS__); -1; })

/* ######################################################################## */
// args.c
extern void 		 mai_args_init(int argc, char *argv[]);

// audio.c
extern int		 mai_audio_init(size_t rate);
extern size_t		 mai_audio_size(size_t size);

extern size_t		 mai_audio_write(    const void *data, size_t frames);
extern size_t		 mai_audio_write_int(const char *data, size_t bytes);
extern size_t		 mai_audio_read(           void *data, size_t frames);
extern size_t		 mai_audio_read_int(       char *data, size_t bytes);

// jack.c
extern int		 mai_jack_init(void);
extern void		 mai_jack_clock(int64_t ptp);

// log.c
extern int 		 mai_log_str(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

// sap.c
extern int		 mai_sap_init( void);
extern int		 mai_sap_start(void);
extern int		 mai_sap_stop( void);

// sock.c
extern int  		 mai_sock_open(int mode, const char *ip, const uint16_t port);

extern int 		 mai_sock_if_set(const char *name);
extern size_t		 mai_sock_if_mtu(void);
extern const void 	*mai_sock_if_addr(void);
extern const char	*mai_sock_if_name(void);
extern void	 	 mai_sock_if_local(uint8_t *out);

// ptp.c
extern int		 mai_ptp_init( void);
extern int		 mai_ptp_start(void);
extern int		 mai_ptp_stop( void);

extern uint32_t		 mai_ptp_rate(uint32_t rate);
extern const char	*mai_ptp_source(void);

// rtp.c
extern int		 mai_rtp_init( void);
extern int		 mai_rtp_start(void);
extern int		 mai_rtp_stop( void);

extern uint64_t 	 mai_rtp_clock(void);
extern void 		 mai_rtp_offset(int64_t offset);

/* ######################################################################## */
#endif // __MAI_H

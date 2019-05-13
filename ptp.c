#include "mai.h"

/* ######################################################################## */
struct packet {
        uint8_t         type;
        uint8_t         version;
        uint16_t        length;
        uint8_t         domain;
        uint8_t         reserved0;
        uint16_t        flags;
        uint64_t        correction;
        uint32_t        reserved1;
        uint8_t         source[10];
        uint16_t        sequence;
        uint8_t         control;
        uint8_t         interval;
        uint8_t		payload[];
} __attribute__((__packed__));

/* ######################################################################## */
static char		ptp_source[32];		// PTP master source (decoded/text)

static int 		ptp_sock  = -1;		// port 319: event messages
static uint64_t		ptp_rate  =  0;		// audio system sample rate
static uint64_t         ptp_recv  =  0;   	// PTP SYNC Receiver  Timestamp (T'1)
static uint64_t         ptp_sync  =  0;   	// PTP SYNC Sender    Timestamp (T1)

static int		gen_sock  = -1;		// port 320: general messages
static uint16_t	 	clk_seq   =  0;		// PTP Two Phase SYNC Sequence
static uint64_t	 	clk_recv  =  0;		// PTP Two Phase SYNC Received

static int		req_sock  = -1;		// socket for sending messages
static uint16_t 	req_seq   =  0;		// request message sequence
static uint64_t		req_sent  =  0;		// PTP DELAY Sender   Timestamp (T2)
static uint64_t		req_sync  =  0;		// PTP DELAY Receiver Timestamp (T'2)

/* ######################################################################## */
static uint64_t ptp_stamp(uint8_t *in) {
	// 48bit seconds in network/msb order
	uint64_t sec = 	((uint64_t)in[0] << 40) | ((uint64_t)in[1] << 32) | ((uint64_t)in[2] << 24) | 
			((uint64_t)in[3] << 16) | ((uint64_t)in[4] <<  8) | ((uint64_t)in[5]);
			
	// 32bit nanoseconds in network/msb order
	uint32_t nsec =	((uint64_t)in[6] << 24) | ((uint64_t)in[7] << 16) | 
			((uint64_t)in[8] <<  8) | ((uint64_t)in[9]);
			
	// convert clock time to sample time
	return((sec * ptp_rate) + ((nsec * ptp_rate) / 1000000000));
}

/* ######################################################################## */
static void ptp_update(void) {
	// send delay requests only in sender mode and only every 2 seconds
	if (!MAI_SENDER || (req_sync > ptp_sync) || ((ptp_sync - req_sync) < (ptp_rate * 2)))
		return;
	
	// expected size of DELAY REQUEST packet (header + 48bits + 32bits)
	static const size_t pktlen = sizeof(struct packet) + ((48 + 32) / 8);
	
	struct packet *packet = alloca(pktlen);
	memset(packet, 0, pktlen);
	
	mai_sock_if_local(packet->source);	// PTP: Local Source
	
	packet->type     = 1;			// PTP: DELAY REQUEST
	packet->version  = 2;			// PTP: VERSION 2
	packet->length   = pktlen;		// PTP: Header + Body Length
	packet->sequence = ++req_seq;		// PTP: Expected Response Sequence
	
	if ((send(req_sock, packet, pktlen, 0)) <= 0)
		mai_error("send: %m\n");
		
	req_sent = mai_rtp_clock();		// set delay request time (T2)
	MAI_STAT_INC(ptp.requests);
}

/* ######################################################################## */
static void *ptp_general(void *arg) {
	// packet data buffer and header overlay
	uint8_t	type, data[2048];

	// state data
	struct packet *packet = (struct packet *)data;
	
	for (ssize_t r; 1; ) {
		if ((r = recv(gen_sock, data, sizeof(data), 0)) <= 0)
			mai_error("recv: %m\n");
			
		if (((packet->version & 0x0F) != 2) || (packet->domain != 0))
			continue;				// skip: PTP VERSION != 2 or PTP DOMAIN != 0
			
		MAI_STAT_INC(ptp.general);
			
		type = packet->type & 0x0F;
		
		if (type == 0x08) { 				// is this the second phase of a two-phase clock?
			if (packet->sequence != clk_seq)	// is this the right sequence?
				continue;
				
			ptp_recv = clk_recv;			// set received time (T'1)
			ptp_sync = ptp_stamp(packet->payload);	// set master time   (T1)
			
			ptp_update();
			
		} else if (type == 0x09) { 			// is this a delay response message?
			if (packet->sequence != req_seq)	// is this the right sequence?
				continue;
				
			req_sync = ptp_stamp(packet->payload);	// set master delay (T'2)
			
			// send calculated PTP offset to RTP system
			mai_rtp_offset(((int64_t)ptp_recv - (int64_t)ptp_sync - (int64_t)req_sync + (int64_t)req_sent) / 2);
		}
	}

	mai_error("Unexpected Thread Exit!");
	return(arg);
}

/* ######################################################################## */
static void *ptp_event(void *arg) {
	const uint16_t flag_two_step = htons(0x0200);
	
	// packet data buffer and header overlay
	uint8_t	data[2048];

	// state data
	struct packet 	    *packet = (struct packet *)data;
	static const size_t  pktlen = sizeof(*packet) + ((48 + 32) / 8);
	
	uint8_t source[sizeof(packet->source)];		// current PTP SYNC source
	memset(source, 0, sizeof(source));		// clear current source and initiate sync
	
	// receive packet loop
	for (ssize_t r; 1; ) {
		if ((r = recv(ptp_sock, data, sizeof(data), 0)) <= 0)
			mai_error("recv: %m\n");
			
		if (((packet->version & 0x0F) != 2) || (packet->domain != 0))
			continue;	// skip: PTP VERSION != 2 or PTP DOMAIN != 0
			
		MAI_STAT_INC(ptp.event);
			
		if (((size_t)r < pktlen) || (ntohs(packet->length) < pktlen))
			continue;	// skip: PTP LENGTH < (sizeof(header) + sizeof(SYNC))
			
		if ((packet->type & 0x0F) != 0)
			continue;	// skip: PTP TYPE != SYNC
			
		// check synchronization source
		if (memcmp(source, packet->source, sizeof(source))) {
			// we just got a SYNC from a different clock, start RESYNC
			memcpy(source, packet->source, sizeof(source));
			
			// save a string copy of clock source (for SAP/SDP broadcasts)
			sprintf(ptp_source, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:0", 
				packet->source[0], packet->source[1], packet->source[2], packet->source[3],
				packet->source[4], packet->source[5], packet->source[6], packet->source[7]
			);
			
			mai_info("Source: %s (#%zu).\n", ptp_source, MAI_STAT_INC(ptp.masters));
		}
		
		// convert ptp timestamp to clk sample stamp
		uint64_t stamp = ptp_stamp(packet->payload);
		
		// let jack adjust it's sample rate from ptp rate
		mai_jack_clock(stamp);
		
		if (packet->flags & flag_two_step) {	// is this a two-phase clock?
			clk_seq  = packet->sequence;	// save sequence
			clk_recv = mai_rtp_clock();	// save received time

		} else {				// otherwise, it's a single phase clock
			ptp_recv = mai_rtp_clock();	// set received time
			ptp_sync = stamp;		// set master time
			
			ptp_update();
		}
	}
	
	mai_error("Unexpected Thread Exit!");
	return(arg);
}

/* ######################################################################## */
static pthread_t evt_tid;
static pthread_t gen_tid;

int mai_ptp_init(void) {
	if ((ptp_sock = mai_sock_open('r', "224.0.1.129", 319)) < 0)
		return(mai_error("could not open PTP event socket\n"));
		
	if ((gen_sock = mai_sock_open('r', "224.0.1.129", 320)) < 0)
		return(mai_error("could not open PTP general socket\n"));
		
	if ((req_sock = mai_sock_open('s', "224.0.1.129", 319)) < 0)
		return(mai_error("could not open PTP message socket\n"));
		
	return(mai_debug("PTP Domain: 224.0.1.129 (0)\n"));
}

int mai_ptp_start(void) {
	// kick off ptp general messages thread
	if (pthread_create(&gen_tid, NULL, ptp_general, NULL))
		return(mai_error("could not start general thread: %m\n"));

	// kick off ptp thread
	if (pthread_create(&evt_tid, NULL, ptp_event, NULL))
		return(mai_error("could not start ptp thread: %m\n"));
        
	// wait for PTP to synchronize
	struct timespec ts;
	
	for (int count=1; !MAI_STAT_GET(ptp.masters); count++) {
		// loop banner
		if (!(count % 5))
			mai_info("Waiting.\n");
			
		// get current time
		if (clock_gettime(CLOCK_REALTIME, &ts))
			return(mai_error("could not get system clock: %m\n"));
			
		// try to join thread, 1s timeout
		ts.tv_sec  += 1;
		
		if (!pthread_timedjoin_np(evt_tid, NULL, &ts))
			return(mai_error("startup aborted.\n"));
			
		// loop overflow
		if (count > 60) {
			mai_error("Timeout.\n");
			return(mai_ptp_stop());
		}
	}
	
	return(0);
}

int mai_ptp_stop(void) {
	pthread_cancel(evt_tid);
	pthread_cancel(gen_tid);
	return(0);
}

/* ######################################################################## */
uint32_t mai_ptp_rate(uint32_t rate) {
	ptp_rate = rate;
	return(ptp_rate);
}

const char *mai_ptp_source(void) {
	return(ptp_source);
}

/* ######################################################################## */

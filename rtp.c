#include "mai.h"

/* ######################################################################## */
// rtp packet structure
struct packet {
	uint8_t		vpxcc;		// Version(2), P(1), X(1), CC(4)
	uint8_t		mpt;		// Marker(1), PT(7)
	uint16_t	seq;		// Packet Sequence Identifier
	
	uint32_t	time;		// Playback Timestamp
	uint32_t	ssrc;		// SSRC Identifier
	
	char		payload[];	// Payload Data
} __attribute__((__packed__));

/* ######################################################################## */
#define ROB_LEN 6					// reorder up to ROB_LEN packet

static int 			 rtp_sock = -1;		// rtp in/out socket
static uint16_t	 		 rtp_next =  0;		// next expected sequence number
static size_t			 rtp_used =  0;		// number of reorder entries used

static uint64_t			 rtp_clock = 0;		// rtp sample clock
static uint32_t			 rtp_samples;		// samples per packet

struct {
	uint16_t         len;
	uint16_t         seq;
	char             payload[8192];
} rob[ROB_LEN];

/* ######################################################################## */
static void rob_scan(void) {
	for (size_t idx, lp=0; rtp_used && (lp < ROB_LEN); lp++) {
		idx = rtp_next % ROB_LEN;				// get buffer index from sequence

		if (rob[idx].seq != rtp_next)				// stop scan: entry does not match
			return;
			
		mai_audio_write_int(rob[idx].payload, rob[idx].len);	// send entry to jack
		rtp_next += 1;						// check next sequence
		rtp_used -= 1;						// release current entry
	}
}

/* ######################################################################## */
static void *rtp_recv(void *arg) {
	uint8_t		 buffer[8192];				// packet data buffer
	struct packet	*packet = (struct packet *)buffer;	// packet structure overlay
	char		*data;					// variable pointer (to skip extensions)
	ssize_t		 len;					// variable data length
	
	// loop on ringbuffer and send samples
	while (1) {
		if ((len = recv(rtp_sock, buffer, sizeof(buffer), 0)) <= 0)
			mai_error("packet recv: %m\n");				// skip: receive error
			
		if ((len -= sizeof(*packet)) <= 0)
			continue;						// skip: no payload
			
		if ((packet->vpxcc & 0b11000000) != 0b10000000)
			continue;						// skip: bad version
			
		data = packet->payload;						// copy payload start
		data += (packet->vpxcc & 0b00001111) * sizeof(uint32_t);	// skip any CSRC's

		if (packet->vpxcc & 0b00010000)					// extension header?
			data += (1 + ntohs(*((uint16_t *)(data + 2)))) * sizeof(uint32_t);
			
		if ((len -= (data - packet->payload)) < 0)		// skip if no data
			continue;
			
		MAI_STAT_INC(rtp.packets);
			
		uint16_t seq      = ntohs(packet->seq);			// get packet sequence number
		 int16_t seq_dist = seq - rtp_next;			// distance from expected sequence
		uint16_t seq_abs  = abs(seq_dist);			// absolute distance
		
		if (seq_abs > (ROB_LEN * 2)) {				// distance too far out
			seq_abs  = 0;					// resynchronize sequence
			rtp_used = 0;					// and drop any reorder entries
		} else if (seq_dist < 0) {
			continue;					// skip: sequence in recent past
		}
		
		if (seq_abs == 0) {					// this is the correct sequence number
			mai_audio_write_int(data, len);			// send this packet to jack
			rtp_next = seq + 1;				// set next sequence number from this packet
			
			rob_scan();					// scan buffer to see if we have next packet already
			continue;					// ready for next packet 
		}
		
		if (seq_abs > ROB_LEN) {				// this sequence is outside of buffer range
			MAI_STAT_INC(rtp.skipped);
			
			rtp_next += 1;					// skip past current next sequence number
			rob_scan();					// scan buffer to see if we have expected packet now
			
			if (seq == rtp_next) {				// if current packet is now ready:
				mai_audio_write_int(data, len);		// send this packet to jack
				rtp_next = seq + 1;			// set next sequence from this packet
				continue;				// ready for next packet
			}
		}
		
		size_t idx = seq % ROB_LEN;				// get reorder index from sequence number
		rtp_used += 1;						// increment reorder use counter
		
		rob[idx].seq = seq;
		rob[idx].len = len;
		memcpy(rob[idx].payload, data, len);			// put this packet into reorder buffer
		
		MAI_STAT_INC(rtp.reordered);
	}
	
	mai_debug("Unexpected Thread Exit!\n");
	return(arg);							// should not reach here
}

/* ######################################################################## */
static void *rtp_send(void *arg) {
	// create an RTP packet and set the static header values
	const size_t   paylen = rtp_samples * mai.args.channels * (mai.args.bits / 8);
	const size_t   pktlen = sizeof(struct packet) + paylen;
        struct packet *packet = alloca(pktlen);
	
	packet->vpxcc = 0b10000000;			// Version=2, P=0, X=0, CC=0
	packet->mpt   = 96;				// M=0, PT=96
	packet->ssrc  = lrand48();			// Set Random SSRC IV
	
	uint16_t seq  = lrand48() & 0xFFFF;		// Set Random Initial Sequence
	uint64_t time;
	
	// loop on ringbuffer and send samples
	for (struct timespec ts = { .tv_sec = 0, .tv_nsec = mai.args.ptime * 900 }; 1; nanosleep(&ts, NULL)) {
		mai_audio_read_int(packet->payload, paylen);		// get packet payload
		
		time = __sync_fetch_and_add(&rtp_clock, rtp_samples);
		
		packet->time = htonl(time & 0xFFFFFFFF);
		packet->seq  = htons(seq++);
		
		if (send(rtp_sock, packet, pktlen, 0) <= 0)		// send packet to network
			mai_error("packet send: %m\n");
		else
			MAI_STAT_INC(rtp.packets);
	}
	
	mai_debug("Unexpected Thread Exit!\n");
	return(arg);							// should not reach here
}

/* ######################################################################## */
static pthread_t tid;

int mai_rtp_init(void) {
	// samples/packet
	rtp_samples = (mai.args.ptime * ((mai.args.rate == 96000) ? 96000 : 48000)) / 1000000;
	
	if ((rtp_sock = mai_sock_open(mai.args.mode, mai.args.addr, mai.args.port)) <= 0)
		return(mai_error("could not open multicast socket\n"));
		
	mai_audio_size(rtp_samples * (ROB_LEN+1));
	
	// bytes/packet + rtp(12) + udp(8) + ip overhead(20)
	size_t rtp_bytes = (rtp_samples * mai.args.channels * (mai.args.bits / 8)) + 40;
	size_t rtp_mtu   = mai_sock_if_mtu();
	
	if (rtp_bytes > rtp_mtu)
		return(mai_error("packet size (%zu) is larger than interface mtu (%zu).\n", rtp_bytes, rtp_mtu));
		
	return(mai_debug("RTP %s: %s:%d\n", (MAI_SENDER ? "Sender" : "Receiver"), mai.args.addr, mai.args.port));
}

int mai_rtp_start(void) {
	if (pthread_create(&tid, NULL, (MAI_SENDER ? rtp_send : rtp_recv), NULL))
		return(mai_error("could not start rtp thread: %m\n"));
		
	return(0);
}

int mai_rtp_stop(void) {
	pthread_cancel(tid);
	return(0);
}

/* ######################################################################## */
uint64_t mai_rtp_clock(void) {
	return(rtp_clock);
}

void mai_rtp_offset(int64_t offset) {
	// if we're within -2 .. +2 packets of master clock
	if ((offset >= -((int64_t)(rtp_samples*2))) && (offset <= (rtp_samples*2)))
		return;
		
	// if not, apply offset to the rtp clock
	MAI_STAT_INC(rtp.resynced);
	__sync_fetch_and_sub(&rtp_clock, offset);
}

/* ######################################################################## */

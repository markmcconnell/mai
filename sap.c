#include "mai.h"

/* ######################################################################## */
#define SDP_TYPE "application/sdp"

struct packet {
	uint8_t		vartec;		// Version(3) Addr(1) Reserved(1) Type(1) Encrypt(1) Compress(1)
	uint8_t		authlen;
	uint16_t	hash;
	uint32_t	source;
	
	uint8_t		mime[sizeof(SDP_TYPE)];
	char		payload[];
} __attribute__((__packed__));

static int      sap_sock   = -1;
static int	sap_active =  1;
static uint32_t sap_source =  0;
static char     sap_addr[INET_ADDRSTRLEN];

/* ######################################################################## */
void *sap(void *arg) {
	// create packet buffer and header object
	uint8_t		 buffer[2048];
	struct packet	*packet = (struct packet *)buffer;
	
	// fill in header with static ipv4 annoucement values
	packet->vartec  = 0b00100000;
	packet->authlen = 0;
	packet->hash    = getpid();
	packet->source  = sap_source;
	
	// add "application/sdp\0" to the packet
	memcpy(packet->mime, SDP_TYPE, sizeof(packet->mime));
	
	// start adding sdp lines to the packet
	char *payload = packet->payload;
	char *ptime = NULL;
	
	switch (mai.args.ptime) {
		case 4000: ptime = (mai.args.rate == 44100) ? "4.35" : "4"; 	break;
		case 1000: ptime = (mai.args.rate == 44100) ? "1.09" : "1"; 	break;
		case  333: ptime = (mai.args.rate == 44100) ? "0.36" : "0.33"; 	break;
		case  250: ptime = (mai.args.rate == 44100) ? "0.27" : "0.25"; 	break;
		case  125: ptime = (mai.args.rate == 44100) ? "0.13" : "0.12"; 	break;
	}
	
	payload += sprintf(payload, "v=0\r\n");
	payload += sprintf(payload, "o=- %ld %ld IN IP4 %s\r\n", time(NULL), time(NULL), sap_addr);
	payload += sprintf(payload, "s=%s\r\n", mai.args.session);
	payload += sprintf(payload, "c=IN IP4 %s/32\r\n", mai.args.addr);
	payload += sprintf(payload, "t=0 0\r\n");
	payload += sprintf(payload, "m=audio %d RTP/AVP 96\r\n", mai.args.port);
	payload += sprintf(payload, "i=%s\r\n", mai.args.title);
	payload += sprintf(payload, "a=rtpmap:96 L%d/%d/%d\r\n", mai.args.bits, mai.args.rate, mai.args.channels);
	payload += sprintf(payload, "a=recvonly\r\n");
	
	if (ptime)
		payload += sprintf(payload, "a=ptime:%s\r\n", ptime);
	
	payload += sprintf(payload, "a=ts-refclk:ptp=IEEE1588-2008:%s\r\n", mai_ptp_source());
	payload += sprintf(payload, "a=mediaclk:direct=0\r\n");
	
	size_t pktlen = sizeof(*packet) + strlen(packet->payload);
	
	// announce the session every 5 minutes
	for (int lp=0; sap_active; sleep(1)) {
		if ((lp++ % 300) != 0)
			continue;
			
		if (send(sap_sock, packet, pktlen, 0) <= 0)
			mai_error("packet send: %m\n");
			
		mai_debug("Sent SAP Announce Packet.\n");
	}
	
	// process shutdown: try to delete the session before exit
	packet->vartec |= 0b00000100;		// Set T=1 to remove session
	send(sap_sock, packet, pktlen, 0);	// Send final SAP packet
	
	// stop thread
	mai_debug("Sent SAP Delete Packet.\n");
	return(arg);
}

/* ######################################################################## */
static pthread_t tid;

int mai_sap_init() {
	// open SAP broadcast socket
	if ((sap_sock = mai_sock_open('s', "239.255.255.255", 9875)) < 0)
		return(mai_error("could not open SAP multicast socket\n"));
		
	// save address in network order for SAP packet
	memcpy(&sap_source, mai_sock_if_addr(), sizeof(sap_source));
	
	// save copy of text based address for SDP packet
	inet_ntop(AF_INET, &sap_source, sap_addr, sizeof(sap_addr));
	
	return(mai_debug("SAP Sender: 239.255.255.255:9875 (%s: %s).\n", mai_sock_if_name(), sap_addr));
}

/* ######################################################################## */
int mai_sap_start() {
	// start sap broadcaster
	if (pthread_create(&tid, NULL, sap, NULL))
		return(mai_error("could not start sap thread: %m\n"));
		
	return(0);
}

int mai_sap_stop() {
	sap_active = 0;				// request broadcast thread to stop
	pthread_join(tid, NULL);		// then wait for it to terminate
	
	return(0);
}

/* ######################################################################## */

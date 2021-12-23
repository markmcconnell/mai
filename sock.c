#include "mai.h"

/* ######################################################################## */
static const char		*if_name;		// multicast interface name
static uint8_t		 	 if_local[10];		// multicast interface l2 address
static unsigned int		 if_mtu;		// multicast interface mtu
static int			 if_index;		// multicast interface index
static struct in_addr		 if_addr;		// multicast interface address

/* ######################################################################## */
static inline int setsockopt_i(int sk, int level, int name, int value) {
	int opt = value;
	return(setsockopt(sk, level, name, &opt, sizeof(opt)));
}

/* ######################################################################## */
static int sock_send(const char *ip, const uint16_t port) {
	// convert target address
	struct sockaddr_in addr;
	
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(port);
	
	if (!inet_aton(ip, &addr.sin_addr))
		return(mai_error("target address (%s): %m\n", ip));

	// create udp4 socket
	int sk;
	
	if ((sk = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return(mai_error("socket: %m\n"));
		
	// set outbound multicast interface
	if (if_name && setsockopt(sk, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr)))
		return(mai_error("multicast interface: %m\n"));
	
	// set type of service (equivalent to DSCP AF41)
	if (setsockopt_i(sk, IPPROTO_IP, IP_TOS, IPTOS_PREC_FLASHOVERRIDE|IPTOS_THROUGHPUT))
		return(mai_error("ip tos: %m\n"));
		
	// set time to live
	if (setsockopt_i(sk, IPPROTO_IP, IP_MULTICAST_TTL, 32))
		return(mai_error("multicast ttl: %m\n"));
	
	// set default send address
	if (connect(sk, (struct sockaddr *)&addr, sizeof(addr)))
		return(mai_error("connect: %m\n"));
	
	return(sk);
}

/* ######################################################################## */
static int sock_recv(const char *ip, const uint16_t port) {
	// setup multicast group request
	struct ip_mreqn req = (struct ip_mreqn){ .imr_ifindex = if_index, .imr_address.s_addr = htonl(INADDR_ANY) };
	
	if (!inet_aton(ip, &req.imr_multiaddr))
		return(mai_error("multicast address (%s): %m.\n", ip));
	
	// create udp4 socket
	int sk;
	if ((sk = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return(mai_error("create socket: %m.\n"));
	
	// join multicast group to start receving packets
	if (setsockopt(sk, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof(req)))
		return(mai_error("ip add membership: %m\n"));
	
	// let other processes bind to same port
	if (setsockopt_i(sk, SOL_SOCKET, SO_REUSEPORT, 1))
		return(mai_error("reuse port: %m\n"));
	
	// let other processes bind to same address
	if (setsockopt_i(sk, SOL_SOCKET, SO_REUSEADDR, 1))
		return(mai_error("reuse address: %m\n"));
	
	// bind to port and multicast destination
	struct sockaddr_in addr;
	
	addr.sin_family 	= AF_INET;		
	addr.sin_port   	= htons(port);		
	addr.sin_addr	 	= req.imr_multiaddr;	
	
	if (bind(sk, (struct sockaddr *)&addr, sizeof(addr)))
		return(mai_error("bind: %m\n"));
	
	// finished, success
	return(sk);
}

/* ######################################################################## */
int mai_sock_open(int mode, const char *ip, const uint16_t port) {
	return((mode == 's') ? sock_send(ip, port) : sock_recv(ip, port));
}

/* ######################################################################## */
int mai_sock_if_set(const char *name) {
	if (!name || !name[0])
		return(0);

	if ((if_name = strdup(name)) == NULL)
		return(mai_error("strdup: %m\n"));
		
	int sk;
	if ((sk = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return(mai_error("socket: %m\n"));

	// setup interface request 
	struct ifreq ifr;
	
	strncpy(ifr.ifr_name, if_name, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ-1] = 0;
	
	// ask for interface mtu
	if (ioctl(sk, SIOCGIFMTU, &ifr))
		return(mai_error("get interface mtu (%s): %m\n", if_name));
		
	if_mtu = ifr.ifr_mtu;

	// ask for interface index
	if (ioctl(sk, SIOCGIFINDEX, &ifr))
		return(mai_error("get interface index (%s): %m\n", if_name));
		
	if_index = ifr.ifr_ifindex;
	
	// ask for primary ipv4 address
	ifr.ifr_addr.sa_family = AF_INET;
	
	if (ioctl(sk, SIOCGIFADDR, &ifr))
		return(mai_error("get interface address (%s): %m\n", if_name));

	if_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
	
	// ask for layer2 address
	if (ioctl(sk, SIOCGIFHWADDR, &ifr))
		return(mai_error("get interface hardware address (%s): %m\n", if_name));

	uint8_t *out = &if_local[0];
		
	*out++ = ifr.ifr_hwaddr.sa_data[0];
	*out++ = ifr.ifr_hwaddr.sa_data[1];
	*out++ = ifr.ifr_hwaddr.sa_data[2];

	*out++ = 0xFF;
	*out++ = 0xFE;

	*out++ = ifr.ifr_hwaddr.sa_data[3];
	*out++ = ifr.ifr_hwaddr.sa_data[4];
	*out++ = ifr.ifr_hwaddr.sa_data[5];

	*out++ = 0x00;
	*out++ = 0x02;

	close(sk);
	return(0);
}

/* ######################################################################## */
size_t      mai_sock_if_mtu(void)           { return( if_mtu);    }
const void *mai_sock_if_addr(void)          { return(&if_addr);   }
const char *mai_sock_if_name(void)          { return( if_name);   }
      void  mai_sock_if_local(uint8_t *out) { memcpy(out, if_local, sizeof(if_local)); }

/* ######################################################################## */

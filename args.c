#include "mai.h"
#include <getopt.h>

/* ######################################################################## */
static int usage(const char *fmt, ...) {
	if (fmt) {
		va_list ap;
		
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		
		fprintf(stderr, "\n\n");
	}

	
	fprintf(stderr, "Usage: mai <args>\n\n");
	
	fprintf(stderr, "-m,--mode      <send|recv>           AES67 sender or receiver  - REQUIRED\n");
	fprintf(stderr, "-a,--address   <ip>[:<port=5004>]    AES67 multicast address   - REQUIRED\n");
	fprintf(stderr, "-i,--interface <interface>           AES67 multicast interface\n\n");
	
	fprintf(stderr, "-s,--session   <session name>        AES67 sender Session Name\n");
	fprintf(stderr, "-t,--title     <session title>       AES67 sender Session Title\n\n");
	
	fprintf(stderr, "-b,--bits      <bits>                AES67 encoding bits <16,24,32>\n");
	fprintf(stderr, "-r,--rate      <samplerate>          AES67 sample rate <44100,48000,96000>\n");
	fprintf(stderr, "-c,--channels  <channels>            AES67 channels in stream <1-8>\n");
	fprintf(stderr, "-p,--ptime     <ptime>               AES67 audio per packet <4000,1000,333,250,125>us\n\n");
	
	fprintf(stderr, "-l,--client    <name>                JACK client name\n");
	fprintf(stderr, "-o,--ports     <names>               JACK port connection list\n\n");
	
	fprintf(stderr, "-u,--user      <userid>              drop privileges to userid\n");
	fprintf(stderr, "-g,--group     <groupid>             drop privileges to group\n\n");
	
	fprintf(stderr, "-V,--version                         Show Version and Copyright\n");
	fprintf(stderr, "-v,--verbose                         Verbose Debugging Output\n");
	fprintf(stderr, "-h,--help                            Show this Help Screen\n\n");
	
	exit(-1);
}

/* ######################################################################## */
struct mai mai;

void mai_args_init(int argc, char *argv[]) {
	// clear structure now
	memset(&mai, 0, sizeof(mai));

	// set command line defaults
	mai.args.client	= "mai";
	mai.args.ptime	= 1000;
	
	// long options structure
	static struct option options[] = {
		{ "mode",	required_argument,	0, 'm'	},
		{ "address",	required_argument,	0, 'a'	},
		{ "interface",	required_argument,	0, 'i'	},
		
		{ "session",	required_argument,	0, 's'	},
		{ "title",	required_argument,	0, 't'	},
		{ "bits",	required_argument,	0, 'b'	},
		{ "rate",	required_argument,	0, 'r'  },
		{ "channels",	required_argument,	0, 'c'	},
		{ "ptime",	required_argument,	0, 'p'	},
		
		{ "client",	required_argument,	0, 'l'	},
		{ "ports",	required_argument,	0, 'o'	},
		
		{ "user",	required_argument,	0, 'u'	},
		{ "group",	required_argument,	0, 'g' 	},
		
		{ "version",	no_argument,		0, 'V'	},
		{ "verbose",	no_argument,		0, 'v'  },
		{ "help",	no_argument,		0, 'h'  },
		{ NULL,		0,			0, 0	}
	};

	char *ptr;
	
	for (int ch; (ch = getopt_long(argc, argv, ":m:a:i:s:t:b:r:c:p:l:o:u:g:Vvh", options, NULL)) != -1; ) { switch (ch) {
		case 'm':
			     if (optarg[0] == 's') mai.args.mode = 's';
			else if (optarg[0] == 'r') mai.args.mode = 'r';
			else usage("ERROR: 'mode' argument must be 's' or 'r', got '%c'.", optarg[0]);
			
			break;
			
		case 'i': 
			if (mai_sock_if_set(optarg))
				usage("ERROR: 'interface' parameter error.");
			
			break;
			
		case 's': mai.args.session = optarg ? strdup(optarg) : NULL; 	break;
		case 't': mai.args.title   = optarg ? strdup(optarg) : NULL; 	break;
		case 'l': mai.args.client  = optarg ? strdup(optarg) : NULL; 	break;
		case 'o': mai.args.ports   = optarg ? strdup(optarg) : NULL; 	break;
		case 'u': mai.args.uid	   = atoi(optarg); 			break;
		case 'g': mai.args.gid	   = atoi(optarg); 			break;
		case 'v': mai.args.verbose = 1;	    				break;
		case 'h': usage(NULL);						break;
		
		case 'V': 
			fprintf(stderr, 
				"MAI: Mark's AES67 Implementation. Version %s.\n\n%s\n\n%s\n\n%s\n", 
				MAI_VERSION, MAI_COPYRIGHT, MAI_LICENSE, MAI_DISCLAIMER
			);
			exit(0);
		
		case 'r':
			mai.args.rate = atoi(optarg);
			if ((mai.args.rate != 44100) && (mai.args.rate != 48000) && (mai.args.rate != 96000))
				usage("ERROR: 'rate' argument must be 44100, 48000 or 96000 (got: %d)", mai.args.rate);
				
			break;
		
		case 'c': 
			mai.args.channels = atoi(optarg); 
			if ((mai.args.channels < 1) || (mai.args.channels > 8))
				usage("ERROR: 'channels' argument must be 1..8 (got: %d)", mai.args.channels);
				
			break;
			
		case 'p':
			mai.args.ptime = atoi(optarg);
			if ((mai.args.ptime != 4000) && (mai.args.ptime != 1000) && (mai.args.ptime != 333) && (mai.args.ptime != 250) && (mai.args.ptime != 125))
				usage("ERROR: argument 'ptime' must be one of <4000, 1000, 333, 250, 125>");
				
			break;
			
		case 'b':
			mai.args.bits = atoi(optarg);
			if ((mai.args.bits != 16) && (mai.args.bits != 24) && (mai.args.bits != 32))
				usage("ERROR: 'bits' argument must be one of <16, 24, 32>");
				
			break;
			
		case 'a':
			if (!optarg || ((mai.args.addr = strdup(optarg)) == NULL))
				break;
			
			if ((ptr = strchr(mai.args.addr, ':')) != NULL) {
				*ptr++ = 0;
				
				int port = *ptr ? atoi(ptr) : 5004;
				if ((port < 1025) || (port > 49152))
					usage("ERROR: 'port' argument must be within 1025..49152");
					
				mai.args.port = port;
			} else {
				mai.args.port = 5004;
			}
			
			break;
			
		case ':':
			usage("ERROR: argument '%c' requires a value!", optopt);
			break;
			
		default:
			usage("ERROR: unknown '%c' argument!", optopt);
			break;
	}}
	
	// check required parameters
	if (!mai.args.mode)
		usage("ERROR: 'mode' argument was not supplied!");
	
	if (!mai.args.addr)
		usage("ERROR: 'address' argument was not supplied!");
		
	if (!mai.args.bits)
		usage("ERROR: 'bits' argument was not supplied!");
		
	if (!mai.args.channels)
		usage("ERROR: 'channels' argument was not supplied!");
		
	if (!mai.args.rate)
		usage("ERROR: 'rate' argument was not supplied!");
		
	// check and fill optional parameters
	if (!mai.args.session) {
		char host[HOST_NAME_MAX];
		
		gethostname(host, sizeof(host));
		host[sizeof(host)-1] = 0;
	
		if (asprintf((char **)&mai.args.session, "%s.%d", host, getpid()) <= 0)
			usage("ERROR: unable to create default 'session' argument!");
	}
	
	if (!mai.args.title) {
		if (asprintf((char **)&mai.args.title, "Jack 1-%d", mai.args.channels) <= 0)
			usage("ERROR: unable to create default 'title' argument!");
	}
}

/* ######################################################################## */

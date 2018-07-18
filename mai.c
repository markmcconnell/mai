#include "mai.h"

/* ######################################################################## */
struct mai_func {
	int		(*func)(void);
	int		mode;
};

static struct mai_func mai_init[] = {
	{ mai_ptp_init,		'*' },
	{ mai_rtp_init,		'*' },
	{ mai_sap_init,		's' },
	{ mai_jack_init,	'*' },

	{ mai_ptp_start,	'*' },
	{ mai_rtp_start,	'*' },
	{ mai_sap_start,	's' },
	{ NULL,			0   }
};

static struct mai_func mai_fini[] = {
	{ mai_rtp_stop,		'*' },
	{ mai_ptp_stop,		'*' },
	{ mai_sap_stop,		's' },
	{ NULL,			0   }
};

static void run(const struct mai_func *ptr) {
	for (; ptr && ptr->func; ptr++) {
		if (((ptr->mode == mai.args.mode) || (ptr->mode == '*')) && (ptr->func)())
			exit(-1);
	}
}

/* ######################################################################## */
static void stats(void) {
	fprintf(stderr, "\n\n----- Statistics -----\n\n");
	
	fprintf(stderr, "Audio Clock Drift:     %zd\n",   MAI_STAT_GET(audio.drift));
	fprintf(stderr, "Audio Buffer Underrun: %zu\n",   MAI_STAT_GET(audio.underrun));
	fprintf(stderr, "Audio Buffer Overrun:  %zu\n\n", MAI_STAT_GET(audio.overrun));
	
	fprintf(stderr, "RTP Clock Resynced:    %zu\n",   MAI_STAT_GET(rtp.resynced));
	fprintf(stderr, "RTP Total Packets:     %zu\n",   MAI_STAT_GET(rtp.packets));
	fprintf(stderr, "RTP Reordered Packets: %zu\n",   MAI_STAT_GET(rtp.reordered));
	fprintf(stderr, "RTP Dropped Packets:   %zu\n\n", MAI_STAT_GET(rtp.skipped));
	
	fprintf(stderr, "PTP Master Changes:    %zu\n",   MAI_STAT_GET(ptp.masters));
	fprintf(stderr, "PTP Delay Updates:     %zu\n",   MAI_STAT_GET(ptp.requests));
	fprintf(stderr, "PTP General Messages:  %zu\n",   MAI_STAT_GET(ptp.general));
	fprintf(stderr, "PTP Event Messages:    %zu\n\n", MAI_STAT_GET(ptp.event));
}

/* ######################################################################## */
int main(int argc, char *argv[]) {
	// intialize drng
	srand48(time(NULL) * getpid());

	// parse command line options
	mai_args_init(argc, argv);
	
	// block signals for all threads
	sigset_t sigset;
	
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGUSR1);
	sigaddset(&sigset, SIGUSR2);
	
	if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
		return(mai_error("could not mask signals: %m\n"));
	
	// initialize modules
	run(mai_init);
        	
	// wait for signal
	int signum = 0;
	
	while (1) {
		sigwait(&sigset, &signum);
		
		if (signum == SIGUSR1)
			stats();
		else
			break;
	}
	mai_info("Signal %d. Exiting.\n", signum);
	
	// stop modules
	run(mai_fini);
	
	// print final statistics
	if (mai.args.verbose)
		stats();
	
	return(0);
}

/* ######################################################################## */

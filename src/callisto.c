#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <math.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>

#include "callisto.h"
#include "conf.h"
#include "serial.h"
#include "util.h"
#include "log.h"
#include "fits.h"
#include "server.h"
#include "eeprom.h"


/* debug off, disable output, stop: */
#define RESET_STRING "D0\rGD\rS0\r"
#define ID_QUERY "S0\r"
#define ID_RESPONSE "$CRX:Stopped\r"

#define HEXDATA_RESET -1

static void hexdata(config_t * config, int serial_fd, signed char c);
static void run_schedule(config_t * config);
static int reset(config_t * config, int serial_fd);
static void init(config_t * config, int serial_fd);
static void start(config_t * config, int serial_fd);
static void stop(int serial_fd);
static void start_overview(config_t * config, int serial_fd);
static void finish_overview(config_t * config);
static void handle_message(config_t * config, int serial_fd, const char *msg);


int buffer_size = 0;
buffer_t buffer[2] = { { NULL, 0, 0 }, { NULL, 0, 0 } };
static int current_buffer = 0;
volatile int save_buffer = -1;
volatile int active_buffer = 0; /* interthread copy of current_buffer */


static char message[MAX_MESSAGE] = "";
static int message_length = 0, in_message = 0, in_data = 0;

enum {
    STOPPED,
    STOPPING,
    STARTING,
    RUNNING,
    OVERVIEW
};
static int state = STOPPED;
volatile int start_command = 0, overview_command = 0, stop_command = 0;




static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\nOptions:\n", prog);
    fprintf(stderr,
            "\t-c,--config <file>   Specify configuration file\n"
	    "\t-o,--datadir <dir>   Specify data output directory\n"
	    "\t-O,--ovsdir <dir>    Specify ovewrview output directory\n"
	    "\t-s,--schedule <file> Specify schedule file\n"
	    "\t-u,--user <user>     Run as the specified user\n"
	    "\t-L,--load-channels   Load channels data into EEPROM\n"
	    "\t-C,--check-only      Only check whether the device is Callisto\n"
	    "\t-P,--pidfile <file>  Write PID into the sepcified file\n"
            "\t-d,--debug           Debug mode (no fork, messages to STDERR)\n"
            "\t-D,--serial-debug    Debug mode with serial port traffic logging\n"
            "\t-4,--ipv4            Bind to IPv4 address (default is IPv6 with IPv4 mapping)\n"
            "\t-6,--ipv6            Bind only to IPv6 address (no IPv4 mapping)\n"
	    "\t-V,--version         Show program version\n"
	    "\t-h,--help            This help text\n"
            );
    exit(EXIT_FAILURE);
}


static volatile sig_atomic_t killed = 0;
void terminate(int signum) {
    if (signum < 0) { /* negative signal means immediate termination */
	   killed = 2;
	   exit(EXIT_SUCCESS);
    } else if (signum > 0) {
	   killed = 1;
    }
}
/*
    if (killed) {
	if (killed < 2) {
	    // stop callisto hw: 
	    write_serial(serial_fd, "GD\rS0\r");
	    logprintf(LOG_NOTICE, "Failed to die cleanly, data loss possible");
	}
	if (pidfd >= 0) {
	    ftruncate(pidfd, 0);
	    close(pidfd);
	    unlink(pidfile);
	}
	
	exit(EXIT_SUCCESS);
    }

    if (signum > 0)
        logprintf(LOG_NOTICE, "Caught signal %i, terminating", signum);

    killed = 1;
}
*/


static volatile sig_atomic_t switch_buffers = 0;
static void hup_handler(int signum) {
    start_command = 1;
}

config_t parse_options(int argc, char **argv) {
    config_t config = default_config();

    /* parse options */
    while (1) {
        int opt;
        static struct option long_options[] = {
            {"config", 1, NULL, 'c'},
            {"datadir", 1, NULL, 'o'},
            {"ovsdir", 1, NULL, 'O'},
            {"schedule", 1, NULL, 's'},
            {"user", 1, NULL, 'u'},
            {"load-channels", 0, NULL, 'L'},
            {"check-only", 0, NULL, 'C'},
            {"pidfile", 1, NULL, 'P'},
            {"debug", 0, NULL, 'd'},
            {"serial-debug", 0, NULL, 'D'},
            {"version", 0, NULL, 'V'},
            {"help", 0, NULL, 'h'},
            {"ipv4", 0, NULL, '4'},
            {"ipv6", 0, NULL, '6'},
            {0, 0, 0, 0}
        };

        opt = getopt_long(argc, argv, "c:o:O:s:u:LCdDVh46", long_options, NULL);
        if (opt == -1)
            break;

        switch (opt) {
        case 'c':
            config.configdir = optarg;
            break;
        case 'o':
            config.datadir = optarg;
            break;
        case 'O':
            config.ovsdir = optarg;
            break;
        case 's':
            config.schedulefile = optarg;
            break;
        case 'u':
            if (getuid() != 0 && geteuid() != 0) {
                fprintf(stderr, "ERROR: Must be root to change privileges\n");
                exit(EXIT_FAILURE);
            }
            {
                struct passwd *pe = NULL;

                if ((pe = getpwnam(optarg)) == NULL) {
                    fprintf(stderr, "error: Invalid user: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }

                config.server_uid = pe->pw_uid;
                config.server_gid = pe->pw_gid;
            }
            break;
        case 'P':
            config.pidfile = optarg;
            if (*optarg != '/') {
                fprintf(stderr, "ERROR: PID file must be an absolute path\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'L':
            config.do_upload = 1;
            break;
        case 'C':
            config.check_only = 1;
            break;
        case 'd':
            config.debug = 1;
            break;
        case 'D':
            config.debug = 1;
            config.serial_debug++;
            break;
        case 'V':
            printf("e-Callisto for Unix " PACKAGE_VERSION "\n");
            exit(EXIT_SUCCESS);
        case '4':
            config.use_ipv4 = 1;
            break;
        case '6':
            config.ipv6only = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
    }
    
    if (optind < argc) {
        fprintf(stderr, "Command line should contain only recognized options\n");
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    return config;
}

int main(int argc, char **argv) {
    char c;
    int i, l;
    int serial_fd = -1;
    int pidfd = -1;

    config_t config = parse_options(argc, argv);
    
    if (!read_configs(&config)) {
        fprintf(stderr, "ERROR: Unable to load config files.\n");
        return EXIT_FAILURE;
    }

    log_init(!config.debug);

    buffer_size = config.filetime * config.samplerate;
    buffer[0].data = (uint8_t*)malloc(buffer_size);
    buffer[1].data = (uint8_t*)malloc(buffer_size);

    if (!buffer[0].data || !buffer[1].data) {
        fprintf(stderr, "ERROR: Cannot allocate data buffers\n");
        return EXIT_FAILURE;
    }

    if (config.debug) {
        logprintf(LOG_DEBUG, "Initializing serial port..");
    }

    serial_fd = init_serial(config.serialport);
    if (serial_fd < 0) {
        fprintf(stderr, "ERROR: The serial port %s could not be initialized.\n", config.serialport);
        return EXIT_FAILURE;
    }
    /* in unknown state => "reset" callisto */
    if (!reset(&config, serial_fd)) {
        fprintf(stderr, "ERROR: The device at %s does not seem to be Callisto (reset failed)\n", config.serialport);
        return EXIT_FAILURE;
    }

    if (config.debug) {
        logprintf(LOG_DEBUG, "Probing serial port..");
    }
    /* identify: */
    write_serial(serial_fd, ID_QUERY);
    i = 0;
    while (read_serial(serial_fd, &c)) {
        message[i] = c;
        if (i < MAX_MESSAGE-1) i++;
    }
    message[i] = 0;
    l = strlen(ID_RESPONSE);
    if (i < l || strncmp(message, ID_RESPONSE, l)) {
        fprintf(stderr, "ERROR: The device at %s does not seem to be Callisto (ID failed)\n", config.serialport);
        return EXIT_FAILURE;
    }
    message[0] = 0;

    if (config.debug) {
        logprintf(LOG_DEBUG, "Callisto device detected..");
    }

    if (config.do_upload && !upload_channels(&config, serial_fd)) {
        return EXIT_FAILURE;
    }

    if (config.check_only) {
        logprintf(LOG_DEBUG, "Initialization checks complete. Exiting..");
        return EXIT_SUCCESS;
    }

    if (config.net_port > 0 && !server_init(config.net_port, !config.ipv6only, !config.use_ipv4)) {
        return EXIT_FAILURE;
    }

    /* drop privileges */
    if (config.server_uid > 0) {
        if (setgid(config.server_gid) != 0) {
            fprintf(stderr, "ERROR: Cannot change GID: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        if (setuid(config.server_uid) != 0) {
            fprintf(stderr, "ERROR: Cannot change UID: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
    }

    if (!fits_init(&config)) {
        return EXIT_FAILURE;
    }
    if (!download_channels(&config, serial_fd)) {
        return EXIT_FAILURE;
    }

    /* daemonize */
    if (!config.debug && daemonize()) {
        fprintf(stderr, "ERROR: Cannot daemonize program: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (config.pidfile != NULL) {
        if (unlink(config.pidfile) && errno != ENOENT) {
            fprintf(stderr, "ERROR: Cannot remove old PID file: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        pidfd = open(config.pidfile, O_WRONLY|O_CREAT|O_EXCL, 0644);
        if (pidfd < 0) {
            fprintf(stderr, "ERROR: Cannot create PID file: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        /* write pidfile */
        char pidstr[20];
        snprintf(pidstr, 20, "%i\n", getpid());
        write(pidfd, pidstr, strlen(pidstr));
        fsync(pidfd);
    }

    /* ignore these normally fatal signals: */
    signal(SIGALRM, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    /* used to immediately write out a FITS file: */
    signal(SIGHUP, hup_handler);
    /* honor these fatal signals: */
    signal(SIGTERM, terminate);
    signal(SIGINT, terminate);

    if (config.net_port > 0)
        server_start(&config);
    fits_start(&config);

    logprintf(LOG_NOTICE, "e-Callisto for Unix " PACKAGE_VERSION " started");

    /* main loop */
    init(&config, serial_fd);

    if (config.autostart)
        start(&config, serial_fd);
    
    while (1) {
        if (killed) {
            if (killed < 2) {
                /* stop hw: */
                write_serial(serial_fd, "GD\rS0\r");
                /* wait for FITS write to finish: */
                while (save_buffer != -1)
                    msleep(1);
                /* If there is data in the current buffer, save
                   it. Otherwise the other buffer has already been
                   saved above. */
                if (buffer[current_buffer].size > 0) {
                    save_buffer = current_buffer;
                    /* wait for FITS write to finish: */
                    while (save_buffer != -1)
                        msleep(1);
                }
            }
            if (pidfd >= 0) {
                ftruncate(pidfd, 0);
                close(pidfd);
                unlink(config.pidfile);
            }
            exit(EXIT_SUCCESS);
        }

        /* check schedule (if it exists) for start/stop/overview commands */
        run_schedule(&config);

        /* state switching: */
        if (overview_command) {
            if (state == OVERVIEW) { /* already in progress */
                overview_command = 0;
            } else if (state == STARTING || state == RUNNING) {
                stop(serial_fd);
                start_command = 1; /* start again after finished */
            } else if (state == STOPPED) {
                start_overview(&config, serial_fd);
            }
        } else if (stop_command) {
            if (state == STARTING || state == RUNNING)
                stop(serial_fd);
            stop_command = 0;
            start_command = 0;
        } else if (state == STOPPED && start_command) {
            init(&config, serial_fd);
            start(&config, serial_fd);
            start_command = 0;
        } else if (state == RUNNING && start_command) {
            switch_buffers = 1; /* start a new FITS file */
            start_command = 0;
        }


        if (state == STOPPED) {
            /* we are stopped => wait for commands */
            msleep(1);
            continue;
        }


        if (!read_serial(serial_fd, &c)) {
            if (state == OVERVIEW) {
                finish_overview(&config);
                continue;
            }
            logprintf(LOG_ERR, "Timeout reading from serial port, resetting");
            reset(&config, serial_fd);
            init(&config, serial_fd);
            start(&config, serial_fd);
            continue;
        }

        if (in_message && c == MESSAGE_END) {
            message[message_length] = 0;
            handle_message(&config, serial_fd, message);
            message_length = 0;
            in_message = 0;
            continue;
        }

        if (!in_message && c == MESSAGE_START) {
            message_length = 0;
            in_message = 1;
            continue;
        }

        if (in_message) {
            message[message_length] = c;
            if (message_length < MAX_MESSAGE-1)
                message_length++;
            continue;
        }

        if (c == EEPROM_READY)
            continue;

        if (!in_data && c == DATA_START) {
            in_data = 1;
            continue;
        }
	
        if (in_data && c == DATA_END) {
            /* data stream ends => stop state machine, write partial
               FITS and reset data buffers: */
            in_data = 0;
            write_serial(serial_fd, "S0\r");
            hexdata(&config, serial_fd, HEXDATA_RESET);
            /* wait for previous buffer save to finish: */
            while (save_buffer != -1)
        	msleep(1);
            /* save the current buffer and wait for it to finish: */
            save_buffer = current_buffer;
            while (save_buffer != -1)
        	msleep(1);
            buffer[0].size = buffer[1].size = 0;
            continue;
        }

        if (in_data)
            hexdata(&config, serial_fd, c);
        else {
            logprintf(LOG_ERR, "Unexpected character '%c', resetting", c);
            reset(&config, serial_fd);
        }
    }

    return EXIT_FAILURE;
}




static void disable_schedule(config_t * config) {
    logprintf(LOG_WARNING, "Disabling scheduling and starting recording");
    config->numschedule = 0;
    if (state != STARTING && state != RUNNING)
	start_command = 1;
}

#define SCHEDULE_CHECK_INTERVAL 60
static void run_schedule(config_t * config) {
    int i, hadsched;
    time_t now = time(NULL);
    static time_t last_check = 0;
    static struct stat old_st;
    struct stat st;

    /* check for new schedule file: */
    hadsched = config->numschedule;
    if (now - last_check >= SCHEDULE_CHECK_INTERVAL) {
        if (config->debug)
            logprintf(LOG_DEBUG, "Checking for new schedule file");

        last_check = now;
        if (stat(config->schedulefile, &st)) {
            if (errno == ENOENT && hadsched)
                logprintf(LOG_WARNING, "Schedule file has vanished");
            else if (errno != ENOENT)
                logprintf(LOG_ERR, "Cannot stat schedule file %s: %s",
            
            config->schedulefile, strerror(errno));
            if (hadsched)
                disable_schedule(config);
            if (config->debug)
                logprintf(LOG_DEBUG, "(Schedule file stat failed)");

        } else if (memcmp(&old_st, &st, sizeof(st))) {
            int e = read_schedule(config);
            memcpy(&old_st, &st, sizeof(st));
            switch(e) {
            case 0: /* fail */
                if (hadsched)
                    disable_schedule(config);
                break;
            case 1: /* ok */
                if (hadsched && !config->numschedule)
                    disable_schedule(config);
                break;
            case 2: /* no file */
                if (hadsched) {
                    logprintf(LOG_WARNING, "Schedule file has vanished");
                    disable_schedule(config);
                }
                break;
            }
            if (config->debug)
                logprintf(LOG_DEBUG, "(Needed to reload schedule file)");

        } else if (config->debug)
            logprintf(LOG_DEBUG, "(Schedule file not changed)");
    }


    for (i = 0; i < config->numschedule; i++) {
        if (now >= config->schedule[i].t) {
            switch (config->schedule[i].action) {
            case SCHEDULE_START:
                start_command = 1;
                logprintf(LOG_NOTICE, "Recording (re)started by schedule");
                break;
            case SCHEDULE_STOP:
                stop_command = 1;
                logprintf(LOG_NOTICE, "Recording stopped by schedule");
                break;
            case SCHEDULE_OVERVIEW:
                overview_command = 1;
                logprintf(LOG_NOTICE, "Overview started by schedule");
                break;
            }
            config->schedule[i].t += 86400;
        }
    }
}


static int reset(config_t * config, int serial_fd) {
    static int count = 0;
    static time_t last_reset = 0;
    int i = 10000;
    char c;
    time_t t;

    while (save_buffer != -1) /* wait for FITS write to complete */
        msleep(1);
    buffer[0].size = buffer[1].size = 0;
    current_buffer = active_buffer = 0;

    hexdata(config, serial_fd, HEXDATA_RESET);
    message_length = in_data = in_message = 0;
    state = STOPPED;

    write_serial(serial_fd, RESET_STRING);
    /* discard bytes until timeout or 10kB read: */
    while (i && read_serial(serial_fd, &c))
        i--;

    /* detect continuous resetting: */
    t = time(NULL);
    if (t - last_reset < 5)
        count++;
    else
        count = 0;
    last_reset = t;
    if (count > 2) {
        logprintf(LOG_ERR, "Reset loop detected, terminating");
        terminate(0);
    }

    return i;
}

void init(config_t * config, int serial_fd) {
    char cmd[64];

    /* clockrate: */
    if (config->clocksource == 1) { /* internal */
	/* Internal clock 11.0592 MHz + prescaler 64x => 172800, 2
	   clock cycles per sample => 86400. Counter goes from 0 to N-1
	   (?) */
        int maxcounter = 86400 / config->samplerate - 1;
        sprintf(cmd, "GS%u\r", maxcounter);
	write_serial(serial_fd, cmd);
    } else if (config->clocksource == 2) { /* external */
	/* External clock 1 MHz => 1000000, 2 clock cycles per sample
	   => 500000. Counter goes from 0 to N-1 (?) */
	int maxcounter = 500000 / config->samplerate - 1;
	sprintf(cmd, "GA%u\r", maxcounter);
	write_serial(serial_fd, cmd);
    }

    /* gain, clocksource, chargepump: */
    sprintf(cmd, "T%u\rO%03u\rC%u\r",
	    config->clocksource, config->agclevel, config->chargepump);
    write_serial(serial_fd, cmd);
}

void start(config_t * config, int serial_fd) {
    char cmd[64];

    /* FOPA, channels, start, enable: */
    sprintf(cmd, "FS%02u%02u\rL%u\rS1\rGE\r",
	    config->focuscode, config->focuscode-1, config->nchannels);
    write_serial(serial_fd, cmd);
    state = STARTING;
}

void stop(int serial_fd) {
    write_serial(serial_fd, "GD\r");
    state = STOPPING;
}


typedef struct {
    double freq;
    int value;
} ovs_t;
#define MAX_OVS 13200
static ovs_t ovs[MAX_OVS];
static int ovsnum = 0;
static time_t ovstime = 0; 

static void start_overview(config_t * config, int serial_fd) {
    write_serial(serial_fd, "T0\rM2\r%5\rF0045.0\rL13200\rP2\r");
    state = OVERVIEW;
    ovsnum = 0;
    ovstime = time(NULL);
    if (config->debug)
	logprintf(LOG_DEBUG, "Started overview");
}
static void finish_overview(config_t * config) {
    char fname[PATH_MAX];
    struct tm tm;
    FILE *f;

    gmtime_r(&ovstime, &tm);
    snprintf(fname, PATH_MAX, "%s/OVS_%s_%04u%02u%02u_%02u%02u%02u.prn",
	     config->ovsdir, config->instrument,
	     tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
	     tm.tm_hour, tm.tm_min, tm.tm_sec);

    if ((f = fopen(fname, "a")) == NULL) {
	logprintf(LOG_ERR, "Cannot write overview file %s: %s",
		  fname, strerror(errno));
    } else {
	int i;
	fprintf(f, "Frequency[MHz];Amplitude RX1[mV] at pwm=%u\n",
		config->agclevel);
	for (i = 0; i < ovsnum; i++)
	    fprintf(f, "%7.3f;%u\n", ovs[i].freq, ovs[i].value);
	fflush(f);
	fclose(f);
    }

    state = STOPPED;
    if (config->debug)
	logprintf(LOG_DEBUG, "Overview finished");
}

static void handle_message(config_t * config, int serial_fd, const char *msg) {
    double ovs_freq;
    int ovs_value;

    if (!strcmp(msg, "CRX:Started")) {
	state = RUNNING;
	if (config->debug)
	    logprintf(LOG_DEBUG, "Recording loop started");
    } else if (!strcmp(msg, "CRX:Stopped")) {
	state = STOPPED;
	if (config->debug)
	    logprintf(LOG_DEBUG, "Recording loop stopped");
    } else if (strstr(msg, "CRX:e-Callisto ETH Zurich")) {
	/* hardware reset, do reset&init in software: */
	int do_start = (state == RUNNING);

	logprintf(LOG_WARNING, "Hardware reset detected, resetting software");

	reset(config, serial_fd);
	init(config, serial_fd);
	if (do_start)
	    start(config, serial_fd);

	/* (the channels are in eeprom => preserved across resets) */
    } else if (state == OVERVIEW
	       && sscanf(msg, "CRX:%lf,%d", &ovs_freq, &ovs_value) == 2) {
	if (ovsnum < MAX_OVS) {
	    ovs[ovsnum].freq = ovs_freq;
	    ovs[ovsnum].value = ovs_value;
	    ovsnum++;
	}
    }
}



static void hexdata(config_t * config, int serial_fd, signed char c) {
    static int value = 0;
    static int count = 0;
    static int end_marker = 0;

    if (c == HEXDATA_RESET) { /* reset */
	value = count = end_marker = 0;
	return;
    }

    if (c >= '0' && c <= '9') {
	value = (value << 4) | (c - '0');
	count++;
    } else if (c >= 'A' && c <= 'F') {
	value = (value << 4) | (0x0a + c - 'A');
	count++;
    } else {
	logprintf(LOG_ERR, "Invalid hex character '%c', resetting", c);
	reset(config, serial_fd);
	return;
    }

    if (count == 4) {
	if (value == 0x2323) {
	    end_marker++;
	    if (end_marker == 2 && config->debug)
		logprintf(LOG_DEBUG, "Hexdata end marker received");
	    else if (end_marker > 2) {
		logprintf(LOG_ERR, "Too many hexdata end markers, resetting");
		reset(config, serial_fd);
	    }
	    value = count = 0;
	    return;
	}

	if (value & ~0xff) {
	    logprintf(LOG_ERR, "Invalid hex value 0x%.4X, resetting", value);
	    reset(config, serial_fd);
	    return;
	} else {
	    int bsize = buffer[current_buffer].size; /* cache volatile value */
	    buffer[current_buffer].data[bsize]
		= (uint8_t)value;
	    if (bsize == 0)
		buffer[current_buffer].timestamp = get_usecs();
	    bsize++;
	    buffer[current_buffer].size = bsize; /* save cached value */
	    if (bsize == buffer_size
		|| (bsize > 0
		    && (bsize % config->nchannels) == 0
		    && switch_buffers)) {
		/* wait for FITS thread to finish if it is currently
		   saving the previous buffer: */
		while (save_buffer != -1)
		    msleep(1);
		/* signal FITS save thread: */
		save_buffer = current_buffer;
		/* swap buffers: */
		current_buffer = 1 - current_buffer;
		active_buffer = current_buffer;
		buffer[current_buffer].size = 0;

		switch_buffers = 0;
	    }
	}
	value = count = 0;
    }
}

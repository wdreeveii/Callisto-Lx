#ifndef CALLISTO_CONF_H
#define CALLISTO_CONF_H

#include <time.h>

/* Channels are stored in 4096-byte EEPROM, 8 bytes per channel: */
#define MAX_CHANNELS 512

/* This is the same as in the Windows host software: */
#define MAX_SCHEDULE 150

/* Supported callisto operating modes: */
#define SCHEDULE_STOP 0
#define SCHEDULE_START 3
#define SCHEDULE_OVERVIEW 8

typedef struct {
    time_t t;
    int action;
} schedule_t;

typedef struct {
    int valid;
    double f; /* frequency, in MHz */
    int lc;   /* number of integrations for lightcurves */
} channel_t;

typedef struct {
    const char *serialport;
    const char *instrument;
    const char *origin;
    const char *channelfile;
    const char *datadir;
    const char *ovsdir;
    const char *schedulefile;
    char *configdir;
    char *pidfile;

    double obs_long, obs_lat, obs_height;
    double local_oscillator; /* MHz */
    int chargepump;
    int agclevel;
    int clocksource;
    int filetime;
    int focuscode;
    int nchannels;       /* = sweep length */
    channel_t channels[MAX_CHANNELS];

    int samplerate;      /* samples / sec */
    int autostart;

    int numschedule;

    int net_port;

    uid_t server_uid;
    gid_t server_gid;
    int use_ipv4;
    int ipv6only;
    int do_upload;
    int check_only;
    int debug;
    int serial_debug;

    schedule_t schedule[MAX_SCHEDULE];
} config_t;

//extern config_t config;

//extern channel_t channels[MAX_CHANNELS];

//extern schedule_t schedule[MAX_SCHEDULE];
//extern int numschedule;

config_t default_config();
int read_configs(config_t *);
int read_schedule(config_t *config);

#endif

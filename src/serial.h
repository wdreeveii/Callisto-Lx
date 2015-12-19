#ifndef CALLISTO_SERIAL_H
#define CALLISTO_SERIAL_H

int init_serial(const char *devname);
int read_serial(int serial_fd, char *c);
int write_serial(int serial_fd, const char *s);

#endif

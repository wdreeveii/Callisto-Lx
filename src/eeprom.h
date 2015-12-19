#ifndef CALLISTO_EEPROM_H
#define CALLISTO_EEPROM_H

int upload_channels(config_t * config, int serial_fd);
int download_channels(config_t * config, int serial_fd);

#endif

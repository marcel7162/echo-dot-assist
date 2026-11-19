/* The clock from NTP where Home Assistant has not set it (ntp.c) */
#ifndef NTP_H
#define NTP_H
#include <stdint.h>

void ntp_start(void);                   /* main(), after settings_load: the thread */
int  ntp_enable(int set);               /* setting ntp_time (-1 reads); any thread */
int  ntp_query(const char *host, const char *port, uint32_t *unix_out);   /* one query; 0 = answered */
#endif

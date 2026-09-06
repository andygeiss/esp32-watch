/**
 * @file net.h
 * The radio, and the clock that arrives over it.
 *
 * Host-side counterpart: nothing. The simulator reads the Mac's own clock and
 * fakes the radio, so this file has no partner in the root main.c — it is
 * here because the device cannot do either for itself.
 */

#ifndef NET_H
#define NET_H

#include <stdbool.h>

/**
 * Set the timezone, and — if an SSID is configured — join the network and
 * start SNTP. Returns as soon as the radio is started; the association
 * happens behind it.
 */
void net_start(void);

/** Whether the station is associated and holds an address. */
bool net_is_up(void);

#endif /* NET_H */

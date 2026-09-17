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
#include <stdint.h>

/**
 * Set the timezone, and — if an SSID is configured — bring the radio up for
 * SNTP, which lets it go again once the clock is set or a minute has passed.
 * Returns as soon as the radio is started; the association happens behind it.
 */
void net_start(void);

/** Whether an SSID is configured at all, so the radio can ever come up. */
bool net_available(void);

/**
 * Ask for the radio, or let it go. The voice loop's hold: on when the
 * microphone hears speech, off when that speech was not for the watch or the
 * conversation is over. The radio is on while this or the boot hold wants it.
 */
void net_want(bool on);

/** Wait up to `ms` for an address. False at once if nobody wants the radio. */
bool net_wait_up(uint32_t ms);

/** Whether the station is associated and holds an address. */
bool net_is_up(void);

#endif /* NET_H */

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the traffic logger. Called automatically via library constructor;
// no-op unless PLEX_TRAFFIC_LOG environment variable is set to a writable path.
void traffic_logger_init(void);

#ifdef __cplusplus
}
#endif

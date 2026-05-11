/*
 * t3_features.h — compile-time feature availability flags.
 *
 * Generated semantics: CMake writes 1/0 values for each optional feature.
 * Consumers test the flag before calling the optional API:
 *
 *   #include "t3_features.h"
 *   #if T3_SHIM_SOCKS5_AVAILABLE
 *   #include "t3_shim_socks5.h"
 *   t3_shim_open(...);
 *   #endif
 *
 * Stability: each flag is 0 or 1 forever; flags are only ever added.
 */

#ifndef T3_FEATURES_H
#define T3_FEATURES_H

/* SOCKS5/CONNECT shim (Story 9-1, Epic 9 calls integration).
 * Set to 1 when CMake option T3_SHIM_SOCKS5=ON; 0 otherwise. */
#ifndef T3_SHIM_SOCKS5_AVAILABLE
#  define T3_SHIM_SOCKS5_AVAILABLE 0
#endif

#endif /* T3_FEATURES_H */

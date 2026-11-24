/*
 * Minimal config.h to build libmodbus v3.1.10 on Linux without autotools.
 * See third_party/libmodbus/VENDORED.md.
 */
#ifndef LIBMODBUS_CONFIG_H
#define LIBMODBUS_CONFIG_H

#define HAVE_ACCEPT4 1
#define HAVE_BYTESWAP_H 1
#define HAVE_DECL_TIOCM_RTS 1
#define HAVE_DECL_TIOCSRS485 1

/*
 * HAVE_STRLCPY is intentionally left undefined: libmodbus includes its own
 * fallback in modbus.c, which keeps compatibility with older glibc versions.
 */

#endif /* LIBMODBUS_CONFIG_H */

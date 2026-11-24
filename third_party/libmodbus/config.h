/*
 * config.h mínimo para compilar libmodbus v3.1.10 en Linux sin autotools.
 * Ver third_party/libmodbus/VENDORED.md.
 */
#ifndef LIBMODBUS_CONFIG_H
#define LIBMODBUS_CONFIG_H

#define HAVE_ACCEPT4 1
#define HAVE_BYTESWAP_H 1
#define HAVE_DECL_TIOCM_RTS 1
#define HAVE_DECL_TIOCSRS485 1

/*
 * HAVE_STRLCPY se deja sin definir a propósito: libmodbus incluye su propio
 * fallback en modbus.c, lo que mantiene la portabilidad a glibc antiguas.
 */

#endif /* LIBMODBUS_CONFIG_H */

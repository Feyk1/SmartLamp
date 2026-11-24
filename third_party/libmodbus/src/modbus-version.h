#ifndef MODBUS_VERSION_H
#define MODBUS_VERSION_H

/* Resolved libmodbus version: v3.1.10 (see VENDORED.md). */

#define LIBMODBUS_VERSION_MAJOR 3
#define LIBMODBUS_VERSION_MINOR 1
#define LIBMODBUS_VERSION_MICRO 10

/* Valor completo, como en el original (3.1.10). */
#define LIBMODBUS_VERSION 3.1.10

/* String form for concatenation. */
#define LIBMODBUS_VERSION_STRING "3.1.10"

/* Numerically encoded version, e.g. v1.2.3 -> 0x010203. */
#define LIBMODBUS_VERSION_HEX                                                                      \
    ((LIBMODBUS_VERSION_MAJOR << 16) | (LIBMODBUS_VERSION_MINOR << 8) |                            \
     (LIBMODBUS_VERSION_MICRO << 0))

/* Returns whether the version is greater than or equal to major.minor.micro. */
#define LIBMODBUS_VERSION_CHECK(major, minor, micro)                                               \
    (LIBMODBUS_VERSION_MAJOR > (major) ||                                                          \
     (LIBMODBUS_VERSION_MAJOR == (major) && LIBMODBUS_VERSION_MINOR > (minor)) ||                  \
     (LIBMODBUS_VERSION_MAJOR == (major) && LIBMODBUS_VERSION_MINOR == (minor) &&                  \
      LIBMODBUS_VERSION_MICRO >= (micro)))

#endif /* MODBUS_VERSION_H */

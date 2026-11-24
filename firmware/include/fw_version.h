#ifndef FW_VERSION_H
#define FW_VERSION_H

/* Application firmware version.
 *
 * The definitions can be overridden from CMake
 * (-DFW_VERSION_MAJOR=1 -DFW_VERSION_MINOR=1) for release/OTA builds.
 */
#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR 1
#endif
#ifndef FW_VERSION_MINOR
#define FW_VERSION_MINOR 0
#endif
#ifndef FW_VERSION_PATCH
#define FW_VERSION_PATCH 3
#endif

#define FW_STRINGIFY_(x) #x
#define FW_STRINGIFY(x) FW_STRINGIFY_(x)
#define FW_VERSION_STRING                 \
    FW_STRINGIFY(FW_VERSION_MAJOR) "."   \
    FW_STRINGIFY(FW_VERSION_MINOR) "."   \
    FW_STRINGIFY(FW_VERSION_PATCH)

#endif /* FW_VERSION_H */

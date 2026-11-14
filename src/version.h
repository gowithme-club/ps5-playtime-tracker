/* The payload version. This is the only place it lives: the Makefile reads it
 * for the artifact name, http.h for X-Client-Version and User-Agent.
 * Bump it with every change in behaviour on the console. */

#ifndef GWM_VERSION_H
#define GWM_VERSION_H

#define GWM_VERSION "1.0.0"

/* The name people see: in Payload Manager (which splits
 * gowithme.club_v<version>.elf into a name and a version), in notifications,
 * in the process list and in the data folder on the console. */
#define GWM_PRODUCT "gowithme.club"

/* "gowithme.club v1.0.0", as the payload introduces itself in the log. */
#define GWM_PRODUCT_VERSION GWM_PRODUCT " v" GWM_VERSION

#endif /* GWM_VERSION_H */

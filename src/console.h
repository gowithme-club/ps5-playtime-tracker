/* The console's name and its identifier.
 *
 * The name comes from the console's settings, e.g. "PS5-123".
 *
 * The site shows it instead of the device id, so it goes with the pairing
 * request and with every heartbeat: the console can be renamed. It is not a
 * key; the device id is.
 *
 * System parameter 6 (SCE_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME on PS4). The
 * buffer must be at least 65 bytes (the PS4 name limit): with 64 bytes the
 * service returns 0x80a10003 on firmware 13.40, with 128 it returns the name.
 * A failure is not cached: the name is asked again on the next heartbeat, and
 * once read it is kept for the rest of the run.
 *
 * The identifier tells the server that a console paired again is the same
 * one: after a reinstall the payload has no key and no state, and without it
 * the site would show the console twice and take its history twice. The name
 * cannot do this, it is editable and not unique. The identifier is a hash of
 * the console's OpenPSID, so the hardware id itself never leaves the console.
 *
 * Hashing is pure, covered by a host test.
 */

#ifndef GWM_CONSOLE_H
#define GWM_CONSOLE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sha256.h"

#define GWM_OPENPSID_LEN   16
/* 32 hex characters and the terminator. */
#define GWM_CONSOLE_ID_LEN 33

/* The prefix keeps the hash from matching one another service might compute
 * from the same OpenPSID. An all-zero id means the system gave none: then
 * there is no identifier. Returns 1 if out holds one. */
static int gwm_console_id_hash(const uint8_t psid[GWM_OPENPSID_LEN], char out[GWM_CONSOLE_ID_LEN])
{
	static const char prefix[] = "gowithme.club/ps5-console/v1:";
	static const char hex[] = "0123456789abcdef";
	uint8_t in[sizeof(prefix) - 1 + GWM_OPENPSID_LEN], digest[GWM_SHA256_LEN];
	int any = 0;

	out[0] = 0;
	for(int i = 0; i < GWM_OPENPSID_LEN; i++) any |= psid[i];
	if(!any) return 0;

	memcpy(in, prefix, sizeof(prefix) - 1);
	memcpy(in + sizeof(prefix) - 1, psid, GWM_OPENPSID_LEN);
	gwm_sha256(in, sizeof(in), digest);

	for(int i = 0; i < 16; i++)
	{
		out[i * 2] = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 0xF];
	}
	out[32] = 0;
	return 1;
}

#ifndef GWM_HOST_TEST

#include "log.h"

#define GWM_SYSTEM_PARAM_NAME 6

int sceSystemServiceParamGetString(int id, char *buf, size_t size);

#define GWM_CONSOLE_NAME_MAX 128

static char gwm_console_name_cache[GWM_CONSOLE_NAME_MAX];

static const char *gwm_console_name(void)
{
	static int attempts = 0;

	if(gwm_console_name_cache[0] || attempts >= 30) return gwm_console_name_cache;

	char name[sizeof(gwm_console_name_cache)] = "";
	int rc = sceSystemServiceParamGetString(GWM_SYSTEM_PARAM_NAME, name, sizeof(name));
	attempts++;

	if(rc == 0 && name[0])
	{
		snprintf(gwm_console_name_cache, sizeof(gwm_console_name_cache), "%s", name);
		gwm_log("console name: %s (attempt %d)", gwm_console_name_cache, attempts);
	}
	else if(attempts == 1 || attempts == 30)
	{
		gwm_log("console name unavailable (0x%08x, attempt %d)", rc, attempts);
	}
	return gwm_console_name_cache;
}

int sceKernelGetOpenPsId(uint8_t *psid);

/* An empty string if the system gives no id; asked once per run. */
static const char *gwm_console_id(void)
{
	static char id[GWM_CONSOLE_ID_LEN];
	static int asked = 0;

	if(asked) return id;
	asked = 1;

	/* Larger than the id, in case a firmware writes a longer struct. */
	uint8_t psid[64] = { 0 };
	int rc = sceKernelGetOpenPsId(psid);

	if(rc == 0 && gwm_console_id_hash(psid, id)) gwm_log("console id: %s", id);
	else gwm_log("console id unavailable (0x%08x)", rc);
	return id;
}

#endif /* GWM_HOST_TEST */

#endif /* GWM_CONSOLE_H */

/* The language of the payload's own notifications.
 *
 * A console set to Russian gets them in Russian; every other language gets
 * English. main.c reads the system language with sceSystemServiceParamGetInt
 * (parameter 1, which a Russian console answers with 8) at start and with
 * every heartbeat, so a change in the settings shows in the next one.
 * Notifications the server words itself, and the tile's page, which follows
 * the browser, are not decided here.
 *
 * GWM_TR picks a string; the choice is pure and covered by a host test.
 */

#ifndef GWM_LANG_H
#define GWM_LANG_H

#define GWM_LANG_EN 0
#define GWM_LANG_RU 1

/* The system's code for Russian. */
#define GWM_SYSTEM_LANG_RU 8

static int gwm_lang = GWM_LANG_EN;

#define GWM_TR(en, ru) (gwm_lang == GWM_LANG_RU ? (ru) : (en))

static int gwm_lang_of(int system_code)
{
	return system_code == GWM_SYSTEM_LANG_RU ? GWM_LANG_RU : GWM_LANG_EN;
}

#endif /* GWM_LANG_H */

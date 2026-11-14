/* A trophy from a notification.
 *
 * PS4 trophy files (trophy.img) are encrypted, but the system posts every
 * earned trophy as a notification in /system_data/priv/mms/notification2.db
 * with bundle_name = 'local:Trophy', and raw_data holds all the server needs:
 *
 *   telemetryData.parameters: isTrophyAttained, isTrophyLegacy, trophyGrade,
 *                             trophyId, trophyName, trophyNpCommIdLegacy
 *   viewData.parameters.game: trophySetName
 *   viewData.parameters:      secondaryText (trophy description)
 *   actions[].parameters:     actionUrl "pstc:browse?comm=NPWR...&trophy=N"
 *
 * Captured from a PS4 game on 13.40. For PS5 trophies the set key may be
 * named differently (without Legacy), so set and number also come from
 * actionUrl.
 *
 * Pure logic, covered by a host test on a real raw_data row
 * (tests/fixtures/trophy_ps4.json).
 */

#ifndef GWM_TROPHY_H
#define GWM_TROPHY_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "record.h"

/* Gets parameter name from a URL like "pstc:browse?comm=X&trophy=Y". */
static int gwm_url_param(const char *url, const char *name, char *out, size_t size)
{
	size_t nl = strlen(name);
	const char *p = strchr(url, '?');

	while(p)
	{
		p++;
		if(strncmp(p, name, nl) == 0 && p[nl] == '=')
		{
			p += nl + 1;
			size_t n = 0;
			while(p[n] && p[n] != '&' && n + 1 < size) { out[n] = p[n]; n++; }
			out[n] = 0;
			return n > 0;
		}
		p = strchr(p, '&');
	}
	return 0;
}

/* NPWR12345_00: four letters, five digits, underscore, two digits. The set
 * is a key on the server, so no garbage gets through. */
static int gwm_set_id_valid(const char *s)
{
	if(strlen(s) != 12) return 0;
	for(int i = 0; i < 4; i++)  if(!isupper((unsigned char)s[i])) return 0;
	for(int i = 4; i < 9; i++)  if(!isdigit((unsigned char)s[i])) return 0;
	if(s[9] != '_') return 0;
	return isdigit((unsigned char)s[10]) && isdigit((unsigned char)s[11]);
}

/* Parses raw_data. Returns 1 for an earned trophy with a valid set and
 * number. */
static int gwm_trophy_parse(const char *raw, gwm_trophy_t *t)
{
	char url[256], tmp[32];
	int attained = 0;

	memset(t, 0, sizeof(*t));

	/* Some trophy notifications are not about earning one (progress, for
	 * example). A present, false flag means skip. */
	if(gwm_json_get_bool(raw, "isTrophyAttained", &attained) && !attained) return 0;

	gwm_json_get_bool(raw, "isTrophyLegacy", &t->legacy);

	if(!gwm_json_get_str(raw, "trophyNpCommIdLegacy", t->set_id, sizeof(t->set_id)) || !t->set_id[0])
		gwm_json_get_str(raw, "trophyNpCommId", t->set_id, sizeof(t->set_id));

	int have_id = gwm_json_get_i64(raw, "trophyId", &t->id);

	if(gwm_json_get_str(raw, "actionUrl", url, sizeof(url)))
	{
		if(!gwm_set_id_valid(t->set_id))
			gwm_url_param(url, "comm", t->set_id, sizeof(t->set_id));
		if(!have_id && gwm_url_param(url, "trophy", tmp, sizeof(tmp)))
		{
			t->id = strtoll(tmp, NULL, 10);
			have_id = 1;
		}
	}

	if(!gwm_set_id_valid(t->set_id) || !have_id || t->id < 0) return 0;

	/* The system capitalizes the grade ("Bronze"); the server expects lowercase. */
	if(gwm_json_get_str(raw, "trophyGrade", t->grade, sizeof(t->grade)))
		for(char *c = t->grade; *c; c++) *c = (char)tolower((unsigned char)*c);
	if(strcmp(t->grade, "bronze") && strcmp(t->grade, "silver") &&
	   strcmp(t->grade, "gold") && strcmp(t->grade, "platinum"))
		t->grade[0] = 0;

	gwm_json_get_str(raw, "trophyName", t->name, sizeof(t->name));
	gwm_json_get_str(raw, "secondaryText", t->detail, sizeof(t->detail));
	gwm_json_get_str(raw, "trophySetName", t->set_name, sizeof(t->set_name));

	return 1;
}

#endif /* GWM_TROPHY_H */

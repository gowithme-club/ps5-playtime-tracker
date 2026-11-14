#include <stdlib.h>

#include "check.h"
#include "trophy2.h"

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if(!f) return NULL;
	static uint8_t buf[65536];
	*len = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[*len] = 0;
	return buf;
}

/* A file from TROPHY.UCP as a string, like gwm_t2_read_at on the console. */
static char *ucp_json(const uint8_t *ucp, size_t n, const char *name)
{
	uint64_t off, size;
	if(!gwm_ucp_find(ucp, n, name, &off, &size) || off + size > n) return NULL;
	char *s = malloc(size + 1);
	memcpy(s, ucp + off, size);
	s[size] = 0;
	return s;
}

int main(void)
{
	gwm_t2_item_t items[GWM_T2_MAX];
	char iso[GWM_ISO_LEN];
	size_t n;

	/* Darwin's Paradox from the console (user and console ids scrubbed):
	 * #6 and #19 earned, the other 18 not. The file uses the journal clock;
	 * an offset of 254 s gives the notification times 01:21:13 and 01:18:34. */
	uint8_t *dat = slurp("tests/fixtures/trptitle_ps5.dat", &n);
	CHECK(dat != NULL);
	size_t dat_len = n;
	uint8_t *orig = malloc(n), *copy = malloc(n);
	memcpy(orig, dat, n);
	memcpy(copy, dat, n);

	CHECK(gwm_t2_parse(copy, n, 254, items, GWM_T2_MAX) == 2);
	CHECK(items[0].id == 6);
	gwm_iso_from_unix(iso, items[0].earned);
	CHECK_STR(iso, "2026-10-08T01:21:13Z");
	CHECK(items[1].id == 19);
	gwm_iso_from_unix(iso, items[1].earned);
	CHECK_STR(iso, "2026-10-08T01:18:34Z");

	/* Room for one: the first is taken. */
	CHECK(gwm_t2_parse(copy, n, 0, items, 1) == 1);

	/* A foreign file, a truncated file, a broken record. */
	CHECK(gwm_t2_parse((const uint8_t *)"T2PDxxxx", 8, 0, items, GWM_T2_MAX) == -1);
	CHECK(gwm_t2_parse(copy, 0x1d00, 0, items, GWM_T2_MAX) == -1);
	copy[0x1de0 + 2] = 0x07;
	CHECK(gwm_t2_parse(copy, n, 0, items, GWM_T2_MAX) == -1);
	copy[0] = 'X';
	CHECK(gwm_t2_parse(copy, n, 0, items, GWM_T2_MAX) == -1);

	/* TROPHY.UCP: table of contents and the JSON inside. */
	uint8_t *ucp = malloc(65536);
	memcpy(ucp, slurp("tests/fixtures/trophy_ps5.ucp", &n), n);
	char *conf = ucp_json(ucp, n, "tropconf.json");
	char *meta = ucp_json(ucp, n, "tropmeta_en-US.json");
	CHECK(conf != NULL);
	CHECK(meta != NULL);
	CHECK(ucp_json(ucp, n, "tropmeta_ru-RU.json") == NULL);

	char title[GWM_TITLE_LEN] = "";
	CHECK(gwm_ucp_title(meta, title, sizeof(title)));
	CHECK_STR(title, "Darwin's Paradox");

	gwm_trophy_t t;
	memset(&t, 0, sizeof(t));
	gwm_ucp_trophy(conf, meta, 6, &t);
	CHECK_STR(t.grade, "bronze");
	CHECK(!t.hidden);
	CHECK(t.name[0] != 0);

	/* Platinum is the first object; #1 is hidden, and the unlock condition
	 * nested in the object does not get in the way. */
	memset(&t, 0, sizeof(t));
	gwm_ucp_trophy(conf, meta, 0, &t);
	CHECK_STR(t.grade, "platinum");
	CHECK_STR(t.name, "PARADOX FIXER");
	memset(&t, 0, sizeof(t));
	gwm_ucp_trophy(conf, meta, 1, &t);
	CHECK_STR(t.grade, "silver");
	CHECK(t.hidden);

	/* Without the config: only id and time. */
	memset(&t, 0, sizeof(t));
	gwm_ucp_trophy(NULL, NULL, 6, &t);
	CHECK_STR(t.grade, "");

	/* Body: the whole set in one line. */
	char line[GWM_RECORD_MAX * 4];
	int next = -1;
	CHECK(gwm_t2_parse(orig, dat_len, 254, items, GWM_T2_MAX) == 2);
	size_t len = gwm_t2_build_line(line, sizeof(line), "Player", "NPWR50690_00", title, conf, meta,
	                               items, 2, 0, &next);
	CHECK(len == strlen(line));
	CHECK(next == 2);
	CHECK(strstr(line, "{\"account\":\"Player\",\"platform\":\"ps5-cfw\",\"setId\":\"NPWR50690_00\","
	                   "\"title\":\"Darwin's Paradox\",\"trophies\":[{\"id\":6,"
	                   "\"earnedAt\":\"2026-10-08T01:21:13Z\",\"grade\":\"bronze\",\"name\":\"") == line);
	CHECK(strstr(line, "},{\"id\":19,\"earnedAt\":\"2026-10-08T01:18:34Z\"") != NULL);
	CHECK(!strcmp(line + len - 3, "}]}"));

	/* Tight: the set splits into lines by trophy, each a body of its own. */
	size_t small = strstr(line, ",{\"id\":19") - line + 4;
	len = gwm_t2_build_line(line, small, "Player", "NPWR50690_00", title, conf, meta, items, 2, 0, &next);
	CHECK(len > 0 && next == 1);
	CHECK(!strcmp(line + len - 2, "]}"));
	len = gwm_t2_build_line(line, sizeof(line), "Player", "NPWR50690_00", title, conf, meta, items, 2, next, &next);
	CHECK(len > 0 && next == 2);
	CHECK(strstr(line, "\"trophies\":[{\"id\":19,") != NULL);

	/* Not even one fits: 0, not an empty array. */
	CHECK(gwm_t2_build_line(line, 120, "Player", "NPWR50690_00", title, conf, meta, items, 2, 0, &next) == 0);

	return gwm_report("trophy2");
}

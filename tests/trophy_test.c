#include <stdlib.h>

#include "check.h"
#include "trophy.h"

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if(!f) return NULL;
	static char buf[65536];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

int main(void)
{
	gwm_trophy_t t;
	char out[GWM_RECORD_MAX];

	/* A real notification from the console: BioShock Infinite, first bronze */
	char *raw = slurp("tests/fixtures/trophy_ps4.json");
	CHECK(raw != NULL);

	CHECK(gwm_trophy_parse(raw, &t));
	CHECK_STR(t.set_id, "NPWR10824_00");
	CHECK(t.id == 1);
	CHECK(t.legacy == 1);
	CHECK_STR(t.grade, "bronze");
	CHECK_STR(t.name, "Written in the Clouds");
	CHECK_STR(t.detail, "In the Main Campaign, completed Lighthouse.");
	CHECK_STR(t.set_name, "BioShock Infinite: The Complete Edition");

	size_t n = gwm_build_trophy(out, sizeof(out), "Player", &t, "2026-10-07T17:45:52Z");
	CHECK(n == strlen(out));
	CHECK_STR(out, "{\"account\":\"Player\",\"platform\":\"ps5-cfw\",\"setId\":\"NPWR10824_00\","
	               "\"title\":\"BioShock Infinite: The Complete Edition\","
	               "\"trophies\":[{\"id\":1,\"earnedAt\":\"2026-10-07T17:45:52Z\","
	               "\"name\":\"Written in the Clouds\","
	               "\"detail\":\"In the Main Campaign, completed Lighthouse.\","
	               "\"grade\":\"bronze\"}]}");

	/* A PS5 trophy without the Legacy key: set and number come from actionUrl */
	const char *ps5 = "{\"telemetryData\":{\"parameters\":{\"isTrophyAttained\":true,"
	                  "\"isTrophyLegacy\":false,\"trophyGrade\":\"Gold\"}},"
	                  "\"viewData\":{\"actions\":[{\"parameters\":{\"actionUrl\":"
	                  "\"pstc:browse?comm=NPWR20188_00&gen=2&trophy=12\"}}]}}";
	CHECK(gwm_trophy_parse(ps5, &t));
	CHECK_STR(t.set_id, "NPWR20188_00");
	CHECK(t.id == 12 && t.legacy == 0);
	CHECK_STR(t.grade, "gold");

	/* not an unlock, or a garbage set: not a trophy */
	CHECK(!gwm_trophy_parse("{\"isTrophyAttained\":false,\"trophyId\":\"1\","
	                        "\"trophyNpCommIdLegacy\":\"NPWR10824_00\"}", &t));
	CHECK(!gwm_trophy_parse("{\"trophyId\":\"1\",\"trophyNpCommIdLegacy\":\"../etc\"}", &t));
	CHECK(!gwm_trophy_parse("{\"channelType\":\"Downloads\"}", &t));

	/* an unknown grade is not sent */
	CHECK(gwm_trophy_parse("{\"trophyId\":2,\"trophyNpCommId\":\"NPWR00001_00\","
	                       "\"trophyGrade\":\"Mythic\"}", &t));
	CHECK(t.grade[0] == 0);

	return gwm_report("trophy");
}

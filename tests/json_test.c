#include "check.h"
#include "json.h"

int main(void)
{
	char buf[256], *p;

	/* build */
	p = gwm_json_kv(buf, buf + sizeof(buf), "title", "Say \"hi\"\n\\");
	CHECK_STR(buf, "\"title\":\"Say \\\"hi\\\"\\n\\\\\"");
	CHECK(p == buf + strlen(buf));

	gwm_i64(buf, buf + sizeof(buf), -1234567890123LL);
	CHECK_STR(buf, "-1234567890123");
	gwm_i64(buf, buf + sizeof(buf), 0);
	CHECK_STR(buf, "0");

	/* overflow stays within the buffer */
	char tiny[8];
	gwm_lit(tiny, tiny + sizeof(tiny), "0123456789");
	CHECK_STR(tiny, "0123456");

	/* read */
	const char *doc = "{\"a\":{\"trophyId\":\"17\",\"n\":-5,\"flag\": true,"
	                  "\"name\" : \"\\u00c9\\u00e8\\u00ea\\u00eb \\ud83c\\udfc6 \\/x\","
	                  "\"text\":\"trophyId\"}}";
	int64_t v = 0;
	int b = 0;

	CHECK(gwm_json_get_i64(doc, "trophyId", &v) && v == 17);
	CHECK(gwm_json_get_i64(doc, "n", &v) && v == -5);
	CHECK(gwm_json_get_bool(doc, "flag", &b) && b == 1);
	CHECK(!gwm_json_get_bool(doc, "missing", &b));

	CHECK(gwm_json_get_str(doc, "name", buf, sizeof(buf)));
	CHECK_STR(buf, "Éèêë 🏆 /x");

	/* "trophyId" as a value inside a string is not a key */
	CHECK(gwm_json_get_str(doc, "text", buf, sizeof(buf)));
	CHECK_STR(buf, "trophyId");

	/* cut on a character boundary: "Éèê" is 6 bytes, a 6-byte buffer holds "Éè" */
	char small[6];
	CHECK(gwm_json_get_str(doc, "name", small, sizeof(small)));
	CHECK_STR(small, "Éè");

	/* raw UTF-8 is cut on a character boundary too */
	const char *raw = "{\"t\":\"Ééé\"}";
	char small2[4];
	CHECK(gwm_json_get_str(raw, "t", small2, sizeof(small2)));
	CHECK_STR(small2, "É");

	return gwm_report("json");
}

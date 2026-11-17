#include "check.h"
#include "tile.h"
#include "web.h"

int main(void)
{
	char method[8], path[64], query[64], host[64], out[GWM_WEB_STATE_MAX];

	/* The console's browser asking for the page. */
	const char *req = "GET /state?lang=en HTTP/1.1\r\nhost: 127.0.0.1:7531\r\nAccept: */*\r\n\r\n";
	CHECK(gwm_web_parse(req, strlen(req), method, sizeof(method), path, sizeof(path), query, sizeof(query), host, sizeof(host)));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/state");
	CHECK_STR(query, "lang=en");
	CHECK_STR(host, "127.0.0.1:7531");

	/* Not all of the head yet. */
	const char *part = "POST /pair HTTP/1.1\r\nHost: localhost\r\n";
	CHECK(!gwm_web_parse(part, strlen(part), method, sizeof(method), path, sizeof(path), query, sizeof(query), host, sizeof(host)));

	CHECK_STR(gwm_web_lang("lang=en"), "en");
	CHECK_STR(gwm_web_lang("t=1&lang=en"), "en");
	CHECK_STR(gwm_web_lang("lang=ru"), "ru");
	CHECK_STR(gwm_web_lang("lang=english"), "ru");
	CHECK_STR(gwm_web_lang(""), "ru");

	/* The site's language is in the address. */
	gwm_web_localize("https://gowithme.club/player/fire", "en", out, sizeof(out));
	CHECK_STR(out, "https://gowithme.club/en/player/fire");
	gwm_web_localize("https://gowithme.club/player/fire", "ru", out, sizeof(out));
	CHECK_STR(out, "https://gowithme.club/player/fire");

	CHECK(gwm_web_host_ok("127.0.0.1:7531"));
	CHECK(gwm_web_host_ok("localhost"));
	CHECK(gwm_web_host_ok("LOCALHOST:7531"));
	/* A name that only resolves to 127.0.0.1 (DNS rebinding) is someone else's page. */
	CHECK(!gwm_web_host_ok("127.0.0.1.attacker.example:7531"));
	CHECK(!gwm_web_host_ok("attacker.example:7531"));
	CHECK(!gwm_web_host_ok(""));

	/* Not linked: the code and its QR code. */
	gwm_web_view_t v = { "code", "042917", -1, 0, "" };
	size_t n = gwm_web_state_json(out, sizeof(out), &v, "ru");
	CHECK(n == strlen(out));
	CHECK(!strncmp(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"code\",\"code\":\"042917\",\"qr\":[\"1111111", 46 + strlen(GWM_VERSION)));
	CHECK(strstr(out, "],\"syncAgo\":-1,\"queued\":0}") != NULL);

	/* The rows are square: as many rows as modules in each. */
	const char *row = strstr(out, "\"qr\":[\"") + 7;
	size_t width = (size_t)(strchr(row, '"') - row);
	int rows = 0;
	for(const char *p = row - 1; (p = strchr(p, '"')) && p < strchr(row, ']'); p = strchr(p + 1, '"') + 1) rows++;
	CHECK(width >= 21 && width % 4 == 1);
	CHECK((size_t)rows == width);

	/* Linked: no code; the profile address in the page's language, escaped as
	 * JSON, and its QR code. */
	gwm_web_view_t linked = { "linked", "", 75, 512, "https://gowithme.club/player/a\"b" };
	gwm_web_state_json(out, sizeof(out), &linked, "en");
	CHECK(!strncmp(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"linked\",\"syncAgo\":75,\"queued\":512,"
	                    "\"profileUrl\":\"https://gowithme.club/en/player/a\\\"b\",\"profileQr\":[\"1111111",
	                    82 + strlen(GWM_VERSION)));
	CHECK(!strcmp(out + strlen(out) - 3, "\"]}"));

	/* No profile yet: neither field. */
	gwm_web_view_t bare = { "linked", "", -1, 0, "" };
	gwm_web_state_json(out, sizeof(out), &bare, "ru");
	CHECK_STR(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"linked\",\"syncAgo\":-1,\"queued\":0}");

	/* The tile. */
	gwm_tile_param(out, sizeof(out), 7531);
	CHECK_STR(out, "{\"titleId\":\"GWMC00001\",\"applicationCategoryType\":65536,"
	               "\"deeplinkUri\":\"http://127.0.0.1:7531/\","
	               "\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"gowithme.club\"}}}\n");
	CHECK(gwm_tile_param(out, 16, 7531) == 0);

	CHECK(gwm_tile_plan(0, 0, 0) == GWM_TILE_REGISTER);   /* first start */
	CHECK(gwm_tile_plan(0, 0, 1) == GWM_TILE_DELETED);    /* the person deleted it */
	CHECK(gwm_tile_plan(1, 1, 1) == GWM_TILE_KEEP);
	CHECK(gwm_tile_plan(1, 0, 1) == GWM_TILE_REGISTER);   /* new icon or port */
	CHECK(gwm_tile_plan(1, 1, 0) == GWM_TILE_REGISTER);   /* files written, register failed */

	return gwm_report("web+tile");
}

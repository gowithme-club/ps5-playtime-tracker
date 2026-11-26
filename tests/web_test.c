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
	gwm_web_view_t v = { "code", "042917", "", -1, 0, "", -1, -1, -1, 0 };
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
	gwm_web_view_t linked = { "linked", "", "", 75, 512, "https://gowithme.club/player/a\"b", -1, -1, -1, 0 };
	gwm_web_state_json(out, sizeof(out), &linked, "en");
	CHECK(!strncmp(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"linked\",\"syncAgo\":75,\"queued\":512,"
	                    "\"profileUrl\":\"https://gowithme.club/en/player/a\\\"b\",\"profileQr\":[\"1111111",
	                    82 + strlen(GWM_VERSION)));
	CHECK(!strcmp(out + strlen(out) - 3, "\"]}"));

	/* No profile yet: neither field. */
	gwm_web_view_t bare = { "linked", "", "", -1, 0, "", -1, -1, -1, 0 };
	gwm_web_state_json(out, sizeof(out), &bare, "ru");
	CHECK_STR(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"linked\",\"syncAgo\":-1,\"queued\":0}");

	/* What the console recorded, once the server has counted it. */
	bare.rec_seconds = 261240; bare.rec_games = 42; bare.rec_trophies = 318;
	gwm_web_state_json(out, sizeof(out), &bare, "ru");
	CHECK_STR(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"linked\",\"syncAgo\":-1,\"queued\":0,"
	               "\"recorded\":{\"seconds\":261240,\"games\":42,\"trophies\":318}}");

	/* Someone else at a linked console: their profile's name and the code
	 * that claims it, with the same QR code as pairing; no profile of anyone. */
	gwm_web_view_t claim = { "claim", "042137", "Mira", 12, 0, "", 0, 0, 0, 0 };
	gwm_web_state_json(out, sizeof(out), &claim, "ru");
	CHECK(!strncmp(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"claim\",\"profile\":\"Mira\","
	                    "\"code\":\"042137\",\"qr\":[\"1111111", 64 + strlen(GWM_VERSION)));
	CHECK(strstr(out, "profileUrl") == NULL);
	/* Before the code arrives: the name alone. */
	claim.code[0] = 0;
	gwm_web_state_json(out, sizeof(out), &claim, "ru");
	CHECK_STR(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"claim\",\"profile\":\"Mira\",\"syncAgo\":12,"
	               "\"queued\":0,\"recorded\":{\"seconds\":0,\"games\":0,\"trophies\":0}}");

	/* The welcome screen, until it is read. */
	gwm_web_view_t welcome = { "connecting", "", "", -1, 0, "", -1, -1, -1, 1 };
	gwm_web_state_json(out, sizeof(out), &welcome, "en");
	CHECK_STR(out, "{\"version\":\"" GWM_VERSION "\",\"state\":\"connecting\",\"syncAgo\":-1,\"queued\":0,\"welcome\":true}");

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

	/* Background blocks: a header; A three times; B and C as they are; then
	 * three blocks copied from three back. */
	unsigned char packed[GWM_TILE_PIC_HEAD + 20 + 36 + 8], wide[GWM_TILE_PIC_HEAD + 8 * 16];
	memset(packed, 'h', GWM_TILE_PIC_HEAD);
	unsigned char *p = packed + GWM_TILE_PIC_HEAD;
	p[0] = 3; p[1] = 0; p[2] = 0; p[3] = 0x80; memset(p + 4, 'A', 16); p += 20;
	p[0] = 2; p[1] = 0; p[2] = 0; p[3] = 0; memset(p + 4, 'B', 16); memset(p + 20, 'C', 16); p += 36;
	p[0] = 3; p[1] = 0; p[2] = 0; p[3] = 0x40; p[4] = 3; p[5] = 0; p[6] = 0; p[7] = 0;
	CHECK(gwm_tile_unpack(packed, sizeof(packed), wide, sizeof(wide)) == sizeof(wide));
	const char *blocks = "AAABCABC";
	for(int i = 0; i < 8; i++) CHECK(wide[GWM_TILE_PIC_HEAD + i * 16] == blocks[i] && wide[GWM_TILE_PIC_HEAD + i * 16 + 15] == blocks[i]);
	CHECK(wide[0] == 'h' && wide[GWM_TILE_PIC_HEAD - 1] == 'h');
	CHECK(gwm_tile_unpack(packed, sizeof(packed) - 1, wide, sizeof(wide)) == 0);  /* cut short */
	CHECK(gwm_tile_unpack(packed, sizeof(packed), wide, sizeof(wide) - 16) == 0); /* does not fit */
	p[4] = 6;                                                                     /* before the start */
	CHECK(gwm_tile_unpack(packed, sizeof(packed), wide, sizeof(wide)) == 0);

	/* The background in assets/ unpacks to a whole BC7 DDS. */
	static unsigned char file[256 * 1024], dds[GWM_TILE_PIC_SIZE];
	FILE *f = fopen("assets/pic0.dds.rle", "rb");
	size_t got = f ? fread(file, 1, sizeof(file), f) : 0;
	if(f) fclose(f);
	CHECK(got > GWM_TILE_PIC_HEAD && got < sizeof(file));
	CHECK(gwm_tile_unpack(file, got, dds, sizeof(dds)) == GWM_TILE_PIC_SIZE);
	CHECK(!memcmp(dds, "DDS ", 4) && !memcmp(dds + 84, "DX10", 4) && dds[128] == 98);

	return gwm_report("web+tile");
}

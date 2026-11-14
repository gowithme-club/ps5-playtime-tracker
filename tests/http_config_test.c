#include "check.h"
#include "config.h"
#include "http.h"

int main(void)
{
	gwm_config_t c;
	char text[] =
		"# comment\n"
		"token = \"abc123\"\r\n"
		"account=  Player \n"
		"popups = OFF\n"
		"host = 192.168.1.10\n"
		"port = 8080\n"
		"unknown = 1\n"
		"; another comment";

	gwm_cfg_parse(&c, text);
	CHECK(c.valid);
	CHECK_STR(c.token, "abc123");
	CHECK_STR(c.account, "Player");
	CHECK_STR(c.host, "192.168.1.10");
	CHECK(c.port == 8080);
	CHECK(c.popups == 0);

	char empty[] = "popups = maybe\nport = 99999\n";
	gwm_cfg_parse(&c, empty);
	CHECK(!c.valid);
	CHECK_STR(c.host, "gowithme.club");
	CHECK(c.port == 80);
	CHECK(c.popups == 1);

	char head[GWM_HTTP_HEAD_MAX];
	size_t n = gwm_build_request(head, sizeof(head), "/ingest/v1/heartbeat",
	                             "gowithme.club", 80, "tok", 42);
	CHECK(n == strlen(head));
	CHECK(strstr(head, "POST /ingest/v1/heartbeat HTTP/1.1\r\nHost: gowithme.club\r\n") == head);
	CHECK(strstr(head, "Authorization: Bearer tok\r\n") != NULL);
	CHECK(strstr(head, "X-Client-Version: " GWM_VERSION "\r\n") != NULL);
	CHECK(strstr(head, "Content-Length: 42\r\n") != NULL);
	CHECK(strstr(head, "\r\n\r\n") == head + n - 4);

	gwm_build_request(head, sizeof(head), "/x", "10.0.0.1", 8080, "t", 0);
	CHECK(strstr(head, "Host: 10.0.0.1:8080\r\n") != NULL);

	CHECK(gwm_parse_status("HTTP/1.1 200 OK\r\n", 17) == 200);
	CHECK(gwm_parse_status("HTTP/1.0 429 Too Many\r\n", 23) == 429);
	CHECK(gwm_parse_status("garbage here", 12) == GWM_HTTP_ERR_PARSE);

	return gwm_report("http+config");
}

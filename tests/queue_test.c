#include <stdlib.h>

#include "check.h"
#include "queue.h"

int main(void)
{
	char dir[] = "/tmp/gwm_queue_XXXXXX";
	char path[256], buf[256];

	CHECK(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/q.jsonl", dir);

	CHECK(gwm_queue_read(path, buf, sizeof(buf)) == 0);

	CHECK(gwm_queue_append(path, "{\"a\":1}\n", 8) == 0);
	CHECK(gwm_queue_append(path, "{\"b\":2}\n", 8) == 0);
	/* an unfinished line after a power cut */
	CHECK(gwm_queue_append(path, "{\"c\":", 5) == 0);

	size_t got = gwm_queue_read(path, buf, sizeof(buf));
	CHECK(got == 16);
	CHECK_STR(buf, "{\"a\":1}\n{\"b\":2}\n");

	/* read no more than the buffer, still on a line boundary */
	char small[12];
	CHECK(gwm_queue_read(path, small, sizeof(small)) == 8);

	CHECK(gwm_queue_consume(path, 8) == 0);
	got = gwm_queue_read(path, buf, sizeof(buf));
	CHECK_STR(buf, "{\"b\":2}\n");

	/* A new line after a crash does not stick to a broken tail. */
	CHECK(gwm_queue_append(path, "{\"d\":4}\n", 8) == 0);
	got = gwm_queue_read(path, buf, sizeof(buf));
	CHECK_STR(buf, "{\"b\":2}\n{\"d\":4}\n");

	CHECK(gwm_queue_consume(path, got) == 0);
	CHECK(gwm_file_size(path) == 0);

	/* Even a queue holding only an unfinished JSON accepts lines again. */
	CHECK(gwm_queue_append(path, "{\"broken\":", 10) == 0);
	CHECK(gwm_queue_append(path, "{\"ok\":1}\n", 9) == 0);
	CHECK(gwm_queue_read(path, buf, sizeof(buf)) == 9);
	CHECK_STR(buf, "{\"ok\":1}\n");
	CHECK(gwm_queue_consume(path, 9) == 0);

	rmdir(dir);
	return gwm_report("queue");
}

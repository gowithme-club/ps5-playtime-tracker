/* State kept between runs: the last trophy notification processed and the
 * last journal row read for sessions.
 *
 * A single "key = value" file, rewritten whole through a temp file and
 * rename. Losing it is mostly harmless: notifications and the journal are
 * read again, and the server is idempotent, so a repeated trophy lands in
 * known rather than new. Journal sessions are the exception: their
 * timestamps depend on the journal clock offset (journal.h), and if that
 * moved by even a second the server would see new sessions.
 */

#ifndef GWM_STATE_H
#define GWM_STATE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

#define GWM_STATE_FILE GWM_DATA_DIR "/state.txt"

typedef struct
{
	int64_t notif_last_id;
	int64_t journal_last_row;
	char journal_epoch[32];
} gwm_state_t;

static void gwm_state_load(gwm_state_t *s)
{
	char line[128];
	FILE *f = fopen(GWM_STATE_FILE, "r");

	memset(s, 0, sizeof(*s));
	if(!f) return;

	while(fgets(line, sizeof(line), f))
		if(!strncmp(line, "notif_last_id = ", 16))
			s->notif_last_id = strtoll(line + 16, NULL, 10);
		else if(!strncmp(line, "journal_last_row = ", 19))
			s->journal_last_row = strtoll(line + 19, NULL, 10);
		else if(!strncmp(line, "journal_epoch = ", 16))
		{
			line[strcspn(line, "\r\n")] = 0;
			snprintf(s->journal_epoch, sizeof(s->journal_epoch), "%.31s", line + 16);
		}

	fclose(f);
}

static void gwm_state_save(const gwm_state_t *s)
{
	const char *tmp = GWM_STATE_FILE ".tmp";
	FILE *f = fopen(tmp, "w");

	if(!f) return;
	fprintf(f, "notif_last_id = %lld\n", (long long)s->notif_last_id);
	fprintf(f, "journal_last_row = %lld\n", (long long)s->journal_last_row);
	fprintf(f, "journal_epoch = %s\n", s->journal_epoch);
	int failed = fflush(f) != 0 || ferror(f);
	fsync(fileno(f));
	fclose(f);

	if(failed) unlink(tmp);
	else rename(tmp, GWM_STATE_FILE);
}

#endif /* GWM_STATE_H */

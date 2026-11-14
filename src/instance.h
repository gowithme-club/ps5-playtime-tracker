/* One copy of the payload per console.
 *
 * Two copies would send every session and heartbeat twice, and the payload
 * gets launched again all the time: a new version, a second tap in Payload
 * Manager, autoload on top of a manual launch.
 *
 * The new copy replaces the old one. The running copy holds a flock on the
 * lock file; a new copy that cannot get it drops a stop file next to it and
 * waits. The old copy checks for the stop file every second and exits,
 * releasing the lock. Processes are never killed by pid: elfldr names every
 * payload payload.elf, and a wrong pid would hit someone else's payload. The
 * kernel releases the lock when the process dies, so it never goes stale.
 */

#ifndef GWM_INSTANCE_H
#define GWM_INSTANCE_H

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

#define GWM_LOCK_FILE GWM_DATA_DIR "/lock"
#define GWM_STOP_FILE GWM_DATA_DIR "/stop"

#define GWM_TAKEOVER_WAIT_S 30

static int gwm_lock_fd = -1;

static int gwm_instance_acquire(void)
{
	gwm_lock_fd = open(GWM_LOCK_FILE, O_CREAT | O_RDWR, 0666);
	if(gwm_lock_fd < 0) return 0;

	if(flock(gwm_lock_fd, LOCK_EX | LOCK_NB) == 0)
	{
		unlink(GWM_STOP_FILE);
		return 1;
	}

	gwm_log("another copy is running, asking it to exit");
	int fd = open(GWM_STOP_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if(fd >= 0) close(fd);

	for(int i = 0; i < GWM_TAKEOVER_WAIT_S; i++)
	{
		sleep(1);
		if(flock(gwm_lock_fd, LOCK_EX | LOCK_NB) == 0)
		{
			unlink(GWM_STOP_FILE);
			return 1;
		}
	}

	unlink(GWM_STOP_FILE);
	return 0;
}

/* Whether a newer copy asked us to step aside. */
static int gwm_instance_stop_requested(void)
{
	struct stat st;
	return stat(GWM_STOP_FILE, &st) == 0;
}

#endif /* GWM_INSTANCE_H */

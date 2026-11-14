# Repository Guidelines

Instructions for anyone, human or agent, working in this repository.

## What this is

A payload for a jailbroken PS5, launched through elfldr or Payload Manager. It detects the running game, measures play sessions, picks up earned trophies and sends them to the gowithme.club ingest API. It is a single-threaded process with a 10-second poll loop (`src/main.c`); every module is a header in `src/`, and each header's top comment explains its design.

Everything sent to the server carries the platform key `ps5-cfw`, not `ps5`, which is the server's official PSN integration. The server owns all domain logic; this client only forwards events.

## Language and documents

Everything in the repository is in English: code, comments, log lines, notifications, commit messages. `README.md` and its Russian translation `README.ru.md` are the public pages for people installing the payload; keep them in step. `CHANGELOG.md` records user-visible changes per version.

## Build and test

```bash
make            # needs ~/ps5-payload-sdk and brew llvm@18
make test       # host tests in tests/, plain cc, no SDK, no console
```

Pure logic is guarded so it compiles on the host with `-DGWM_HOST_TEST`. When a bug is reproducible in pure logic, add a host test instead of spending a console run.

## Running on the console

Payload Manager's HTTP API on port 8084 is the way in. Every upload or launch needs the header `X-PLDMGR-Request: 1` and a POST; the upload takes `mode=new` on a console without the payload and `mode=replace` over an existing `gowithme.club` folder, and the launch takes the slot, not the file name:

```bash
curl -H 'X-PLDMGR-Request: 1' -H 'Content-Type: application/octet-stream' -X POST --data-binary @build/gowithme.club_v<version>.elf 'http://<ps5>:8084/manage:upload?filename=gowithme.club_v<version>.elf&mode=replace'
curl -H 'X-PLDMGR-Request: 1' -X POST 'http://<ps5>:8084/loadpayload:local/gowithme.club'
curl 'http://<ps5>:8084/processes_list'
```

A new copy replaces the running one by itself. Logs and state are in `/data/gowithme.club/`, readable over FTP.

## Invariants learned on the console

- `sceUserServiceInitialize` hung forever when the payload was run from elfldr. Every user-service call runs in a thread with a timeout (`src/user.h`); keep it that way.
- `app_info.title_id` is at offset 16. The SDK's `ps` sample reads 20 and gets the last five characters.
- Never hold a lock on a system database: SceShellCore writes them. Small ones are read as an in-memory copy; the telemetry journal `sl2_log.db` is ~100 MB, so it is opened in place without locks, and a read counts only if the file did not change meanwhile (`src/db.h`). Keep journal queries short: a full scan reads tens of megabytes.
- The console's clock can be off by tens of minutes, and the journal runs on its own clock. Everything is recorded in raw system time and corrected by the server's `Date` header at send time.
- `account` must be identical in heartbeat, sessions and trophies.
- A journal session is queued exactly once: its timestamps depend on the journal clock offset, and the server matches sessions by exact `startedAt` (`src/journal.h`).
- PS4 trophies exist only as notifications, which the system deletes within hours (`src/notif.h`). PS5 trophies come from the trophy files (`src/trophy2.h`).
- A real `TRPTITLE.DAT` carries the user id and the console's openPsid. `tests/fixtures/trptitle_ps5.dat` keeps only the block table and the trophy records; scrub any new console dump the same way before committing it.
- Never kill other processes by pid: every elfldr payload is named `payload.elf`.

## Workflow

Commits land straight on `main`. Bump `GWM_VERSION` in `src/version.h` with every change in behaviour on the console.

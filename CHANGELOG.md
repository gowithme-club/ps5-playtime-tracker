# Changelog

## 1.0.1

- The pairing notification says where to enter the code:
  "Enter code 123456 at gowithme.club/ps5".
- The payload sends an anonymous console identifier, a hash of the hardware
  id, so a console paired again after a reinstall stays one console on the
  site instead of appearing twice.

## 1.0.0

First public release. Tested on PS5 firmware 13.40 with Payload Manager.

- Play sessions from the system telemetry journal, including games played
  while the payload was not running.
- The payload's own poll as a fallback when the journal has no matching
  session.
- PS5 trophies from the console's trophy files, PS4 trophies from trophy
  notifications.
- Live "playing now" status while the payload runs.
- Pairing with an on-screen code; no config file needed.
- Offline queue, clock correction from the server's time, rest mode
  handling, one running copy per console.

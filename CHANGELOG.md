# Changelog

## 1.2.0

- The tile has its own background on the home screen: what gowithme.club is,
  with the marks of the platforms it reads and of Telegram and Discord.
- The first time the tile is opened, a welcome screen shows what
  gowithme.club does with the animated diagram from the site's front page;
  its button goes on to linking. Once pressed, or once the console is
  linked, it does not come back.
- Before the console is linked, the code sits under the QR code.
- The tile's page links the repository on GitHub.
- Once linked, the tile shows the gowithme.club profile of whoever is
  signed in on the console, with what that profile has recorded: hours,
  games and trophies. Before, it always showed the console's owner.
- A profile on the console that no one has linked yet gets a code on the
  tile, entered at gowithme.club/ps5 like the pairing code, so whoever
  plays on it gets their own games. A notification says so once per run,
  and another one when the profile is linked.
- An unlinked console no longer gets the code every 10 seconds. Two
  reminders, at the start and two minutes later, say "Link this PS5" and
  send people to the tile, which shows the code and the QR code. With the
  tile deleted the code still repeats, since nothing else shows it.
- Notifications put the news in the large line and the rest in the small
  one above it ("Tracker started" under "gowithme.club"). Before, a long
  line wrapped and the code had no small line.
- Pressing PS on a notification opens the tile's page, as with a trophy.
- On a console set to Russian the payload's notifications are in Russian;
  every other language keeps English.

## 1.1.0

- A gowithme.club tile in Media. It shows the pairing code with a QR code
  that opens gowithme.club/ps5 on a phone with the code filled in; once the
  console is linked, it shows the last report and the owner's profile as a
  link and a QR code. The page follows the console's language.
- Notifications carry the gowithme.club icon and no longer say "Debug".
- The pairing code is shown for 20 minutes. After that the payload says the
  console is not linked and stays quiet; "Show code" on the tile or loading
  the payload again brings a new one. Before, the code repeated every 10
  seconds for as long as the payload ran.
- A console removed on the site shows "console unlinked" and no new code.
- Without a connection the payload says it cannot get a code.

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

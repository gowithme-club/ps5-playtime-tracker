<p align="center">
  <a href="https://gowithme.club"><img src=".github/assets/banner.png" alt="PS5 playtime tracker: a jailbroken PS5 sends play sessions, hours and trophies to one gowithme.club profile" width="100%"></a>
</p>

<p align="center">
  <b>English</b> · <a href="README.ru.md">Русский</a>
</p>

<p align="center">
  <a href="https://github.com/gowithme-club/ps5-playtime-tracker/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/gowithme-club/ps5-playtime-tracker?style=flat-square&labelColor=131C16&color=C7F547&label=release"></a>
  <img alt="Jailbroken PS5" src="https://img.shields.io/badge/PS5-jailbroken-C7F547?style=flat-square&labelColor=131C16">
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/badge/license-MIT-C7F547?style=flat-square&labelColor=131C16"></a>
</p>

# PS5 Playtime Tracker

An open-source payload that tracks playtime, play sessions and trophies on a
jailbroken PS5, for PS5 and PS4 games, and syncs them to your
[gowithme.club](https://gowithme.club) profile. No PSN account needed.

**Requires:** a jailbroken PS5 with an ELF loader (Payload Manager or elfldr) and a free gowithme.club account.<br>
**Tested on:** PS5 firmware 13.40.

## Features

- Every play session with its start, end and length.
- Trophies for PS5 games, read from the console's trophy files.
- Trophies for PS4 games, read from the console's trophy notifications.
- Sessions played while the payload was stopped, recovered from the system journal.
- Live “playing now” status on your profile while the payload runs.
- Reports queue on the console while it is offline.
- A gowithme.club tile in Media: the pairing code with a QR code for your
  phone, and once linked, the last report and your profile.

## Install

1. Download `gowithme.club_v<version>.elf` from the
   [latest release](https://github.com/gowithme-club/ps5-playtime-tracker/releases/latest).
2. Load it with Payload Manager. The console shows a six-digit code, and
   the gowithme.club tile appears in Media.
3. Open the tile and scan its QR code with your phone, or enter the code at
   [gowithme.club/ps5](https://gowithme.club/ps5). Sign in with Google or
   Telegram; a new account is created on first sign-in.

No config file is needed.

## FAQ

**How do I track playtime on a jailbroken PS5?**
Run this payload and link it to a gowithme.club profile with the on-screen code.
Sessions, hours and trophies then show up on the profile.

**Do I need a PSN account?**
No. The payload reads activity on the console and sends it to gowithme.club directly.

**Does it jailbreak the console?**
No. It needs a console that is already jailbroken and can load ELF payloads.

**Does it have to run all the time?**
No. Sessions stay in the system journal and are picked up on the next start.
PS4 trophy notifications disappear after a while, so run the payload
regularly if you play PS4 games.

**Can I use elfldr?**
Yes, send the file to port 9021. Under elfldr the console may not give the
payload profile names, and the site then shows profiles by their id.

**Does it survive a reboot?**
No. Load it again after each jailbreak, or add it to your loader's autoload.

**Does it need internet?**
To link the console, yes: it has to reach gowithme.club over HTTP. If you
block updates through DNS, let gowithme.club through. Once linked, reports
wait in a queue while the console is offline and go out once it is back.

**What does it send?**
The game's title ID and name, session times, unlocked trophies, the console
profile name, the console's name from its settings and an anonymous console
identifier, a hash of its hardware id.

**How do I remove it?**
Stop loading the payload, delete the gowithme.club tile in Media (Options,
then Delete) and delete `/data/gowithme.club` on the console. To unlink the
console, remove it in your gowithme.club settings.

## Build from source

```bash
brew install llvm@18
curl -LO https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
unzip ps5-payload-sdk.zip -d ~
make          # dist/gowithme.club_v<version>.elf
make test     # host tests, no console needed
```

## Other consoles

| Console | Repository | Runs as |
| --- | --- | --- |
| PS5, jailbroken | **ps5-playtime-tracker** | payload |
| PS3 on HEN or CFW | [ps3-playtime-tracker](https://github.com/gowithme-club/ps3-playtime-tracker) | VSH plugin |
| Switch with Atmosphere | [switch-playtime-tracker](https://github.com/gowithme-club/switch-playtime-tracker) | sysmodule |

Stock PS4 and PS5, Steam, Xbox, Nintendo Switch and Discord connect on
[gowithme.club](https://gowithme.club) without installing anything.

## Support

[Telegram support](https://t.me/gowithme_club_support) ·
[GitHub issues](https://github.com/gowithme-club/ps5-playtime-tracker/issues) ·
pull requests are welcome.

## License

[MIT](LICENSE). SQLite is bundled under its public-domain dedication, and Project Nayuki's QR Code generator under the MIT license.

Unofficial homebrew, not affiliated with or endorsed by Sony Interactive
Entertainment. PlayStation and PS5 are trademarks of their respective owners.

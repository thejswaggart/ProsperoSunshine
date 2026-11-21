# ProsperoSunshine for PS4

A native, Sunshine-compatible game-stream host for the **PlayStation 4**. Add your console to Moonlight and stream the
PS4 screen, game audio and a controller over your LAN.

> **This is a separate PS4 port of [ProsperoSunshine](https://github.com/Darkatek7/ProsperoSunshine).** The original
> project, by [Darkatek7](https://github.com/Darkatek7), is a PS5 host; this fork adapts it to the PS4 and is maintained
> here. **Report PS4 problems on this repository's issue tracker, not upstream** — the upstream author has no PS4 and
> can't test or support the port.

> **Status: 0.1.0 — experimental, but working for everyday streaming.** Picture, game audio and one controller work end to end
> with Moonlight over the LAN. Confirmed on a **PS4 Pro (14.00)** and fat PS4s on **13.52** and **9.00**. Other firmware
> may work but is untested: the host runs on any firmware whose kernel-layout scan
> matches and warns, on the screen and in the log, when the firmware is untested — because it patches the kernel, an
> untested one may be unstable.

## Firmware compatibility

| Firmware | Support |
| --- | --- |
| **14.00** | ✅ Confirmed |
| **13.52** | ✅ Confirmed |
| **9.00** | ✅ Confirmed |
| All others | ⚠️ Experimental — the host runs if its kernel-layout scan matches, but may be unstable |

Firmware newer than 14.00 is untested — support there depends on the kernel offsets in the SDK (see
[Building from source](#building-from-source)).

## What works

- **Video:** the PS4 screen at **720p60, H.264** (no HEVC, HDR or 1080p/4K), captured through the console's own
  Remote Play encoder.
- **Audio:** game audio in **stereo** at 48 kHz, through Sunshine's Opus path.
- **Input:** **one** virtual DualShock 4 for the Moonlight gamepad.
- **Reconnect:** disconnect and reconnect without reloading the payload — the capture is held warm across streams and a
  keyframe is resent, so reconnects are quick.
- **App list:** a single **"PS4 Screen"** app with its own cover art.

## How it works

Like upstream ProsperoSunshine, this builds [Sunshine](https://github.com/LizardByte/Sunshine) (a pinned commit with
small, documented patches) for the console and replaces the parts that talk to PC hardware. The PS4 screen capture is
driven through **PBS (PlayStation Broadcast System)**: the host attaches to the console's SceRemotePlay service with a
ptrace technique and reads the H.264 frames its encoder produces. Build and architecture notes are in `CLAUDE.md` and
`docs/`.

## Requirements

- A **PS4** with a homebrew environment — an **ELF loader** (for example GoldHEN's BinLoader on TCP 9090) and, for file
  transfer, an FTP server (GoldHEN's FTP on TCP 2121).
- **Firmware:** see [Firmware compatibility](#firmware-compatibility) above — 14.00 and 13.52 are confirmed, everything
  else is experimental and warned about, not blocked.
- A wired or good wireless LAN, and a Moonlight client on the same network.

> The PS5 settings app (`PPSA47989`) and the PS5 web page are **not** used on the PS4 — install and control are by the
> ELF and the console-local pairing page below.

## Install

Send `prospero_sunshine_ps4.elf` (from a release, or `out/ps4/` after building) to your ELF loader. With GoldHEN's
BinLoader on TCP 9090:

```sh
cat prospero_sunshine_ps4.elf > /dev/tcp/<console-ip>/9090
```

The host keeps its state — identity, paired clients, `sunshine.conf` — in `/data/prosperosunshine/`. When it starts it
shows a notification on the console telling you it is up and where to pair. To replace a running host, stop it first
("Stop host" on the pairing page): a second ELF cannot bind the ports and exits, leaving the old host running.

## Pairing

By default the pairing page is reachable **only on the console itself** (bound to loopback), so no one else on the network
can pair or stop the host:

1. In Moonlight, add the console by its IP and start pairing — Moonlight shows a PIN.
2. In the **PS4's own web browser**, open `http://127.0.0.1:47990` and enter that PIN.

To pair from another device on the LAN instead — and to let `scripts/ps4_deploy.sh` stop the host remotely — create an
empty file `/data/prosperosunshine/pin_lan`. The pairing page then listens on the LAN and its address is shown in the
start-up notification. Treat this as a development / trusted-network option: anyone on the LAN can then pair or stop the
host.

## Streaming

Pick **"PS4 Screen"** in Moonlight and start. You get 720p60 H.264, stereo game audio and one controller. Disconnect and
reconnect as you like — the host holds the capture warm, so reconnecting is quick and never needs the payload reloaded.

## Known limits

- **The console's own Remote Play does not work while the host runs.** The host holds the SceRemotePlay service for
  capture; it frees it again when the host stops.
- **One device at a time.** The PS4 has a single capture channel and one virtual pad, so a second device that tries to
  join a running stream is refused.
- **720p60, H.264 only** — no HEVC, HDR, 1080p/4K or the per-request resolution modes of the PS5 host.
- **Stereo game audio only** — no surround.
- **One controller**; no keyboard/mouse, no per-device profiles, no web page and no internet/UPnP streaming. Those are
  PS5-only features of upstream and are not part of this port.
- **Untested firmware is experimental.** The host attaches on any firmware whose layout scan matches and warns when it is
  untested; because PBS patches the kernel, an untested firmware may be unstable.

## Building from source

Built with the [ps4-payload-dev SDK](https://github.com/ps4-payload-dev/sdk). It was built from source for firmware
14.00 support; newer kernel offsets land in that SDK over time, so pulling and rebuilding it may extend support past
14.00. With the SDK's toolchain environment set up:

```sh
PROSPERO_TARGET=ps4 bash scripts/build.sh   # -> out/ps4/prospero_sunshine_ps4.elf
bash scripts/test.sh                         # portable unit tests
```

`CLAUDE.md` has the full build, test and deploy notes.

### Deploying to a console

`scripts/ps4_deploy.sh` stops a running host (via the pairing page's `/api/quit`), sends the new ELF to the loader and
waits for it to come back:

```sh
PS4_HOST=<console-ip> bash scripts/ps4_deploy.sh
```

This needs the pairing page reachable on the LAN — that is, the `pin_lan` file described under Pairing.

## Credits

- A **PS4 port of [ProsperoSunshine](https://github.com/Darkatek7/ProsperoSunshine)** by
  [Darkatek7](https://github.com/Darkatek7). All the PS5 host work, and the Sunshine port it stands on, are theirs.
- Built on [Sunshine](https://github.com/LizardByte/Sunshine) and
  [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c).
- PS4 screen capture uses **PBS (PlayStation Broadcast System)** by
  [seregonwar](https://github.com/seregonwar). The injection borrows a ptrace technique from prospero-menu / libNineS
  (John Tornblom).

## License

GPL-3.0-only, like Sunshine and ProsperoSunshine, which this builds on (`LICENSE`). Third-party components and their
licences are listed in `docs/licenses.md`.

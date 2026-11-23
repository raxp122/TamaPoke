# TamaPoke

[![Flash in browser](https://img.shields.io/badge/flash-in%20browser-FF6B00?logo=googlechrome&logoColor=white)](https://raxp122.github.io/TamaPoke/)
[![Latest release](https://img.shields.io/github/v/release/raxp122/TamaPoke?label=firmware&color=4F93C4)](https://github.com/raxp122/TamaPoke/releases/latest)
[![MakerWorld](https://img.shields.io/badge/MakerWorld-3D%20case-00AE42?logo=bambulab&logoColor=white)](https://makerworld.com/es/models/2937822-tamapoke-a-pokemon-pokeball-tamagotchi)
![Board](https://img.shields.io/badge/board-ESP32--S3%20round%20AMOLED-E7352C?logo=espressif&logoColor=white)
![Pokédex](https://img.shields.io/badge/Pok%C3%A9mon-386-E8503A)
![Languages](https://img.shields.io/badge/languages-6-FFCB05)

TamaPoke is a Pokémon-inspired virtual pet for the
**Waveshare ESP32-S3-Touch-AMOLED-1.75**. This fork builds on
[ShadowEnemyx's expanded TamaPoke](https://github.com/ShadowEnemyx/TamaPoke) and adds
Wi-Fi with over-the-air updates, an internet clock and a redesigned interface
for the round screen. What changed in each version is in [CHANGELOG.md](CHANGELOG.md).

## Install

### First installation: [web installer](https://raxp122.github.io/TamaPoke/)

Use desktop **Chrome or Edge** with the board connected by USB:

1. Press **Connect board & install firmware** and pick the board's serial port.
   Leave **Erase device unchecked** if the board already has a save.
2. Optional: copy the sprites to the microSD with step 2 of the installer.
   Without it, the first update over Wi-Fi downloads them (about 105 MB).

This is needed only once. If your computer does not see the board, try another
USB cable (many only charge) or another computer: the board keeps its save.

### Updates: over Wi-Fi

Every merge into `main` publishes a release (`1.37.<build>`), and the device
installs it from **Settings › UPDATE**. See [Wi-Fi and updates](#wi-fi-and-updates).

## Controls

| Gesture | Action |
|---|---|
| Tap the Pokémon | pet it (synthetic chirp in full sound mode) |
| Swipe up | cards: profile, personality, daily, box, battle, medals, progress, expedition, steps (swipe sideways to change card) |
| Swipe down | settings |
| Swipe sideways | Pokédex |
| **Short PWR press** | **back**: closes the open screen; on the main screen it locks the display (PWR again wakes it) |
| Long PWR press | power off |
| Hold the Pokémon 3 s | release dialog |

The main screen shows the four needs as arcs along the edge (**FOOD** and
**ENE** on the left, **JOY** and **HYG** on the right, each with the icon of its
button), the four action buttons along the bottom curve and the steps, day
streak and battery at the top. While the Pokémon sleeps the screen dims only on
the main screen.

**Pokédex**: two buttons cycle the region (all, Kanto, Johto, Hoenn) and the
filter (all, raised, caught, shiny). In the detail of a Pokémon, **circle your
finger around the screen** to browse: clockwise goes forward, counter-clockwise
back, and lifting the finger opens the Pokémon in the centre.

## Settings

Swipe down for a menu of big buttons:

- **Time**: manual clock, **time zone** and **summer time** (auto, always on,
  always off). With a time zone set, the clock is synced from the internet at
  start-up and every hour.
- **Sound & screen**: sound level, power saving, language (6 languages).
- **Wi-Fi**: saved networks and automatic update check.
- **Update**: installed and available version, check and install.
- **Help** and **Info** (firmware, save, microSD, last save time, slowest loop).

## Wi-Fi and updates

- **Networks**: search, tap a network and type the password on the on-screen
  keyboard (`ABC` upper case, `#+=` symbols). Up to **10 networks** are
  remembered (home, work, hotspot…); when the list is full the one used least
  recently is replaced. Tap a saved network to change its password, `X` twice to
  forget it.
- **AUTO**: at start-up and every 12 hours the device connects to the strongest
  saved network, checks for an update and switches the radio off again. A new
  version shows a short notice and an orange **UPDATE !** button in the settings.
- **Install**: the device downloads the firmware and, only when they changed, the
  sprites for the microSD. Both are checked with SHA-256 before use. The save game
  stays where it is.

### Safety

An update must never force a reinstall over USB:

- a new firmware is confirmed only after 45 seconds of running with enough free
  memory; until then any crash or reset brings back the previous version;
- three crashes in a row also go back to the previous version, and that version
  is not offered automatically again;
- CI refuses to publish when the partition table changes, the firmware gets close
  to the 3 MB update slot or static RAM grows too much, and runs the screen
  simulator with AddressSanitizer.

## Game

- 386 Pokémon from Kanto, Johto and Hoenn, including Shiny variants
- region and starter selection on a new save
- evolution with branching, day/night, bond and stat conditions; it needs the
  species' level and at least three of the four needs above 40
- wild battles, catching, training, five minigames, daily goals
- expeditions with an item inventory, Hoenn tour sites and walking goals
- profile, personality, medals, collector ranks and cosmetic frames
- step counter with daily rewards (500 / 2,000 / 5,000 steps) and lifetime trail
  ranks; walking also raises JOY and BOND within daily caps
- RTC-based day/night and offline progression
- synthetic species chirps and sound effects; no game audio or ROM samples
- browser save backup/restore in the web installer

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-1.75 (standard or `-G`)
- round 466×466 CO5300 AMOLED and CST9217 touch
- 16 MB flash with OPI PSRAM, microSD, PCF85063 RTC, AXP2101 power management
- QMI8658 IMU and ES8311 audio codec

The `-B` model's supplied case does not fit the linked Pokéball case; the
`1.75C` is a different board.

## Development

Toolchain: ESP32 Arduino core `3.3.10`, GFX Library for Arduino `1.6.7`,
SensorLib `0.4.1`, XPowersLib `0.3.3`.

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" \
  --build-property compiler.cpp.extra_flags=-DTAMAPOKE_GEN3_RELEASE --build-path build .
```

Native tests (pet, battle, Wi-Fi/OTA logic, time zones, gestures, web serial):

```bash
cd tests && make CXX=clang++ clean test
```

**Screen simulator** (`tools/sim`): runs the real sketch on a PC with simulated
hardware, plays the scene scripts in `tools/sim/scenes/` (taps, swipes, finger
rotation, PWR) and writes screenshots plus a layout report (elements outside the
round panel, buttons smaller than 44 px) to `tools/sim/out/`:

```bash
bash tools/sim/run.sh                 # all scenes
SIM_SANITIZE=1 bash tools/sim/run.sh  # with AddressSanitizer/UBSan
```

CI (`.github/workflows/release.yml`) runs all of this on every push, attaches the
screenshots to the build and, on `main`, publishes the release and the web
installer.

Sprites come from [PMD SpriteCollab](https://github.com/PMDCollab/SpriteCollab)
and are packed with `tools/pack_pmd.py`, `tools/make_thumbs.py` and
`tools/pack_bundle.py`. The older local installer scripts (`tools/build_web*.sh`,
`web/dev.html`) are kept for reference.

## Credits and license

This is a personal, non-commercial fan project and is not affiliated with or
endorsed by Nintendo, Game Freak or The Pokémon Company.

- Original project: [socquique/TamaPoke](https://github.com/socquique/TamaPoke)
- Expanded fork: [ShadowEnemyx/TamaPoke](https://github.com/ShadowEnemyx/TamaPoke)
- This fork: Wi-Fi, OTA updates, internet clock and round-screen interface
- Animated pixel art: [PMD SpriteCollab](https://github.com/PMDCollab/SpriteCollab), CC BY-NC 4.0
- 3D Pokéball case/remix: [MakerWorld](https://makerworld.com/es/models/2937822-tamapoke-a-pokemon-pokeball-tamagotchi), CC BY-NC-SA
- Source code: [MIT](LICENSE)

See [CREDITS.md](CREDITS.md) for complete attribution.

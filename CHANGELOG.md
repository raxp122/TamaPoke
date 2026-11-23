# Changelog

Releases are published automatically on every merge into `main`
(`1.37.<build number>`) and installed on the device from **Settings › UPDATE**.
Build numbers are not consecutive: they count every CI run.

## Next

- **GPS** (`-G` board): Settings › GPS shows the status, latitude/longitude,
  altitude, satellites and UTC time of the LC76G module; for the first five
  minutes after start-up the position is also written to the serial log.
  Boards without the module show "No GPS module".

## Unreleased number (merged before GPS)

- Settings › Time: time zone and **summer time** (auto / always on / always off)
  next to the manual clock; the Wi-Fi page shows only the sync status.
- README rewritten for this fork; this changelog.

## 1.37.28 — Pokédex

- Grid of 3×4 inside the circle; region and filter as two big buttons;
  big page buttons with "page n/N".
- Detail: circle your finger to browse Pokémon in a carousel (clockwise forward,
  counter-clockwise back, a tick at every step).

## 1.37.25 — Settings pages and cards

- Settings become a menu of big buttons with separate pages (time, sound &
  screen, Wi-Fi, update, help, info); labels spelled out in full.
- Medals, expedition, steps, box and profile cards fit inside the round panel;
  buttons at least 44 px.
- No more OK/X buttons: PWR is back everywhere.

## 1.37.22 — New main screen

- Needs shown as arcs along the edge, four 68 px buttons along the bottom curve,
  steps/streak/battery pill at the top, food menu above the pet.
- Sleep dimming only on the main screen.

## 1.37.17 — Safer updates, touch, internet clock

- Update safety: new firmware confirmed only after 45 s with enough memory;
  three crashes in a row roll back; CI checks partition table, size, RAM and
  memory errors.
- Touch: gestures by dominant direction, small finger slides count as taps,
  slow swipes accepted, dimmed screen reacts to the first touch.
- Internet clock at start-up and every hour, time zone selection.

## 1.37.14 — PWR is back

- Short PWR press closes the open screen; on the main screen it locks.
- Exit swipes, "tap: back" hints and the moving @SE watermark removed.
- Screen simulator for development.

## 1.37.9 — Web installer

- Clearer first-installation button and a notice when the flashing tool cannot load.

## 1.37.5 — Wi-Fi and over-the-air updates

- Up to 10 saved Wi-Fi networks with an on-screen password keyboard.
- Updates from GitHub releases: firmware and, when changed, sprites for the microSD.
- Automatic release on every merge and web installer on GitHub Pages.

## 1.36.2 and earlier

Versions of [ShadowEnemyx/TamaPoke](https://github.com/ShadowEnemyx/TamaPoke):
Gen 3 (386 Pokémon), Hoenn expeditions, step rewards and everything before.

# Changelog

Releases are published automatically on every merge into `main`
(`1.37.<build number>`) and installed on the device from **Settings › UPDATE**.
Build numbers are not consecutive: they count every CI run.

## Next

- **Settings wheel**: the settings menu is a ring made for the round screen.
  The chosen item sits big in the centre with its icon and name, the others
  around the edge. Turn your finger around the screen (the ring follows it),
  swipe up/down or tap an item on the ring to bring it to the centre; tap the
  centre to open it. An available update shows orange on the ring.

## 1.37.55 — Tilt axes

- **Tilt**: the sensor is mounted turned by 90 degrees: tilting up, down,
  left and right now moves the ball that way (1.37.52 still mixed the axes).

## 1.37.52 — Ball, Clean, candy, frame, battery

- **Tilt**: axis signs reversed (not enough, see above); tap the screen to
  set the current position as flat (useful if the ball keeps rolling: hold
  the board level and tap).
- **Ball**: smooth movement (physics follows real time, 25 frames per
  second). The ball can be hit only below the dashed line; a tap that misses
  it, or comes too early, costs a life; a hit at the last moment (below the
  orange line) is worth 2 points. Bounces get lower and the ball falls faster
  as the score grows.
- **Clean**: keeping the finger on a stain cleans it too, so fast rubbing
  without lifting the finger works (it used to stop counting).
- **Candy**: completing all three daily goals gives a candy (up to 9). It
  appears in the food menu only when you have one: it forgives a slip-up or,
  with none, gives +10 bond. The daily card shows how many you have.
- **Favourite berry**: the Pokémon says so and gets a little extra energy.
- **Frame**: the profile shows the latest unlocked collector frame
  automatically (no more arrows); Settings › Sound & screen › Frame picks one
  by hand or goes back to auto. The TRAINER rank is now readable.
- **Battery**: a thin arc under the buttons with the percentage (and a bolt
  while charging) replaces the icon in the steps pill.
- The expedition bag no longer sits on the main screen: only an expedition in
  progress or ready to collect is shown there.
- New save keys `frauto` and `candy`; nothing else in the save changes.

## 1.37.45 — Tilt

- New minigame **TILT**: tilt the board to roll a Poké Ball, collect berries
  and avoid the holes that appear after 3, 6 and 9 points (3 lives, 30 s).
  The way you hold the board at the start counts as flat. Trains defence and
  has its own record (new key, the save is otherwise unchanged).

## 1.37.42 — Minigames

- **Ball**: a tap sends the ball to a random height inside the screen (never
  above the start), walls absorb energy instead of adding it, gravity and side
  speed grow and the tap area shrinks (50 → 34 px) with the score: games end.
- **Memo**: never the same colour twice in a row; a pause with all pads
  flashing ("Get ready...") before every sequence, a sound when a round is
  passed and when it is your turn.
- **Type**: the rival is a real Pokémon with one or two types; the right answer
  is super effective by the game's full type chart, wrong answers are resisted
  (later also neutral); less time per question as the score grows.
- **Catch**: traps appear from 4 points on: tapping one costs a life, letting it
  go is fine.
- **Clean**: stains must be rubbed away with the finger; a stain left too long
  spreads and costs a life.
- Score and record no longer touch the edge in Catch, Clean and Type.

## 1.37.39 — Diagnostics

- Info page: "LOOP MAX" no longer counts the light sleep of power saving (it
  showed up to ~750 ms that were sleep, not work) and names the slowest part of
  the loop (touch, render, save, sprite SD, wifi, gps, imu, pet).
- Main screen arcs are prepared about 15 times faster at start-up.

## 1.37.36 — GPS

- **GPS** (`-G` board): Settings › GPS shows the status, latitude/longitude,
  altitude, satellites and UTC time of the LC76G module, which runs only while
  that page is open (held in reset otherwise; the first fix can take a minute).
  The position is also written to the serial log. Boards without the module
  show "No GPS module".

## 1.37.31 — Summer time, README

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

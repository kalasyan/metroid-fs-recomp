# Native port testing

## Warp mode (`MF_WARP`)

A testing tool: it skips the title screen, starts a new game with all
equipment (energy, missiles and bombs maxed out, every ability, security level
4, every map) and goes straight into a room, or into one of the game modes
that are normally hours of play away. It works in any frontend (headless, SDL,
Android) because it is read from the environment. The code is in
`port/src/warp.c` (destination list, simulated input) and `port/game/warp.c`
(game side).

```
MF_WARP=DESTINATION[,DESTINATION...]
  DESTINATION = AREA:ROOM[@EVENT]
              | MODE:N[@EVENT]
```

- `AREA`: number (0-9) or name: `main` (Main Deck), `s1`..`s6` or `srx`,
  `tro`, `pyr`, `aqa`, `arc`, `noc`; `test1`..`test3` are the test areas left
  in the ROM.
- `ROOM`: number, or `*` for every room in the area (those with a door), one
  after another.
- `EVENT`: event counter (decimal or `0x..`). By default, the event at which
  that sector is first reached (Main Deck: 13, after the elevator with the
  SA-X).
- `MODE`:
  - `cutscene:1`..`12` or `cutscene:*`: monologues, the secret conversation,
    the lab detaching, the BSL explosion.
  - `nav:EVENT` or `nav:*`: the conversation with the computer for that
    event, starting in its navigation room.
  - `pause:0`: pause screen (map, status with R/L).
  - `gameover:0`: Samus dies, game over screen.
  - `ending:0`: full ending, credits and results.
  - `credits:0`: just the credits.
  - `sr388:0`: the ending where the ship crashes into SR388.
  - `saxcloseup:0`: SA-X close-up.
  - `save:0`: saves to slot A, resets and loads it from the menu.
  - `event:N` or `event:*`: the room where event N happens (bosses,
    abilities, SA-X encounters, escapes...) with the previous event, that is,
    just before it happens.

Modes are reached from a room, as in the game: after 40 frames of gameplay
`gPauseScreenFlag` / `gCurrentCutscene` is set and the same fade the game uses
is performed. With `MF_WARP_DIRECT=1` cutscenes start right at boot, without
going through a room (to compare against an emulator by writing those same
variables).

Other variables:

- `MF_WARP_FRAMES=N`: frames per destination when there are several or `*` is
  used (300 by default). With a single destination it stays there. When the
  list ends the program exits.
- `MF_WARP_ABILITIES=N`: number of abilities (0-18, in game order; 8 = up to
  the Varia Suit, 14 = up to the Gravity Suit). All by default.
- `MF_WARP_MOVE=0`: no simulated input. By default, in rooms Samus runs
  shooting in the direction she faces, jumps, turns back, morphs into a ball,
  lays bombs and aims up; in menus and text A is pressed; in the pause screen
  the map is moved and it switches to the status screen and back.

Examples:

```sh
# Every room in Sector 3, 300 frames each, one PPM every 60 frames
MF_WARP='s3:*' ./build/mf_headless --rom mf_eu.gba --frames 100000 \
    --dump-every 60 --out out/s3

# Box's room, just before its fight (event 40, Box defeated)
MF_WARP='event:40' ./build/mf_headless --rom mf_eu.gba --frames 2000

# Every cutscene, 2400 frames each
MF_WARP='cutscene:*' MF_WARP_FRAMES=2400 ./build/mf_headless --rom mf_eu.gba \
    --frames 30000 --dump-every 120
```

## AddressSanitizer sweep

```sh
cmake -B build-asan -G Ninja -DMF_HOST=headless -DMF_REGION=eu \
    -DCMAKE_C_COMPILER=clang \
    "-DCMAKE_C_FLAGS=-fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
ninja -C build-asan
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:suppress_equal_pcs=0
for a in main s1 s2 s3 s4 s5 s6; do
    MF_WARP="$a:*" ./build-asan/mf_headless --rom mf_eu.gba --frames 100000 \
        --dump-every 60 --out out/$a > out/$a.log 2>&1 &
done
```

With Ubuntu's Clang you need `libclang-rt-18-dev` (the ASan runtime).

## What was found this way

GBA shortcuts that crash or draw something else on a 64-bit host:

- **DMA macros**: the GBA's `DMA_SET` does `(vu32)src`, which only casts the
  first operand, so `palette_u16 + 2 * PAL_ROW_SIZE` adds *bytes*. The native
  version added elements (twice or four times as much): palettes from the
  wrong row, and message banner and credits Samus graphics read from the wrong
  place. Now `src/` follows the agbcc rule and the code written for the port
  (`port/decomp`, `port/game`) follows the C rule (`PORT_DMA_C_ARITHMETIC`).
- **Reads past the end of a table**, which on the GBA read the ROM that
  follows: tileset tilemaps (rooms that use blocks outside their tileset), the
  sound engine's noise table, the timers of blocks that regenerate. They are
  read from the ROM copy (`Port_RomMirror` / `Port_RomFind`, which now also
  finds unaligned byte tables).
- **NULL read as BIOS**: already-defeated bosses (Box, Nightmare...) that are
  deleted on init but keep animating their multi-sprite data; the final
  monologue cursor before it has any text.
- **Larger structs natively**: `SamusInit` copied a 40-byte block of zeros
  into structs containing pointers.
- **"End of previous array" pointer tables** (`sUnk_79bea8`, status screen):
  on the GBA the arrays are contiguous, natively they are not.
- **Code copied to RAM**: Samus's HBlank in the credits is called directly.

## Rumble and second screen

`port/game/status.c` reads, once per frame (from `Port_RunFrame`, before
presenting), the game's global variables: Samus's energy, ammo, equipment,
pose and position, bombs and power bombs, screen shake and the minimap data
the game keeps for the pause map. It only reads; the game code does not
change. By comparing with the previous frame it detects events: damage
(energy lost plus invincibility frames), continuous drain (lava, acid, cold),
a bomb or power bomb exploding, a fall of more than 7 blocks, a shinespark
crash, Samus's death and an item or ability pickup. Screen shake (bosses,
explosions) rumbles while it lasts. The copy of the previous frame is
forgotten when a save state is loaded, so no false events are seen.

- **Rumble** (`port/src/extras.c`): each event triggers a short effect; the
  strongest one drives both gamepad motors (`SDL_RumbleGamepad`). The device
  vibrator (on Android, SDL's `VIBRATOR_SERVICE` haptic device) only supports
  pulses, so it gets one each time the rumble starts or gets stronger.
  `MF_RUMBLE_LOG=1` writes each event to the log (also with `mf_headless`, to
  test it with `MF_WARP` and `--press`).
- **Second screen** (`port/src/second_screen.c`): drawn in software on a
  canvas 256 pixels wide. The map uses the pause map's tiles and palette with
  the explored and downloaded rooms (`gMinimapData_Visited`), framed on the
  sector's rooms, and Samus's tile blinks; the status page is text in an 8x8
  font (energy, missiles, power bombs and equipment). On PC it is a second SDL
  window (on another monitor if there is one; closing it turns it off). On
  Android SDL only has one window, so `MfActivity` looks for a secondary
  display (`DisplayManager`, *presentation* category first), shows a
  `Presentation` there that never takes focus, and fetches the image through
  JNI (`host_sdl.c`) on every refresh. Without a second display it does
  nothing. `mf_headless --screen2 map|status` also dumps that image with each
  capture.

Options in `settings.ini` (`[rumble] gamepad, device, strength` and
`[second_screen] mode = off | map | status`) and in the menu. Rumble is off by
default; the second screen shows the map on Android (only when there is a
second display) and is off on PC.

## Smoothness: 60/120 Hz and frame blending

The GBA draws 59.7275 frames per second and almost all displays run at 60 Hz
(or 90, 120, 144...). By default the game follows the GBA clock, so on a 60 Hz
display with VSync one frame is shown twice every ~3.7 s, and at 144 Hz frames
last 2 or 3 refreshes irregularly. Two options in `settings.ini` (`[video]`)
and in the menu (Video, Smoothness), both off by default and only in the SDL
frontend (`port/src/host_sdl.c` and `port/src/pacing.c`):

- `sync_to_display = true`: if the display refresh rate is a multiple of
  ~59.73 Hz within 1% (59.94, 60, 120, 240 Hz...), the game runs one frame
  every N refreshes, paced by VSync: at 60 Hz it runs 0.46% faster and every
  frame lasts exactly one refresh. Sound plays that same 0.46% faster with a
  small dynamic correction that keeps the audio queue at a fixed level, so it
  neither drains nor overflows. On other displays the GBA clock is followed.
- `frame_blend = true`: on displays that can't be synced (90, 144 Hz...), each
  refresh blends the last two frames according to the moment it is shown. It
  spreads motion evenly at the cost of a slight trail and half a frame of
  latency.

Both turn VSync on, and neither touches the game logic: only when frames are
presented and the speed sound plays at change. `mf_pacing_test` (built with
the headless host and run in CI) simulates 10 minutes of sound at several
refresh rates and sound card clock errors. `MF_SYNC_LOG=1` prints the game
fps, audio queue level and speed ratio every 5 s, and `MF_REFRESH_HZ=N` fakes
another display refresh rate.

## Widescreen

The `widescreen` option (`off | auto | 16:10 | 16:9 | 2:1`, Video menu;
`--width N` in headless) makes the picture 256, 284 or 320 px wide instead of
240, with the same height. **The game does not change**: its camera, its
240 px screen, the sprites' on-screen state and the logic are the GBA's. It
just draws more:

- **PPU** (`port/ppu`, `ws_origin` / `ws_obj`): the GBA screen is drawn at
  column `origin` of the wide frame and the backgrounds continue on both
  sides. Each OAM entry gets a placement: room object, HUD anchored left or
  right, centered label, or "as on the GBA" (clipped to its 240 px).
- **Tilemaps** (`port/game/widescreen.c`): every frame the tilemap columns
  missing for the chosen width are rewritten from the room data, including
  the blocks that come from the common tileset (`sCommonTilemap`).
- **Sprites** (`src/sprite.c`): the ones in the extra columns are drawn too,
  without touching their state, leaving 32 OAM entries free for Samus and her
  shots.
- **Framing** (`port/src/widescreen.c`): the GBA screen stays centered; near
  the edge of what the camera may show the picture stops there and the GBA
  screen slides off-center, and where the area is narrower than the picture
  it is centered with black on the sides. Nothing the GBA camera could not
  show is ever shown. The HUD sticks to the edges of what is shown.
- Everything outside the in-game loop (title, menus, map, cutscenes) is drawn
  at 240 px in the center. Save states don't change.

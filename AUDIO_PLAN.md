# NeoTS2 audio plan

**Status:** untracked planning document. Not a commitment, not a design that has been through review.

**Date:** 2026-11-12
**NeoTS2 studied:** `main` @ `c6a6364`
**Reference:** opensims2 `docs/audio.md` (facts read from the shipped Mac binary and the game's data), `docs/game-flow-mac.md` §4, `docs/cinematics-mac.md` §1, and opensims2 `src/audio/*` as one working arrangement — not as code to copy.

**Confidence:** high for the formats and the naming rules, medium for mix behaviour (voice counts, pausing, ducking) and medium for cost on the SDL 1.2 target.

---

## TL;DR

NeoTS2 already emits sound events and nothing listens. `World::setSoundEvents` is called by the VM with the raw sound name and the source object (`src/sim/vm_objects.cpp:163-193`); the callback is never set. Animation sound events (type 1014) are parsed but not dispatched. There is no audio module, no decoder and no output.

The plan is to build a small `src/audio` module — bank, resolver, decoders, mixer — and wire it where the effects system is already wired (`app/game_effects.cpp:139` is the pattern to copy). The VM side needs almost no work: the hard part is the bank and the decoders.

**What you can hear after each milestone:**

| Milestone | You hear |
|---|---|
| AM0 | A test tone through a real device, and a WAV from the headless mix |
| AM1 | Object sounds started by scripts: doorbell, chair, fridge, sink |
| AM2 | Sim voices, footsteps with the right surface and shoe suffix |
| AM3 | Lot ambience that follows time and outside-ness, plus menu/hood/lot music |
| AM4 | TV audio, component loops (running water, fire), movies once video exists |
| AM5 | Positional sound and animation-driven sounds |

**Suggested first branch:** `audio` cut from `main`. Before any code, open an issue (CONTRIBUTING: "open an issue before starting anything big") with this document's scope and the AM1 slice, because this is a multi-week feature and the maintainer may want a narrower first PR.

---

## 1. What NeoTS2 has today

These are the anchors, checked in `c6a6364`:

- **The VM already names sounds.** Play Sound Event reads string table `0x4132` ("Sound ID Table") from the object's own, global or semi-global group, handles the negative-index variable form, picks the source object from byte 4 (`0x02` = stack object) and stops with bit `0x40`. It mutes hidden objects and Sims off the lot. Then it calls `world().soundEvents()`. See `src/sim/vm_objects.cpp:158-193` and `kSoundNameTable` in `src/sim/vm_execution.hpp:236`.
- **Nothing subscribes.** `setSoundEvents` has no caller in `app/`; a search for `setSoundEvents` finds only the definition and the VM call site.
- **Stop All Sounds is a no-op.** Opcode `0x0030` returns true with the comment "Nothing plays sounds yet" (`src/sim/vm.cpp:141-143`). It needs to reach the audio layer like Play Sound does.
- **Effects show the wiring pattern.** `Game::doEffectCommand` is registered with `world().setEffectCommands(...)` in `app/game_effects.cpp:139`, owned per lot and fed from `Game::updateLot`. Audio should attach the same way, not invent a second lifecycle.
- **Animation events are parsed.** `formats::anim::Event` has `type` and `text`; type 1014 is a sound, and the comment in `src/formats/anim.hpp:77` says so. Nothing dispatches it; `sim` only consumes 1005 and 8.
- **Resource access is ready.** `content::ResourceIndex` can find and read any resource by key, with the install's packages already indexed. The audio bank should read through it rather than open packages itself.
- **No platform audio.** `src/platform/platform.hpp` has windows, input, GL and display modes, no device. SDL3 is the main backend; SDL 1.2 exists for Mac OS X 10.4/10.5 on PowerPC and has an OpenGL-only renderer.
- **Existing tests** are one executable (`vm_tests`, `tests/test_main.cpp` + `CHECK`), with `fuzz/` targets per parser. A new `tests/audio_tests.cpp` and `fuzz/audio_decode_fuzz.cpp` fit that structure and need registering in `CMakeLists.txt`.

Two existing behaviours worth preserving:

- The VM treats a missing name as silence and returns true. The audio layer must not turn that into an error or a stall.
- `SoundEvent` currently carries only `name`, `source` and `stop`. That is enough for Play/Stop; anything else (volume, priority) is a later, additive change.

---

## 2. What the game does (facts, not code)

Everything here is from opensims2's `docs/audio.md` and `docs/cinematics-mac.md`, which are written against the shipped Mac binary and the game data.

### 2.1 Resources

| Type | Name | What it holds |
|---|---|---|
| `0x2026960B` | AUDIO | Samples (XA, SPX, MP3), plus text INIs, FSC and compiled HIT |
| `0x0B9EB87E` | track settings | A property set per track (sample rate, instance, instance high) |
| `0x7B1ACFCD` | hit list | Version 56, count, then pairs of (instance, instance high) |
| `0x9D796DB4` | track logic | Binary, not decoded; only 6 in `Misc.package` |
| `0xEBFEE33F` | property set | Ambience layers, music categories, `aliases`, `shoetypes`, `events` |

A *track* is a track-settings resource and a hit list with the same group and instance, in a `*/Tracks` group; its hit list names samples in the matching `*/Samples` group. The base game has 15,935 hit lists and 50,068 entries; 49,998 resolve, and the 70 that do not are voice tracks that do not exist in the base data.

Groups come from `[DBSegments]` in `TSAudio` (`Misc.package`, group `0xADD550A7`): the folder a file came from becomes the group (`0xEB8AB35D` = `UI/Tracks`, `0x6B8AB3D7` = `UI/Samples`, and so on).

Name keys are the scenegraph rule NeoTS2 already implements: instance = CRC24 of the lowercased name with `0xFF000000`, instance high = CRC32 of the name (MSB, no final xor). `hash::scenegraphKey` is the same computation; verify it against the audio cases before adding a second implementation.

### 2.2 Samples

- **XA** — 7,398 files in the base. A 24-byte header (`XA`, two variant bytes, decoded size, WAVEFORMATEX), then 15-byte blocks per channel: one header byte (predictor 0-3, shift -8) and 14 bytes of 4-bit residuals, high nibble first, 28 samples per block. Predictor pairs `(0,0)`, `(240,0)`, `(460,-208)`, `(392,-220)`; the formula is `s = ((r << 28) >> shift + c1 + c2 + 0x80) >> 8`. Stereo alternates bytes after both headers. Some files end with a half block; decode with zeros and cut by the header size.
- **SPX** — 42,962 files. `SPX1`, channels, decoded size, Speex mode (2 = ultra-wideband, 32 kHz), 640 samples per frame, then length-prefixed Speex packets. opensims2 uses libspeex 1.2.1 with perceptual enhancement on; that is a safe default until the game's setting can be checked.
- **MP3** — 143 files, music and a few stings, 32/44.1 kHz stereo with 7 mono, no ID3.
- WAV and UTK are accepted by the engine's INIs but absent from the base game. Low priority.

### 2.3 Naming: modifiers, aliases, voices

An event name is composed, not looked up raw:

1. **Modifiers** from the `events` property set (211 events): `destination`/`source` footstep surfaces, shoe (`bare`, `leath`, `heel`, `rub`, `sand`, `slip`), object quality (`exp`, `chp`, `wdglass`, `gmetalexp`), skill suffix (`ls`/`ms`/`hs`, `a`-`j`). Example: `foot_step` + grass + leather = `foot_step_grass_leath`.
2. **Vox** is appended last for `vox*` events: age letter from person data 58 (`b`, `p`, `c`, `t`, `a`, `e`), gender from 65 (`m`/`f`), voice from 88 modulo the age's voice count (only adults have two), ghost from 68 bit 0 (`g`). Example: `vox_exmach_pec_stressA_hs_ama`.
3. **Aliases** (121 entries) redirect the composed name; example `foot_run_cpet_leath` → `foot_run_cpet_rub`.
4. **Fallback**: if no track matches, drop suffixes from the end, one at a time, until one does. Some objects ask for tracks the base game does not have (`fridge_idle_run_lp_exp`); that is normal and must be silent, not an error.

`STR# 0x94` ("soundinfo") supplies four items per object: the surface it is or offers, the surface of what it holds, its quality, and its ambience partition. The resolver needs it, so A-11 is not only the property sets.

### 2.4 Ambience, music and object effects

- **Ambience**: 17 layers with `EnabledPrimitiveIds`, `AND`/`NOT` partitions (`isflora`, `iswater`, `isinside`, `istombstone`, `islot`, `isneighborhood`), a loop name or an FSC sequence (`version 7` text: volume, BPM, 20-column notes with pan, duration, probability, sample), and volume curves over `outsideness` and `zoom`. The lot's outside fraction comes from rooms; the neighbourhood has no clock and uses midday.
- **Music**: categories `Build`, `Buy`, `CAS`, `Nhood`, `hiphop`, `metal`, `pop`, `rnb`, `salsa`, `techno`; shuffled order, volume 0.7, 3-second fade out; scene states (menu splash, hood, CAS, buy, build, lot) as `audio.md` ("Música"). Stereos play `stereo_<station>` as a category, not as a track.
- **Component effects** (`cTSSoundEffect`, component type 7): version, flags, `audioId` (the track instance), position interval, duration, volume, with Start / ApplyEffect / Stop. This is what makes running water and fire loop.

TV and movies are MP3-free: `.movie` files are Madcow video with `adpcm_ea_r1` audio in an EA chunk stream (`cinematics-mac.md` §1), so TV audio depends on the media workstream, not on this one.

---

## 3. Design

### 3.1 Where it lives

```
src/audio/
  device.hpp            // thin interface: open, write, pause, volume, close
  system.hpp/.cpp       // owns the device, the bank, the mixer and the scene mix state
  sound_bank.hpp/.cpp   // group table, name keys, track + hit list lookup, package reads
  resolve.hpp/.cpp      // modifiers, aliases, soundinfo, vox, suffix fallback
  track.hpp/.cpp        // track settings + hit list, sample selection
  decode.hpp/.cpp       // format dispatch, PCM + rate
  xa.cpp  spx.cpp  mp3.cpp
  mixer.hpp/.cpp        // voices, buses, resampling, mixing
  voice.hpp             // one playing sound
  music.hpp/.cpp        // category player and scene states
  ambience.hpp/.cpp     // layers, FSC player, curves
src/platform/audio.hpp                 // platform-neutral device factory
src/platform/sdl3/audio_sdl3.cpp       // SDL_OpenAudioDeviceStream
src/platform/sdl12/audio_sdl12.cpp     // decision in A-28; may be a no-op build
app/game_audio.cpp                     // wiring: scene states, VM hook, per-frame update
tests/audio_tests.cpp
fuzz/audio_decode_fuzz.cpp
```

`Game` owns one `audio::System` for the app's lifetime, as it owns the renderer. Scenes push mix state; the lot registers a listener position and voice sources. This is deliberately not lot-owned: the menu, hood and CAS need music too, and `leaveLot` should not have to tear down a device.

### 3.2 Signal path

```
VM Play Sound Event ──> world.soundEvents(name, source, stop)
                            │
animation 1014 ─────────────┤
component effect ───────────┤
                            ▼
                    resolve(name, source)
                  modifiers, vox, aliases, fallback
                            ▼
                  sound_bank.findTrack(name)
                            ▼
                  hit list -> pick one sample (RNG)
                            ▼
                  decode (cached) -> PCM at the device rate
                            ▼
                  mixer voice { source, gain, pan, loop }
                            ▼
                  device
```

- **Voice key** is `(source object, raw event name)`, the name the script gave, not the resolved one — resolution depends on state (floor, shoe, mood) that may change before the stop arrives. A `stop` stops every voice with that key. One-shots cannot be stopped and simply end.
- **Sample choice is random per play** in the game. Use a mixer RNG, never the VM's, so a save does not change the sound a reload produces and tests stay deterministic when seeded.
- **Decode once**, cache by resource key + target rate, with a byte budget (A-25). Decoding never happens on the device callback.
- **Failures are silent**: missing name, missing track, missing sample, unknown format and zero-length hit list all log once at debug level (`--sound-log`), then do nothing. This matches the VM's current behaviour.

### 3.3 Threads and real-time rules

- The device callback (or the SDL stream's) mixes only; it allocates nothing and locks only with `try_lock`.
- Decoding runs on a worker (one is enough at first), as the game's own tracks are short; music can decode ahead.
- The game thread only enqueues commands (start/stop/set listener) into a small queue consumed at the next mix block.

### 3.4 Output

Use SDL3's audio stream on the SDL3 targets: it handles device rate, format conversion and channel layout. The SDL 1.2 target is a separate decision (A-28); it may get the same interface over SDL 1.2 audio, or a clean no-audio build. The device is opened lazily on the first sound, so `--screenshot`, headless CI and the fuzzers never open one.

Master and per-category gains: `Master`, `Sfx`, `Vox`, `Music`, `Ambience`, `UI`. Values live with the other game options and persist where the Graphics/Game Options panels already persist theirs (A-29).

### 3.5 Pausing and speed

The game has three clocks that matter: the simulator (pause, speed 1/2/3), the animation clock and real time. The plan:

- SFX, vox and ambience follow the simulator's pause, except for objects whose scripts run while paused (`SimObject::runsWhilePaused`).
- Music does not pause with the simulator; it changes with the scene state.
- TV continues while the game is paused (it is a real-time medium), pending a behaviour check.
- Speed does not change pitch. All clips are mixed at their authored rate.

These are the least certain rules in the document; see §7.

---

## 4. Tasks

IDs continue the audio workstream in `FEATURE_PORT_PLAN.md` (§7), so the two documents do not drift. Sizes: XS = under half a day, S = 0.5-1 day, M = 2-4 days, L = 1-2 weeks.

### AM0 — Make the path exist

| ID | Task | Notes / acceptance | Size |
|---|---|---|---|
| A-01 | Audio resource types and constants | `AUDIO`, track settings, hit list, track logic, property set types; one header, no magic numbers elsewhere | XS |
| A-10 | Platform audio device | `platform::AudioDevice` over SDL3; open lazily, close on exit; a null device when there is none. Builds and runs on Windows and macOS | M |
| A-23 | Device selection and recovery | Enumerate outputs, remember the chosen one, reopen the default when a device disappears. Gameplay never blocks on a missing device | S |
| A-09 | Mixer skeleton | N voices, per-voice gain/pan, 16-bit stereo output; silence when no voices. Unit-tested math | M |
| A-24 | Headless mixdown `--sound-mix out.wav` | Runs the same mixer without a device; a test tone or a decoded sample lands in a WAV. This is the CI-visible proof of AM0 and the basis of later tests | S |
| A-28 | SDL 1.2 decision | **Decided:** SDL 3 only. The SDL 1.2 build gets a silent `system_none.cpp` that logs once; SDL 1.2 does have an audio API, but its PowerPC targets are untested and the interface leaves room to add it later | S |
| A-30 | Diagnostics: `--sound-log`, missing-name counters | Every resolve failure counted by name; a summary at exit. No game data in logs beyond names | XS |

**Exit:** a test tone is audible through a real device on Windows and macOS, and `--sound-mix` writes a WAV with the same samples.

### AM1 — First real sounds

| ID | Task | Notes / acceptance | Size |
|---|---|---|---|
| A-02 | Index the sound packages | Walk `TSData/Res/Sound/*.package` and the locale overlays through `ResourceIndex`, build the group table from `[DBSegments]`; no whole-package loads | M |
| A-03 | Name-key verification | Prove `hash::scenegraphKey` matches the audio keys for a sample of names; reuse it, do not add a second hash | XS |
| A-08 | Track and hit-list selection | Track settings + hit list by group/instance; random sample per play; unresolved entries skipped | M |
| A-04 | XA decoder | Mono and stereo, all four predictors, half-block tail; duration and peak checks | M |
| A-13 | Wire `World::setSoundEvents` | Register from `Game` where effects register; map `stop` correctly; hidden/off-lot objects already filtered by the VM | S |
| A-14 | Stop All Sounds | Opcode `0x0030` exists and currently returns true with the comment "Nothing plays sounds yet" (`src/sim/vm.cpp:141-143`). It needs its own small hook (`setSoundStopAll` or an extra field on the event), because `SoundEvent` cannot express "stop everything this object plays" | S |
| A-25 | Decoded-sample cache | Byte budget, LRU, thread-safe; no unbounded growth across lots | M |
| A-31 | UI sounds | Button clicks and panel opens play their named tracks (`ui_button_play` and siblings) through the same one-shot path; no VM involvement | S |

**Exit:** doorbell, chair, fridge, sink and TV switch make their sounds in a lot, from scripts, and stop when the script stops them.

### AM2 — Voices and correct names

| ID | Task | Notes / acceptance | Size |
|---|---|---|---|
| A-05 | SPX decoder | Vendor libspeex (BSD) with a notice; mode 2, 640-sample frames; a decode test and a fuzz target | M |
| A-11 | Event modifiers and aliases | `events` and `aliases` property sets, ordered modifiers, suffix-dropping fallback; table-driven tests | L |
| A-27 | `soundinfo` (STR# 0x94) | Surface, held surface, quality, ambience partition; consumed by A-11 and A-18 | S |
| A-12 | Vox suffix | Age/gender/voice/ghost rules from person data 58, 65, 88 and 68; test the documented examples | M |
| A-21 | Audio tests (unit) | Key, resolver, selection, mixer, WAV writer; no game files | M |

**Exit:** Sims speak with the right voice, and footsteps change with floor and shoe. A missing suffix chain falls back instead of going silent.

### AM3 — The bed

| ID | Task | Notes / acceptance | Size |
|---|---|---|---|
| A-06 | MP3 decoder | Vendor dr_mp3 (public domain) behind the decoder interface; do not depend on SDL3_mixer. Verify duration and rate on the install's music | M |
| A-18 | Ambience | Layers, partitions, outside fraction, zoom curve, FSC sequences; midday for the hood | L |
| A-19 | Music | Categories, shuffle, 3 s fade, scene states (menu/hood/CAS/buy/build/lot) | M |
| A-17 | Volume and mute UI | Master + categories in the existing options panels, persisted with the other settings | M |
| A-29 | Settings persistence | Follow `saveLauncherSettings` for the file, not a new format | S |

**Exit:** the menu, hood and lot each have music; a lot has ambience that changes with time and whether the camera is inside; volume sliders work and survive a restart.

### AM4 — Objects and screens

| ID | Task | Notes / acceptance | Size |
|---|---|---|---|
| A-22 | Component sound effects | `cTSSoundEffect` Start / ApplyEffect / Stop, looping by duration | M |
| A-20 | TV and movie audio | EA ADPCM in `.movie`; decoding is independent, but sync needs the video clock from M-01, so schedule with it | M |

**Exit:** running water and fire loop and stop; a TV plays sound in sync once video exists.

### AM5 — Polish

| ID | Task | Notes / acceptance | Size |
|---|---|---|---|
| A-15 | Animation sound events (1014) | Dispatch from the animation event path, deduped by (object, event time, name); muted for off-lot Sims like the VM rule | M |
| A-16 | Positional sound | Distance attenuation and pan from the listener; no HRTF, no reverb | M |
| A-26 | Decoder fuzz and conformance | Random input must not crash or hang; XA vectors hand-computed; SPX vectors from Speex's own test data (BSD) | M |
| A-07 | WAV and UTK fallback (optional) | Only if mods ship them; the base game does not. Behind the same decoder interface | S |

**Total:** roughly 10-14 weeks for one developer, with A-04, A-11 and A-18 the risky ones. The port plan's M1 range (3-6 weeks for A-01-A-16) did not include device lifecycle, decoder verification, the cache or the music/ambience player; this document is the more honest range.

---

## 5. Test plan

CONTRIBUTING forbids game files and extracted assets in the repository. Everything below either uses synthetic data, our own generated fixtures, or a local install that is never committed.

1. **Pure functions** (in `tests/audio_tests.cpp`, registered in `CMakeLists.txt`): name keys, modifier composition, alias and suffix fallback, vox rules, hit-list choice with a seeded RNG, gain/pan math, resampling, WAV writing. Table-driven where the docs give examples.
2. **XA**: encode a handful of blocks by hand (nibbles, predictors, shifts) and compare against the computed output; include the half-block tail and both channel orders.
3. **SPX**: decode a synthetic silent frame to prove framing and sizes; decode Speex's own test vectors for sample accuracy. Keep the vectors under `tests/data` with their BSD notice.
4. **MP3**: one short generated sine (our own work, committed with a note) proves decode length and rate; no game music.
5. **Mixdown**: `--sound-mix` on a scripted lot run produces a WAV; assert non-silence, event counts from `--sound-log`, and that a stop actually truncates a loop.
6. **Fuzz**: `fuzz/audio_decode_fuzz.cpp` over XA/SPX/MP3 headers and bodies; the decoder must reject bad sizes without allocating wildly.
7. **Manual, with a real install**: a new `--play-sound <name>` debug flag, and a lot run with `--sound-log`, compared by ear and by the missing-name report. Say in the pull request which of these were done, as CONTRIBUTING asks.
8. **Performance**: decoding 1,000 short SFX under a target on the oldest supported machine; no allocation in the mix callback.

---

## 6. Licensing and provenance

- **No game code and no game data** in the tree. Formats and naming rules are facts from the docs; the code is NeoTS2's.
- **libspeex** is BSD-3-Clause: vendor under `third_party/speex` with its licence, add it to `third_party/licenses` and the README credits.
- **dr_mp3** is public domain (or CC0); same treatment.
- **SDL3** is zlib; already vendored and credited.
- Do **not** copy opensims2's `src/audio/*` or its decoders: it is MPL-2.0 and this project is MIT + Commons Clause. Reading it as one arrangement of the same documented facts is fine; copying files is not.
- Do not ship ffmpeg. It is useful locally to cross-check a decoded WAV, and that is all.
- Test fixtures must be ours or BSD-licensed; no audio extracted from the game.

---

## 7. Open questions

1. **Pausing.** Which buses pause with the simulator? Music presumably not; ambience and SFX presumably yes; TV presumably not. Needs a behaviour check against the game.
2. **Voice count and stealing.** The game has a limit per object and per lot; the number is not in the docs. Start with a generous fixed cap and evict the oldest one-shot.
3. **Sample choice.** Random, but seeded how? Using our own RNG keeps saves deterministic, at the cost of not matching the game exactly.
4. **XA variants `0x12` and `J`.** Possibly loop flags; the decoders are the same. Matters for looping SFX.
5. **SPX perceptual enhancement.** opensims2 enables Speex's default; the game may not. Compare by ear on a voice line.
6. **SDL 1.2.** Whether PowerPC Macs can decode XA/SPX in real time, or whether that target gets a no-audio build.
7. **Sound events while paused and for off-screen Sims.** The VM mutes hidden and off-lot objects for Play Sound; animation events may need the same rule or a different one.
8. **Ducking.** Does music duck under dialog or cinematics? Not documented; leave out until observed.

---

## 8. Definition of done for the first pull request (AM0 + AM1)

A first pull request that does AM0 and AM1 is the recommended step, because it is the smallest slice that is both audible and testable:

- `A-01`, `A-02`, `A-03`, `A-04`, `A-08`, `A-09`, `A-10`, `A-13`, `A-24`, `A-25`, `A-30`, `A-31` done, and the A-28 decision written down.
- A lot plays object sounds started by scripts, and stops them, on Windows and macOS.
- `--sound-mix` produces a WAV in CI.
- `simrun` with `--sound-log` reports no crash and a missing-name list that is only the known-missing tracks.
- No game files committed; new third-party code has a licence and a notice.
- The pull request says how it was checked, as CONTRIBUTING requires, and notes that it will be rebased onto the next snapshot.

Everything else in this document can follow in later PRs; the numbering is stable so the plan does not have to be rewritten between them.

---

## 9. Out of scope

- **TV and movie video.** M-01 owns the Madcow decoder; this document covers only the audio beside it.
- **Reverb, EAX, HRTF, surround.** The game's own reverb preset is not documented; stereo with distance and pan is the target.
- **New music formats or mod sound banks** beyond what DBPF packages provide.
- **Voice recording or synthesis.** Only the shipped samples.
- **A mixer UI.** Voices are managed by priority and age; no player-facing mix panel beyond volume sliders.
- **Streaming music from disk.** Tracks are decoded into memory; the cache budget is the guard.
- **UI layout changes.** Volume and mute use the existing options panels.

---

## 10. Implementation status (2026-11-12)

The first slice is in and builds: the `audio` branch at `c6a6364` plus the changes listed below.

**Done**

- `src/audio/bank.*`: finds every product's `TSData/Res/Sound` and locale sound packages, indexes track settings (`0x0B9EB87E`), hit lists (`0x7B1ACFCD`), samples (`0x2026960B`) and property sets (`0xEBFEE33F`) through one `ResourceIndex`; looks names up with `hash::scenegraphKey`, falling back to the instance alone for packages whose index has no instance high.
- `src/audio/xa.*`: the XA decoder, with the 24-byte header, all four predictors, half-block tails and a size cap; tested against hand-built blocks.
- `src/audio/system.*`: SDL 3 output stream, a 32-voice mixer with resampling, a decoded-sample cache, random sample choice per play, looping tracks (`0xff595ffd`), and track resolution with quality/surface/shoe suffixes, alias redirection and the vox suffix for person data 58/65/88/68.
- `app/game_audio.cpp`: the VM hook (`World::setSoundEvents`), soundinfo (STR# 0x94), Stop All Sounds clearing, and per-lot teardown.
- Animation sound marks (type 1014, `sound_name=...`) are told through the same mixer, merged with the SimAntics marks in time order (`World::setAnimationSound`, `sim/overlay_events.cpp`). This is where most object sounds come from — the toilet flush, chair scrapes and similar — rather than from a BHAV.
- Sounds are placed in the lot: the listener is the camera target, gain is full within 5 tiles then `(5/d)^1.2`, and pan follows the side of the screen (opensims2's model; OpenTS2 is 2D).
- Looping tracks play: a track loops when its settings carry `0xff595ffd` or its name ends in `_lp`; one loop starts per object and event, a stop removes it, Stop All Sounds clears the object's voices, and a removed object's voices are pruned. The loop seam is interpolated, as opensims2 does.
- UI clicks play the button's own `btnclicksnd` (matched by CRC24 against the game's `ui_*` names, as opensims2 does), or `ui_primary_button` when the script names none.
- `--play-sound <name>` plays one track by name at startup, for checking a decoder or a name.
- SDL 1.2 builds get `src/audio/system_none.cpp`, which logs once and stays silent (A-28).
- `tests/audio_tests.cpp`: name hashing, XA nibbles, size trimming, rejection.

**Not yet**

- SPX voices (`A-05`) and MP3 music (`A-06`), so Sims are silent and there is no music.
- Ambience (`A-18`), music states (`A-19`), and component effects (`A-22`).
- The full `events` modifier set: the resolver tries the common quality/surface/shoe combinations rather than reading the event's own modifier list. Misses are logged by name.

**Checking it**

```
build\neots2.exe "<install>" --menu --play-sound door_open
build\neots2.exe "<install>" --menu --play-sound toilet_flush
```

The log says which track was resolved, its duration and its rate. Indexing the sound packages takes several seconds and runs beside the menu; a sound asked for before it finishes is dropped, while `--play-sound` waits.

While testing, add `--no-crash-dialog`: a run killed with Task Manager leaves an "unexpected exit" report that would otherwise show its dialog at the next start and look like a hang.

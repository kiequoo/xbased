# xbased changelog

## 1.0.36 (2026-11)

### Fixes
- Grand Theft Auto: San Andreas (European Classics release) crashed while
  loading with its 60 FPS patch on. The shipped patches (60 FPS,
  widescreen, 480p) were made for the USA releases and write bytes at fixed
  places of the game's executable; given to other releases of the same game
  they landed on other code (in San Andreas, half of a call). They now go
  only to the USA release each one was made for: 45 copies on other
  releases are gone, and an update deletes them from the mods folder at
  start (only while they're unchanged, never a patch of your own).

## 1.0.35 (2026-11)

### Sound
The voice processor now follows raddad772's measurements of the audio chip
on a retail console (xemu pull request #3105):

- 3D sound (HRTF): the filter taps are read as the chip's small floating
  point numbers, so 3D voices no longer come out several times too loud and
  lopsided between ears; the left and right ears go to their own routes, a
  3D voice plays mono, and new HRTF settings blend in across one frame.
- Voices are resampled with the chip's 8-point interpolation instead of a
  linear one: cleaner highs, no aliasing.
- Volume and pitch changes ramp across each frame instead of stepping (no
  clicks or buzz on envelopes), with the chip's volume curve and envelope
  timing.
- The ParaEQ and I3DL2 filters and the voice LFOs (vibrato, tremolo) work;
  the DLS2 filter has no extra input cut or distortion.
- DirectSound submixes (multipass voices) are no longer up to 42 dB too
  quiet, and a voice sharing a page number with an output buffer no longer
  goes silent.
- Savestates from earlier versions load; their voices fade back in over
  one frame.

## 1.0.34 (2026-11)

### Fixes
- Spikeout: Battle Street said its program was damaged and didn't start.
  Its executable has 398 sections and the loader took no more than 256; it
  now takes up to 512.
- That error message said hakuX where it should say xbased.

## 1.0.33 (2026-11)

### Fixes
- Rainbow Six 3 crashed back to the dashboard while loading the first
  campaign mission. At boot it rewrites the C runtime's malloc, calloc and
  free to use its own allocator, and malloc had already been translated, so
  the old version kept handing out the system heap's memory while free sent
  it to the game's allocator. Changing a range's protection (as a game does
  before and after patching its code) now drops the code translated from
  it.

## 1.0.32 (2026-11)

### Fixes
- SNK vs. Capcom: SVC Chaos went back to the dashboard after the Neo Geo
  screen. It makes thousands of small contiguous memory blocks at boot, and
  past 4096 of them every allocation failed even with plenty of memory free,
  so a disc read went into a block it never got. The kernel now holds as
  many blocks as memory has pages, so only running out of memory fails, as
  on the console.

## 1.0.31 (2026-11)

### Fixes
- StarCraft: Ghost's 2003 build froze on its title screen with 128 MB of
  console memory. D3D's push buffer went at the very top of memory, past
  the end of the area the GPU reads commands from (D3D leaves the top 20 KB
  of a development kit's 128 MB to the GPU), so its last commands never
  ran. With 128 MB those 20 KB now stay out of every allocation, as on a
  development kit.
- Development builds that import the debug monitor (xbdm.dll) no longer
  stop at the first call into it: its DmRegisterCommandProcessor is a stub
  that reports success (StarCraft: Ghost's GhostR.xbe stopped at its first
  video). Other xbdm imports are listed in the log.

## 1.0.30 (2026-11)

### Fixes
- WWE WrestleMania 21 stayed on a black screen at boot. It copies its
  1024 x 512 legal screen whole into its 640 x 480 front buffer, and the rows
  past the buffer's end ran over the commands it had just sent the GPU (and
  a page of its own code), so nothing after them ever ran. A copy whose rows
  are wider than the render target it goes into now stops at the target's
  edges, which leaves the target exactly as the console's copy does.
- Prince of Persia: The Two Thrones' menus and gameplay were washed out to
  a bright green and yellow haze. Its glow shrinks the frame in place,
  drawing each smaller copy into the corner of the same target while it
  reads the bigger one, and those reads were taken as each pixel reading
  itself, so nothing shrank and the frame was added over itself. A draw now
  reads its own target that way only where the texture is the target's
  full size.

## 1.0.29 (2026-11)

### Fixes
- Hunter: The Reckoning - Redeemer crashed at the copyright screen and
  stayed black (on every boot once its cache was on the hard disk). Its back
  buffer is a supersampled 640 x 960 one, bound before the video mode is
  set, and the two 640 x 480 front buffers cleared then were taken for
  640 x 960. Writing one back to memory ran 480 rows past its end, over the
  game's sound data. Until the video mode is known a target is now as tall
  as it's drawn, not as tall as that first buffer.

## 1.0.28 (2026-11)

### Changes
- 1.0.27's change for Prince of Persia: The Two Thrones on AMD graphics
  cards is taken back. It wasn't tested on AMD hardware and it isn't what
  draws the blocks, so images read before anything wrote them behave as in
  1.0.26 again. The blocks are still being looked into.

## 1.0.27 (2026-11)

### Fixes
- Prince of Persia: The Two Thrones on AMD graphics cards: the screen
  filled with blocks of other images over the menus and the game. Its glow
  shrinks the frame through targets down to 10x7 and samples them before
  all of them are drawn; an image read before anything wrote it held the
  memory's leftovers, which NVIDIA's drivers clear and AMD's don't. Such an
  image now reads as zeros (a depth buffer as its far plane) on every GPU.

## 1.0.26 (2026-11)

### New
- Console memory, 64 MB (retail) or 128 MB (debug kit), for all games or
  one (Settings › System, or a game's settings): betas, demos and debug
  builds made for a debug kit's 128 MB (StarCraft: Ghost's beta, Halo 2's
  E3 2003 demo and others) get it. A game marked for 64 MB keeps 64, as a
  debug kit runs it (given 128, a retail Halo drew nothing). The GPU
  reports the memory as well. On the command line: `ram=128`.
- Play a game file without adding it to the library: Settings › Interface
  › Play a game file, or drop the disc image on xbased's window.

## 1.0.25 (2026-11)

### Fixes
- 3D sound (Halo, Grand Theft Auto San Andreas and every game with
  positional effects and voices): muffled, smeared and too quiet. The
  console's HRTF filters, the head-related responses DirectSound loads
  for 3D voices, come as sign-and-magnitude bytes; they were read as two's
  complement, so a small negative tap (0x81, -1) became -127 and each
  response turned to noise across its 31 taps. Each was then divided by
  the sum of its taps' sizes, which left 3D voices 8 to 10 dB under the
  rest of the mix (dialogue among them). The taps are read as the
  hardware reads them and weighed as it does: the frontal response passes
  speech at about unity, and the sides show the head's shadow and the
  ear's resonance as they should. Savestates from before are converted as
  they load.

## 1.0.24 (2026-11)

### New
- A game with a frame rate patch of its own (60 or 120 fps, in Mods &
  patches) says so when the frame rate is unlocked: the patch is made for
  the game and keeps its speed right, where lifting the console's pace can
  break it. The frame rate option's help says so too.

## 1.0.23 (2026-11)

### New
- Cheats for 500 games: the Xbox scene's cheat files (.etm and .xbtf,
  the ones EvolutionX, Avalaunch and XBMC4Xbox ran as trainers) ship in
  `cheats/`, about a thousand of them, by title ID. A game's Mods & patches
  lists each file's options as `[cheat]` entries; switched on, the file's
  code runs once before the game's entry point and patches it, as the
  console's loader ran it. `.xbtf` files get the helper routines XBMC4Xbox
  loads for them. Your own files go in `cheats/<TITLE ID>/`.
- Over 160 widescreen, 60 fps and 480p patches ship in `mods/<TITLE ID>/`
  (icup321's and Slippy2k's XboxGraphicalPatches, matched to their games
  through xemu's title database; the 720p ones, which their authors call
  unstable, left out). They were made for the USA releases: a patch made for
  another release of a game can break it.
- Widescreen 16:9 (database) for 90 games: OgXHD's widescreen patterns,
  searched for in the disc's executable and written where they're found
  (their old bytes checked), so they hold across releases where the code is
  the same and do nothing where it isn't.

## 1.0.22 (2026-11)

### New
- CHD disc images (`chdman createdvd`, from an XISO or a full dump): in the
  library and on the command line, like an ISO, read through libchdr.
- Desktop shortcuts (a game's page › Create desktop shortcut): xbased with
  the game's image as its argument, which boots the game straight away, with
  the game's own icon. Windows: a shortcut on the desktop; Linux: a desktop
  entry there and in the applications menu. Steam and other front ends take
  the same command: `xbased "<image>"`.
- A game's page: Name (rename it; left empty, the disc's own name again),
  Choose cover image (a PNG or JPEG of your own as its box art) and Hide
  game. Settings › Interface › Show hidden games brings them back, dimmed.
- Settings › Interface › 12-hour clock (the dashboard's clock and the save
  dates).
- Downloaded content and title updates copied in by hand
  (hdd/Partition1/TDATA/<title id>/$c, $u) are signed for this console as
  the game starts. Signed for another console, or by a tool with another
  key, a game called them damaged.

### Fixes
- The frame rate counter wobbled between 58 and 62 at a steady 60: it
  counted frames in fixed half seconds, where one frame more or less is
  2 fps. It now measures the time between the frames themselves, over a
  second.

## 1.0.21 (2026-11)

### Fixes
- The King of Fighters: Maximum Impact (Maniax): a black screen at boot.
  The game closes each file by waiting on it until it is signalled, a
  file it read nothing from too. A file object's event was signalled only
  once an I/O of its own completed, so a file opened and closed unread
  was waited on forever. As on the console (the open's request completes
  on that event), a file is now signalled from its open: the game boots,
  plays its movies and reaches its menus and fights.

## 1.0.20 (2026-11)

### New
- Frame rate, per game (the game's Mods and patches): Original (the
  console's pace), Up to 60 fps, Up to 120 fps or Unlocked. The limits let
  a game draw faster than the console did but no faster than the chosen
  rate. Anything other than Original can break a game (its speed, physics
  or cutscenes), and the option's help says so. Fast forward lifts the
  limit while held. A game set to the old Unlocked stays Unlocked.

## 1.0.19 (2026-11)

### Fixes
- The Lord of the Rings: The Return of the King: walls and floors turned
  into a smear of another level's textures after playing for a while
  (a savestate loaded there showed them right). A texture is defined
  again when its memory is written, and that was seen through dirty
  page bits any other user of the same memory could take in first (another
  texture over it, a render target's copy). Each texture now also keeps
  its pages' write counts, which only grow, and is defined again when
  they change.

## 1.0.18 (2026-11)

### Fixes
- Disney's The Haunted Mansion: the cutscenes were black with their sound
  playing. The game copies each Bink frame into a frame buffer and draws it
  from there; the frame buffer's image was written back to memory as it was
  unbound, after the game had already written its next frame there (the
  command decoder runs behind the CPU), landing the older image over it.
  The CPU's writes now go into the image first.
- Settings: holding left or right on an on/off option changes it once,
  instead of repeating on to the next value.

### About
- xbased is an original Xbox emulator for Windows and Linux, developed by
  crazyriddler. Based on hakuX. The About screen, the executable's details
  and the Linux menu entry say so.

## 1.0.17 (2026-11)

### Fixes
- AMD graphics: Grand Theft Auto San Andreas' and Vice City's characters
  stretched into spikes. Their vertices are 9 bytes, 16-bit values at odd
  offsets, and AMD's vertex fetch reads a value only at an offset its size
  divides (NVIDIA's reads them anywhere). Such vertices are now copied with
  each value aligned, as they already were for Mali-G57.
- Windows: back from a game to the dashboard (the in-game menu's quit, or
  a game closing itself), the dashboard could still be left without the
  gamepad and keyboard until touched or clicked (an ROG Xbox Ally X). The
  game now brings the dashboard's window to the front itself as it ends,
  while it still has the foreground.

## 1.0.16 (2026-11)

### Fixes
- Windows: back in the dashboard after quitting a game from the in-game
  menu, the dashboard showed but had neither the keyboard nor the gamepad
  until clicked or Alt+Tabbed to. The game hid its window before handing
  the foreground on, and hidden it no longer had one to hand; it now hands
  it on first, and the dashboard takes the foreground as it comes back.

## 1.0.15 (2026-11)

### New
- Trigger calibration (Settings › Controls › Triggers, also from the in-game
  menu's settings): a bar per trigger shows its travel as you
  press it, with the dead zone and the full-press zone, beside what the
  game gets and how far it reached. Trigger dead zone (default 10%) and
  Full press at (default 100%: lower it for a trigger that no longer
  reaches its end). Past the dead zone a trigger now rises smoothly to full
  instead of jumping from nothing to a tenth.

### Fixes
- AMD graphics cards: geometry broken or missing in games with vertex
  programs (Def Jam: Fight for NY's menus black, with a stray band across
  them). AMD's drivers lack the 3-component 8- and 16-bit vertex formats;
  their 4-component forms were used, and the extra component, read past
  the array, became the position's w. It is now 1, as on the console.
- Windows: back from a game to the dashboard, or a game restarting itself
  (Crimson Skies after a mission), the window could end up behind others,
  its taskbar button flashing, as if minimized.

### Shader cache
- The compiled shaders are kept in one file per game (hle_spirv/<title
  id>.pack) instead of a file each: thousands of small files took far more
  disk than their size and were slow to open on a hard disk. The existing
  ones are packed into one file (legacy.pack) the first time, nothing to
  compile again.

## 1.0.14 (2026-11)

### Fixes
- Def Jam: Fight for NY: froze at the intro movie (black screen or a still
  frame, the game never reaching its title). Its 60 Hz input timer
  schedules each tick from the last through D3D's SetTimerCallback, which
  refuses a time already passed; one tick a frame late (the emulation held
  up a moment) and the timer stopped for good, and with it the game's
  updates. Such a tick now comes at once and the game catches up: it
  reaches the menus and plays.
- PTIMER's alarm: at a title's first alarm, its thread could start before
  the alarm was set and raise a stray alarm interrupt flag.

## 1.0.13 (2026-11)

### Fixes
- Prince of Persia: The Sands of Time: a band of stray colour (green,
  purple) down the right of the screen in the menus and in game. The game's
  glow adds four sizes of a blurred picture in one pass, each smaller than
  the image it sits in (the extra columns are never drawn); only the first
  was kept to its own size, and the others' filtering read the stale
  columns. Every texture of such a pass is now kept to its own size.

## 1.0.12 (2026-11)

### New
- Import from xemu (Memory, Y): xemu's game saves (its hard disk's E:
  UDATA and TDATA) copied into xbased and re-signed, so games accept them.
  Copied as they were, a game found them damaged: xemu's console signs
  saves with its own keys (from its BIOS and EEPROM), xbased with its own.
  xemu's settings (xemu.toml) are found where xemu keeps them, or chosen;
  its BIOS, boot ROM, EEPROM and qcow2 or raw hard disk image are only read.
  Saves whose game isn't in the library are copied but not re-signed: add
  the game and import again. xemu_import.log beside xbased lists each file.
- Add a game file (Settings, below the game folders, or Y in an empty
  Games screen): a
  disc image the folder scan doesn't find (another extension, another
  place) added on its own; it stays in the library.

### Fixes
- Halo: Combat Evolved: starting a campaign level closed the game back to
  xbased's menu. Halo lays out its game state and tags from 0x61000 to past
  16 MiB, and D3D's second page of visibility-test results has to be below
  16 MiB: the kernel kept 256 KiB of low memory it doesn't use (the Xbox
  kernel lets its start-up memory go), none was left, and the game read
  through the page it didn't get.
- Grand Theft Auto III and Vice City: a black screen at start. Their timer
  reads save the registers with the 16-bit PUSHA/POPA, which stopped the
  game; they run now.
- Quit from the in-game menu, and a game closing itself (a title opening
  the Xbox dashboard), could leave a black full-screen window that had to
  be closed by force: the process now ends at once, without waiting on the
  graphics, gamepad and audio drivers' shutdown.
- A game opening the Xbox dashboard's Xbox Live account page (creating or
  recovering an account) with no dashboard installed now says so instead
  of just closing.

### Dashboard
- Long messages and questions wrap over several lines instead of being cut.

## 1.0.11 (2026-11)

### Fixes
- Mission: Impossible - Operation Surma: a new game stopped on the "disc is
  dirty or damaged" screen. The game copies its levels to the hard disk's
  cache in the background, keeping each copy's file open and unshared until
  it is complete, and loads a level from the cache only when it can open it
  (from the disc meanwhile). Files on the hard disk now keep the share
  access their handles were opened with, as the Xbox's do: an open of a
  copy still in progress is refused, and the level loads from the disc.

### Picture
- Upscaling with AMD FidelityFX Super Resolution 1 (its EASU edge
  reconstruction and RCAS sharpening) in place of Snapdragon GSR: Settings ›
  Video › Upscaling › AMD FSR 1. A setting that chose GSR (global or a
  game's) now chooses FSR; bilinear stays bilinear.

## 1.0.10 (2026-11)

### New
- PAL games at 60 Hz by default, as a PAL console with its PAL-60 setting
  on runs them (Panzer Dragoon Orta, 50 fps before, plays at 60). Settings ›
  System › PAL games sets 50 Hz instead, for all games or one (its
  settings). A game without a 60 Hz mode stays at 50, as on the console.

### Fixes
- Driv3r: Tanner drawn with flat, skin-coloured legs and a bald head,
  where he has jeans and hair. The game's characters add
  an environment cube map through a texture stage whose coordinates their
  vertex program leaves unwritten; a PC GPU's look-up along no direction
  returned a bright texel, and the game's tint turned it into skin tone over
  the whole body. That look-up now gives black, and Tanner looks as he
  does on the console.
- Save states no longer depend on where xbased's folder is: they kept the
  full path of each file the game had open, so a state stopped loading once
  the folder moved (or reopened the old folder's files), and a Windows
  state didn't load on Linux. They now keep each file's place in the hard
  disk folder; states from earlier versions load too.
- Windows: no console windows flash as xbased starts (the update check and
  the box-art downloads ran curl in one each).
- The mouse pointer hides itself: hidden when xbased opens and during play,
  it shows when the mouse moves, and hides again after a few still seconds
  or when a key or a gamepad is used.

## 1.0.9 (2026-11)

### New
- Stick range, in Settings › Controls (and in the in-game menu's settings):
  a screen that shows both sticks live. A green square is the range the
  games expect (the Xbox controller's); a white outline is the range your
  stick reaches, as the game gets it, with its reach in percent. Rotate the
  sticks once around and adjust while watching it:
  - Range, 50% to 150%: above 100% for a worn stick that doesn't reach its
    edge, below for games that turn too quickly.
  - Shape: Square (Xbox), the default, or Round (the stick's own circle).
  - Dead zone, also here.
  While the screen is open, the stick doesn't move through the menu.

## 1.0.8 (2026-11)

### Fixes
- After a game closed, the dashboard's settings showed their defaults until
  xbased restarted, and a setting changed then wasn't saved. Reloading the
  settings file (a game may change it) lost its path on the way. The games
  themselves kept the right settings all along.
- Analog stick diagonals (Tony Hawk's Pro Skater 2x's tricks, a direction
  and a button, and other games of the time): a modern stick pushed fully
  diagonal reached only two thirds of each axis, where the Xbox controller
  reached nearly all of it, so games read a straight direction, or none. The
  stick now responds like the Xbox controller's: full deflection is full on
  both axes at any angle, and the dead zone is a circle around the centre
  instead of a band along each axis.

## 1.0.7 (2026-11)

### New
- Updates: as the dashboard starts, xbased looks for a newer release on
  GitHub and offers it: Update, Not now, or Don't ask again. Update downloads
  it and installs it over this copy (the Windows folder, or the Linux
  AppImage), then starts it; settings, saves, states and the hard disk stay
  as they are. Settings › Interface › Check for updates at startup turns the
  check back on (or off). A copy that can't replace itself opens the
  release's page instead.
- Audio output, in Settings › Audio: Stereo (the default) or 5.1 surround.
  In 5.1 the console reports Dolby Digital, the games mix for six speakers,
  and xbased plays the six channels (front, center, subwoofer, rear); on
  stereo speakers the system mixes them down. Stereo is now set whatever an
  older console image (EEPROM) had kept.

## 1.0.6 (2026-11)

### Picture
- Internal resolution up to 8x (5120x3840 for a 640x480 game), from 4x:
  5x and 6x for 4K screens, 7x and 8x as supersampling. A target too big for
  the GPU's largest image at the chosen scale renders at the largest one that
  fits. Fable plays at its full 30 fps at 8x.

## 1.0.5 (2026-11)

### Credits
- xbased is crazyriddler's Windows and Linux edition of hakuX by rfandango,
  and now says so everywhere: the About screen (in every language, with
  github.com/crazyriddler/xbased), the README, the program's version
  information on Windows and its desktop entry on Linux.

## 1.0.4 (2026-11)

### Fixes
- Fable: it stopped on a black screen as it booted. Its own memory pager
  leaves each page fault with an IRETD, which the core didn't run. Fable now
  plays.
- Games on a console set to a language their disc doesn't have (Spanish on
  Fable, The Thing, Call of Duty: Finest Hour...): they start in English, as a
  console would have them do. Swapping in the English files under the other
  language's name wasn't enough: Fable saw that language's folder missing,
  loaded half of it and crashed.
- The Thing: it stopped at boot. The kernel now has I/O completion ports
  (CreateIoCompletionPort, GetQueuedCompletionStatus,
  PostQueuedCompletionStatus), with packets from reads and writes of the
  files tied to them. The Thing plays.

## 1.0.3 (2026-11)

### Fixes
- Windows: the "disc is dirty or damaged" message in games that copy disc
  files to their cache on the hard disk (Panzer Dragoon Orta and others).
  Such a game writes each file under a temporary name and renames it while
  it is still open, which Windows refused: the cache stayed broken and the
  game reported a bad disc. Files are now opened so they can be renamed or
  deleted while open, as on the Xbox and on Linux. Panzer Dragoon Orta plays
  its stages.

## 1.0.2 (2026-11)

### Fixes
- Games that stopped forever on a loading screen while one of their threads
  waited for another of the same priority (NFL Fever 2004 before its
  matches): giving up the CPU now takes a step off a thread's wait boost, as
  on the Xbox, so the thread it waits for runs. NFL Fever 2004 plays its
  matches at full speed.

## 1.0.1 (2026-11)

Fixes for games that stopped with their "disc is dirty or damaged" message,
or closed at start.

### Fixes
- Windows: xbased in a folder whose path has an accent, an ñ or letters of
  another alphabet (`C:\Users\José\...`) put its virtual hard disk in another
  folder, or nowhere. Games that write their cache then stopped with the
  dirty-disc message, or closed. xbased now uses UTF-8 paths throughout.
- Windows: when xbased's folder can't be written (Program Files, a read-only
  copy, Windows' controlled folder access on Desktop or Documents), its data
  goes to `%LOCALAPPDATA%\xbased` instead of failing.
- Disc images trimmed too far by an image tool: reads past the image's end
  return zeros, as on the disc, instead of the dirty-disc message.
- Disc images whose directory tree an image tool left damaged: every file is
  still found.
- A disc read that fails is tried again, through a path the emulated
  console's memory protection can't refuse.
- Memory: an allocation too big to fit gives back what it took. Games that
  ask for all the memory they can get (BloodRayne 2) closed at start; they
  run now.

## 1.0 (2026-11)

The first release of xbased, crazyriddler's Windows and Linux edition of
[hakuX](https://github.com/rfandango/hakuX) by rfandango.

### Emulation
- hakuX's core on x86-64: the guest's code translated to native x86-64, its
  return addresses predicted, interrupts delivered at block boundaries.
- The Xbox kernel's thread priority boosts, so games that lean on them
  (NightFire and others) keep their frame rate.
- Frame-rate unlock, per game, that reaches hundreds of frames per second
  where the game allows it, and that sleeps instead of spinning when capped
  (low power on laptops and handhelds).
- Per-game patches as mods (higher frame rates for a set of games so far),
  toggled per game. IPS patches shared online work too: drop them into
  `mods/<TITLE ID>/` (MODS.md).
- Anisotropic filtering up to 16x, alongside FXAA, internal resolution and
  upscaling.
- Games start in the console's language, or in English when the disc doesn't
  have that language's files.
- Save states with thumbnails, 10 slots per game.

### Front end
- A console-style dashboard: animated orb, hexagons and fog, 3D transitions,
  navigation sounds (can be turned off), a clock.
- Game library with box art (libretro-thumbnails) and the discs' own icons.
- Per-game settings, mods and cheats, controls and states.
- Memory manager for the games' own saves, with their icons: export, import,
  delete.
- In-game menu (F1, or Back + Start on a gamepad): resume, states, settings,
  controls, restart, quit.
- Borderless full screen by default, like a console; Exit from the dashboard,
  with a confirmation, so it closes from a gamepad.
- Controls: any key, button or axis per input, 4 players, per-game mappings,
  configurable shortcuts; prompts follow the device in use.
- CRT filters (scanlines, TV, arcade), frame counter, volume and
  mute-in-background.
- Online: Insignia, XLink Kai and System Link.
- 11 languages, the system's by default; the console's language follows.
- Any window size, aspect ratio and DPI.

### Platforms
- Windows 10/11 x64: a portable folder, system DLLs only.
- Linux x86-64: an AppImage (glibc 2.35 or newer: Ubuntu 22.04, Debian 12,
  Fedora 36 and later); settings in `~/.local/share/xbased`.

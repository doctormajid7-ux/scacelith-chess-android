# Scacelith

A photorealistic chess game played in first person, seated at a waxed wooden table in a sunlit
royal hall, against a porcelain robot driven by Stockfish. Everything is rendered by an in-house
engine written for this game on OpenGL 4.6 (no third-party engine): procedural geometry,
real-time material shaders, physically based lighting.

## This fork: the Android version, and a lighter renderer

This repository is a fork of [DarkCenobyte/scacelith-chess](https://github.com/DarkCenobyte/scacelith-chess)
that adds an **Android** version of the game and a **simple renderer** that keeps modest machines
fluid. Its [releases](https://github.com/doctormajid7-ux/scacelith-chess-android/releases) hold the
Android APK next to the Windows and Linux builds.

### Android (arm64-v8a)

The whole game, from the same sources as the desktop one: the hall, the robot, Stockfish 19, the
coach and its voice.

- **Requirements**: Android 8 or later, a 64-bit ARM phone or tablet with an OpenGL ES 3.2 GPU.
  Tested on a Snapdragon 8 Elite Gen 5 (Adreno 840): 60 to 100+ FPS at the screen's full
  resolution, with a 120 Hz display mode requested.
- **Installing**: download `Scacelith-<version>-android-arm64.apk` from the releases and open it
  (allow installing apps from that source). It is about 250 MB: Stockfish's neural network is
  embedded. The app asks for one permission only, the network.
- **Stockfish 19** is built in for arm64 (armv8 and armv8-dotprod, chosen at run time), its NNUE
  network included: every difficulty level plays as on the desktop.
- **The spoken coach**: the voice model (Supertonic 3, ~139 MB) downloads from within the game
  over HTTPS (Android's own TLS stack), and the speech is synthesised on the phone (portable
  kernels, vectorised for ARM NEON, checked against the x86 ones by the desktop tests).
- **Touch controls**, chosen in Options > Game > Touch control:
  - *Touchpad (arrow)*, the default: the finger moves an arrow, a tap clicks where the arrow is, a
    still long press holds the button (to carry a piece); gliding never presses.
  - *Direct (finger)*: the finger is the pointer: tap a piece, then its square (or drag it).
  - In both: two fingers look around, a pinch leans in or out, a two-finger tap presses the chess
    clock, a three-finger tap opens the menu.
  - A strip of buttons gives the keys a touchscreen has no gesture for: menu, chess clock, move
    list, look at the board, scoresheet, take back, confirm, keyboard.
- **Options for a phone**: the simple renderer (below), the render scale, and the **text size**
  of the interface (Options > Display, 80 to 160 %, 120 % by default on Android).
- **Not on Android yet**: live online games and direct matches (no WebSocket client and no
  AES-GCM / ECDH on this build yet). Single player, the coach, two players on one device and the
  observer mode all work.

The port in short: OpenGL ES 3.2 instead of OpenGL 4.6 (the renderer's direct state access is
emulated in `src/gl/gl46_gles.cpp`, the shaders are emitted as GLSL ES 3.20), AAudio for the
sound, a JNI bridge to `HttpURLConnection` for HTTPS (`src/net/transport_android.cpp`). Building
it: see [Android](#android) below and `docs/ANDROID.md`.

### The simple renderer (all platforms, on by default)

The original renderer is demanding (procedural materials evaluated for every pixel, screen-space
reflections, volumetric light, a compute post-processing chain). This fork adds a light one and
**makes it the default on Windows and Linux too**, so the game runs well on modest configurations
(integrated graphics, a GTX 1050) as well as on phones. The full renderer is one switch away:
Options > Graphics > Simple renderer.

The simple renderer draws the scene in one forward pass with the sun and its shadow, the light
probes and a plain tone mapping, and has its own switches in Options > Graphics:

| Option | What it does | Default |
|---|---|---|
| Material textures | *Plain* colours, *Baked* (each material's real procedural surface computed once at load into textures: marble veins, stone, wood, tapestry), or *Full* (procedural per pixel, demanding) | Baked |
| Indirect lighting | the light the sun bounces around the hall, and its soft reflections | On |
| Anti-aliasing (MSAA) | 4x multisampling | On (desktop), off (Android) |

### Other changes in this fork

- **The chess clock's display has a soft backlight**: its digits stay readable when it stands in
  the robot's shadow (playing Black).
- **Linux, laptops with two GPUs**: the game asks for NVIDIA's PRIME render offload by itself when
  the NVIDIA driver is loaded (as `prime-run` does) and falls back to the integrated GPU when the
  offload is unavailable; `SCACELITH_INTEGRATED_GPU=1` keeps the integrated one.
- `scacelith_gles` (`ninja -C build scacelith_gles`, Linux): the Android renderer built for the
  desktop on an OpenGL ES 3.2 context, to check the ES path without a device.

## Installing

The [releases](https://github.com/DarkCenobyte/scacelith-chess/releases) hold the game for Windows
x64 and Linux x86-64; both need a GPU with OpenGL 4.6, and Stockfish 19 is built in.

- **Windows**: unpack `Scacelith-<version>-windows-x64.zip` anywhere and run `Scacelith.exe` (or
  download the executable alone).
- **Linux**: unpack `Scacelith-<version>-linux-x86_64.tar.gz` and run `./scacelith`;
  `./install.sh` adds the game to your applications menu (in `~/.local`, no root needed) and
  `./install.sh --uninstall` removes it. It needs glibc 2.34 and OpenSSL 3 (Ubuntu 22.04,
  Debian 12, Fedora 36, RHEL 9 and later), X11 or Wayland with XWayland, and plays its sound
  through ALSA (`libasound2`; PulseAudio and PipeWire through their ALSA plugins). Without
  `libasound2` or a sound device the game runs silent, the coach in subtitles.

## Playing

- **Left click** on one of your pieces: your hand reaches for it. Tournament rules apply: a piece
  you touch must be moved if it has a legal move (touch-move).
- **Left click** on a square: the piece is played there. Captured pieces are put beside the board.
  You can also drag: press on the piece, release on the square. While a piece is in hand, the
  square under the pointer is outlined on the board, your own pieces never get in the way of the
  pointer (it looks through them at the square behind), and your arm turns see-through.
- **Space**, or a click on the chess clock: your hand presses the clock. A move is only completed
  once the clock is pressed, and after a promotion the new piece must be on the board first.
  With Options > Gameplay > Auto-press clock your hand presses it by itself once the move is on
  the board (capture, castling rook and promotion piece included); online, the server (or the
  host of a direct match) decides for each game. A game without a time limit ("No clock") has no
  clock press at all, for both sides and in every mode: the move is completed as its last piece
  is released (FIDE 4.7), the turn passes at once, and the clock shows dashes.
- **Right mouse button** (hold and drag): look around from your chair. The pointer at the top of
  the window: look up at your opponent. **Mouse wheel**: lean towards the board. **Middle click**
  or **C**: look at the board again.
- **S**: look at your own scoresheet, lying out of sight beside you, and back (**S** again,
  **C** or a look around).
- **Tab**: move list. **Esc**: menu (offer or claim a draw, resign, options).

At the table the game draws its own pointer, which shows what a click will do: a gold ring over a
piece you can touch or over the clock, a sight with a gold centre over a square the piece in hand
can go to (Options > Gameplay > Game pointer switches back to the system arrow).

A new game asks for the opponent's strength (Stockfish presets, or custom Skill Level / Elo /
depth / move time / nodes) and the time control (unlimited, 1+0, 3+0, 3+2, 5+0, 5+3, 10+0, 10+5,
15+10, 30+0, 30+20, 90+30, or custom with increment and delay). Your colour is random for the
first game, then alternates. Every physical action (touching, moving, capturing, promoting,
pressing the clock) takes exactly the same time for both players, and each clock only stops when
the lever is actually pressed.

Illegal moves are only possible when legal-move hints are turned off (Options > Gameplay). As in
tournaments, the arbiter then restores the position, gives the opponent extra time, and a second
illegal move loses the game.

### Scoresheets

As in tournaments, both players record the game on their own scoresheet, a pad lying on the side
of the table opposite the clock. Right after every clock press each player writes the move down
with the hand on the pad side, while the other hand stays free to play and press the clock, so
keeping score never costs clock time. The player whose clock stands on the left therefore plays
with the left hand and writes with the right. The header is filled in when the game starts (event
"Scacelith", date, round, the players' names and ratings: your name from Options > Player,
"Human" by default (translated with the interface), and "Stockfish"), a full page of 40 moves is turned over the top of the pad,
and the result is written before the final handshake. You hear the pen on the paper and the page
being turned.

Each player has a handwriting of their own (Caveat, Marck Script or Bad Script for Latin and
Cyrillic; names in other scripts are written in Aref Ruqaa, Klee One or LXGW WenKai), and moves use
the piece letters of the interface language.

### Your Elo

Your rating is computed after every game against Stockfish as FIDE computes tournament ratings
(FIDE Rating Regulations, 2024), with the same rules as the online server's:

- **First rating.** You start unrated, shown at 1500. Your first five games give your first
  rating, FIDE's way: the average of your opponents' ratings, counting two extra draws against
  1800-rated players, plus FIDE's rating difference for your score (for example five draws against
  1500-rated opponents give 1586, five wins 1895). A first rating is at most 2200. As FIDE
  ignores a newcomer's first event without a point, the games you lose before your first draw
  or win do not count towards it.
- **Then** each game changes your rating by K × (score − expected score), where the expected
  score comes from FIDE's table for the rating difference (counted as 400 points at most): K = 40
  for your first 30 counted games (those of your first rating included, not the losses before
  your first draw or win; the rating is provisional until then), 20 afterwards, 10 once you have
  reached 2400. A rating never drops below 100.

The opponent's rating is the preset's (Novice 800 up to Stockfish Max 3500; a custom opponent is
rated from its Skill Level or UCI Elo). A game counts once both players have moved; leaving a
game, or closing the game, resigns it. The title page shows your rating, record and peak, the new
game page the score you can expect against the selected opponent, and the game over card the
change (`Elo 1512 → 1524 (+12)`). Watched games are never rated. Games are rated one by one, where
FIDE rates a month's games together.

### Two players on one PC

Set **Opponent** to **Human, same PC** on the new game page to play a friend at the same computer,
each from their own robot's eyes. The page asks for both names (White defaults to your name from
Options > Player, Black to "Player 2"), each player's handwriting, the time control, the side of
the clock (at White's or at Black's right: the player whose clock is on their left plays with the
left hand), and whether the game is rated. **Swap colours** exchanges the two players.

- The player to move has the mouse and the keyboard, with the same rules as against Stockfish
  (touch-move, pressing the clock by hand, the arbiter, the claims in the Esc menu).
- Once the clock is pressed (without a clock: once the move is made), the camera flies over the
  table into the other player's eyes, and both clocks stand still until it lands. Options >
  Gameplay > Hot-seat handover sets the flight length (0.8 to 2 s) or an instant cut through
  black. Buttons still held when the view leaves are ignored until released, and each player
  keeps their own look (right drag, wheel).
- Each player fills in their own scoresheet in their own handwriting. The mover writes the move at
  once; the next player writes it when the view reaches them, or after their own move if they
  touch a piece first.
- **Esc** offers a draw (it goes with your next move, as in FIDE 9.1.2: the opponent sees an
  accept / decline card when the view reaches them, and touching a piece declines it), claims a
  draw, or resigns for the player to move, confirmed with their name.
- The top left corner shows both names (and ratings); a caption names the player whose turn begins.
  The game over card names the winner, and the rematch swaps the colours (the clock follows its
  player, so each keeps the same hand).

A two-player game is friendly by default and never changes your rating against Stockfish. A rated
one keeps a separate rating per name (`[local_player_N]` in the settings file; a new name starts
unrated at 1500, with the same FIDE rules), updated for both players against each other's rating
before the game (a game between two unrated names counts for both, unless it is lost by a name
that has not drawn or won yet: FIDE then ignores it for both); leaving a game early rates
nothing.

## Coach

**Coach** on the title page seats you in front of a robot with "COACH" on its chest, a teacher
who plays you at your level and talks you through the game. The Coach page chooses the level and
your colour (White, Black, or alternating from one game to the next); the choice is remembered
(`[coach]` in the settings file).

- **The rules of chess** (level 0) is an interactive lesson, always with White: the coach sets up
  small positions and shows how each piece moves and captures, then castling, promotion, check
  and checkmate, and you try each idea on the board. Legal-move hints are on, touch-move is
  relaxed (a piece can be put back), nothing is written down; leaving it resumes at the same
  chapter next time.
- **Levels 1 to 6** (from *First steps* to *Expert*) are real games against the coach's Stockfish
  at that strength, with its teaching repertoire in the opening. The lower the level, the plainer
  the words; levels 0 to 2 show the board's coordinates. There is no clock, the game is never
  rated, the scoresheets name the coach with its level's rating, and touch-move and the hints
  option apply as in a normal game.
- After your move the coach may say what it saw: a threat, a good move, a mistake. It points at
  the pieces and squares it talks about, which light up in blue on the board, and it can show a
  line by playing it with its own hand, then put the pieces back. After a blunder it offers to take
  your move back: **Take back** or **Backspace** accepts, **Play on** or touching one of your
  pieces declines.
- **Space** skips what the coach is saying. **Esc** opens the menu: take back my move (your last
  move and the coach's reply, as long as neither scoresheet has written them), offer or claim a
  draw, resign, options, main menu (abandons the game).
- At the end the coach says a word on the game, shakes your hand and sums the game up; the end
  card offers to play again at the same level. When the coach suggests another level, the Coach
  page proposes it next time.

The coach speaks with a voice synthesised on your computer (Supertonic 3). Its model is not part of
the game: the first time you open the Coach page (or switch Options > Audio > Coach voice on), the
game offers to download it, about 145 MB, from Hugging Face (or, if that fails, from the sherpa-onnx
release on GitHub), into the game's folder of application data: `%APPDATA%\scacelith\coach\` on
Windows, `$XDG_DATA_HOME/scacelith/coach/` (by default `~/.local/share/scacelith/coach/`) on Linux.
The prompt shows the model's licence (OpenRAIL-M) and its use restrictions; "Not now" switches the
coach's voice off. A small panel in the corner shows the download while you keep playing, and an
interrupted download continues where it stopped. Without the model, with Coach voice off, or
without a sound device, the coach speaks through subtitles only. Options > Audio > Subtitles shows
its words at the bottom of the screen (Automatic: when it does not speak the language of the menus,
as with Chinese menus where it speaks English), and Options > Audio > Voice volume sets its volume.
`[tts]` in the settings file tunes the synthesis: `threads` (0 = 2), `voice` (-1 = the default
voice), `steps` (5) and `arch` (`auto`, or `avx512`, `avxvnni`, `avx2`, `sse2`, `scalar` when
troubleshooting).

## Watch a Game

**Watch a Game** on the title page lets two Stockfish players (one preset per side, and a time
control) play each other while you move freely around the hall, invisible to them. The choice is
remembered (`[viewer]` in the settings file).

- **W A S D** or **Z Q S D** (AZERTY) or the arrows: move along the view. **E / Space** and
  **C / Ctrl** (or Page Up / Down): up and down. **Shift**: three times faster.
- **Right mouse button** (hold and drag): look around. **Mouse wheel**: movement speed.
- **1 – 9**: fly to a viewpoint (beside the table, above the board, the hall, the clock, White's
  face, Black's face, the duel, the windows, the tapestries). **0**: through the eyes of the player
  to move; after each move the camera flies over the table into the other player's eyes.
- **Tab**: move list. **H**: hide the controls. **Esc**: menu (resume, options, main menu).

The players claim and offer draws like the opponent of a normal game (repetition, fifty moves, an
equal position late in the game), and the game over card offers to watch another game.

## Online play

**Play Online** on the title page plays people through a Scacelith server, still in the first
person: your opponent sits in the other chair as a robot that moves with them, live: it takes the
piece they touch, holds it over the square they aim at, looks where they look and leans in when
they do.

- **Server.** The official server is `caissa.scacelith.com` (port 443, secure web API and
  secure WebSocket on the same port). Options > Online server > Custom server takes a community
  server instead: host (domain or IP), HTTPS/API port, WSS port (empty = the API port) and, for a
  server with a self-signed certificate, its **Certificate fingerprint (SHA-256)** as its owner
  gives it (64 hexadecimal characters, colons allowed; empty = the Windows certificate store).
  **Test connection** shows the server's name, message and whether it runs a compatible
  version. Google sign-in works only when the server is added under the public host name and API
  port its owner gives. You sign in separately on each server: an account and its sign-in are
  never shared between servers, and nothing secret is written to `Scacelith.ini`.
- **Account.** Sign in with your user name or e-mail and password (and the code of your
  authenticator app once two-factor authentication is on), or with Google. New accounts confirm
  their e-mail address (the page can send the link again); a forgotten password is reset by
  e-mail. The account page lists your rating in every time control (`1500?` while it is
  provisional, with games and wins / draws / losses), changes the password, turns two-factor
  authentication on (QR code or key, then ten recovery codes shown once) or off, makes new
  recovery codes, and signs you out here or everywhere.
- **Finding a game.** Pick a time control (your rating in each is under it), rated or casual, and
  **Find opponent**: a card counts the waiting time and shows the rating range searched. You can
  also challenge a player by name (any time control; only the official ones can be rated, with
  the colour you want), or create a private game whose short code a friend enters to play you.
  Challenges you receive appear as a card wherever you are in the menus, and at the table
  between two games.
- **At the table.** You can only let go of a piece on a legal square. When the server lets the
  robots press the clock (its default; in a direct match, the host's choice), the move is sent the
  moment you choose it, and the robot hand then places the piece and presses the clock. Otherwise
  a notice says so at the start of the game: the robot places the piece, you press the clock
  (Space or a click), and the move goes at the press. The opponent's robot plays their move from
  where their hand is, and its head follows theirs unless Options > Gameplay > Ignore opponent's
  head movements is on (the robot then looks around by itself, as against Stockfish; it still
  moves the pieces with them). The clocks are the server's (they never stop, not even in the Esc
  menu). The ping to the server is in the top right corner. Esc: offer or claim a draw, resign,
  abort before your first move, report the opponent, leave (which resigns, or aborts before your
  first move; closing the window does the same). If your opponent loses
  the connection a banner counts down the time they have to come back; if yours drops, the game
  waits behind a "Reconnecting…" veil and picks up where the server is. The scoresheets are headed
  with the server's name, "Online", the time control, rated or casual, both players with their
  ratings and the game's number; the game over card shows the rating change and offers a rematch.
- **Direct match.** Two computers play each other directly, without a server or an account
  (friendly games, never rated): one player **hosts** (time control, colour, port 47100 by
  default, opened on the home router with UPnP when possible) and reads the address, the port and
  a code (`XXXX-XXXX-XXXX`) to the other, who **joins** with them. The page says whether the
  router opened the port, when to forward it by hand, and when the internet provider shares the
  address (carrier-grade NAT: try IPv6 or a VPN).

Anyone can run a server: see [DarkCenobyte/scacelith-chess-server](https://github.com/DarkCenobyte/scacelith-chess-server). The game speaks
the realtime protocol v1 with it, whose specification is kept in this repository too, in
[protocol/](protocol/README.md).

Development: `--online-mock` replaces the network with an in-process fake server and a fake
direct-match friend (any password works; see `src/game/online_mock.h` for the inputs that try
error paths), and `--start-online [category]` goes straight to a game (with `--online-mock` the
opponent is a random mover whose hands and head move like a player's; `--online-manual-clock`
leaves the clock press to the players, and `--play e2e4,...` makes your moves). In a mock game F9
makes the opponent disconnect for a while and F10 drops your own connection. `--scene ui
--ui-screen online-play` (and the other `online-*` and `direct-*` screens listed in
`src/ui/ui_viewer.cpp`) shows the pages on the fake server.
[docs/ONLINE_CLIENT.md](docs/ONLINE_CLIENT.md) describes the client side. QR codes are drawn with
Nayuki's [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) (MIT licence,
`third_party/qrcodegen/`).

## Options

Settings are stored in `Scacelith.ini` in `%APPDATA%\scacelith\` (on Linux
`$XDG_CONFIG_HOME/scacelith/`, by default `~/.config/scacelith/`), with the saved logins
(`Scacelith.credentials`) and the log (`scacelith.log`). A `Scacelith.ini` next to the executable
makes a portable install (versions up to 1.0.0-beta.1 put it there): the game then keeps all three
in the executable's folder, as it also does when the user folder cannot be written. A file given with `--ini <file>` is read and written there only, with the
logins beside it (the log warns when it cannot be written). All of them are
editable from the Options page: display mode and resolution, V-sync, render scale, quality
preset, motion blur, depth of field, brightness, volumes, ambience, legal-move hints, auto-press
clock, the opponent's head movements, mouse sensitivity, the game pointer, and the hand-over
between the two players of a game on one PC (a camera flight, or an instant cut). An option
marked with a small circled **i** after its name has a definition: rest the pointer on the name
or on the mark, or keep the keyboard focus on the row for a moment, to read it.

- **Auto-press clock** (Options > Gameplay, off by default): your robot presses the clock by
  itself once your move is on the board. In online games the server decides, and in a direct
  match the player who hosts it (**Auto-press clock** on the Host a game page, on by default).
- **Ignore opponent's head movements** (Options > Gameplay, off by default): in online games and
  direct matches the opponent's robot looks where its player looks; with this option it moves
  its head by itself, as against Stockfish.

The first start opens on a brightness calibration: three squares, black on the left, mid grey and
white on the right, each with a black knight, drawn exactly as the 3D hall would show them at the
brightness of the slider. Move the slider until the knight on the black square is barely visible,
or no longer visible, and **Continue** (Esc keeps the current value); Options > Graphics >
Brightness changes it later. Until Continue or Esc, every start opens on it: a first start closed
during the loading, or a first run straight into a game, shows it the next time (screenshot runs
never do). `--calibrate` opens the page again, and the UI viewer shows it with
`--scene ui --ui-screen calibration`.

The interface speaks English, French, German, Spanish, Ukrainian, Russian, Arabic (laid out right
to left), Japanese, Simplified Chinese and Traditional Chinese. The first start follows the
system language; Options > Display > Language changes it at once. Options > Player holds your
name and the handwriting in which you fill in your scoresheet, with a preview. Translations live
in `assets/i18n/<code>.lang` (one `key = text` per line, English is the reference and the
fallback); `--lang <code>` overrides the language for one session.

## Building

Requirements: CMake 3.21+, Ninja, GCC with GNU binutils (tested with GCC 13; on Windows,
MinGW-w64 GCC with POSIX threads; Clang cannot build the embedded Stockfish variants), and
Python 3 for the instruction-set audits and the Windows exception table check that run at every
build (a Windows Release build, the shipped exe, does not configure without it; other builds skip
them with a warning). The Linux build also needs the OpenSSL 3, X11 and OpenGL development files
(for example `libssl-dev`, `libx11-dev` and `libgl-dev`); ALSA is loaded at run time
(`libasound.so.2`), not linked, and a Linux Release build links libstdc++ and libgcc in, so that
the executable runs on other distributions than the build machine's. The Windows build is produced with
MinGW-w64 (native or cross-compiled from Linux) and is a single self-contained executable
(Stockfish 19 and its neural network are embedded). Stockfish is compiled once per x86-64
instruction set, from plain x86-64 to AVX-512, and the game runs the best one the CPU supports;
`-DSCACELITH_SF_VARIANTS=x86-64-avx2` (or another variant the CPU runs) builds a single one, for
quicker local builds (see `third_party/stockfish/README.scacelith.md`). The first configure also
downloads the coach's voice model archive (129 MB, from the sherpa-onnx release on GitHub) into
the build folder, for the unit tests and `--coach-dir build/coach`:
`-DSCACELITH_SUPERTONIC_DIR=<extracted folder>` or `-DSCACELITH_SUPERTONIC_ARCHIVE=<.tar.bz2>`
take a local copy instead, and `-DSCACELITH_SUPERTONIC_DOWNLOAD=OFF` does without it (the tests
that need the model are then skipped; see `third_party/supertonic3/README.scacelith.md`).

```sh
# Windows x64 (cross-compiled from Linux)
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-win            # -> build-win/Scacelith.exe

# Linux x86-64 (the game, its tests and headless screenshots)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/scacelith_tests

# The Windows tests under Wine (ninja -C build-win scacelith_tests first)
tools/test_win.sh             # [filter-substring]

# Android (arm64-v8a, NDK 27.2.12479018, JDK 17, Gradle 8.13; no wrapper in the repository):
# the optimized APK, signed with the debug key so it installs as is
cd android && ANDROID_HOME=$HOME/android-sdk JAVA_HOME=/usr/lib/jvm/java-17-openjdk \
    gradle --no-daemon assembleRelease   # -> app/build/outputs/apk/release/app-release.apk
```

<a id="android"></a>The Android target is a port of the same sources, not a second program: OpenGL ES 3.2 instead of
OpenGL 4.6 (the renderer's direct state access is emulated in `src/gl/gl46_gles.cpp`), GLSL ES
3.20 instead of 4.60, AAudio instead of WASAPI or ALSA, Stockfish compiled for arm64, HTTPS through
Java. Build the release variant: the debug one compiles the game without optimisation (several
times slower). `docs/ANDROID.md` has the details.

Wine names the Linux files in the character set of the host locale: in the POSIX locale (`LANG`
unset, common in containers) that is ASCII, a file named after "Élodie" cannot be created and the
saved games' tests fail although Windows takes the name. `tools/test_win.sh` and
`tools/shot_win.sh` run Wine in a UTF-8 locale (`LC_ALL=C.UTF-8`) when the current one is not.

Development options: `--scene <name>` runs a viewer scene (`--list-scenes`), `--data-dir .`
reads shaders from disk and **F5** reloads them, **F12** saves a screenshot,
`--shot out.png --frames N --size 1280x720` renders headlessly (leaving the settings file and the
saved games as they are). `tools/shot.sh` and `tools/shot_win.sh` do this under Xvfb (Linux build
and Windows build through Wine). The Windows exe has no console: redirect what it prints
(`Scacelith.exe --list-scenes > scenes.txt`, or `| more`), or configure with
`-DSCACELITH_CONSOLE=ON` for a console build.

The game itself (`--scene game`, the default) takes `--start` (a game against Stockfish at once;
`--human white|black`), `--viewer` (a watched game at once; `--white-preset N --black-preset N`
with N an index of the preset list, `--viewpoint 0..9`), `--tc N` (time control index), `--cam
x,y,z [--look x,y,z] [--fov deg]` (initial observer camera when watching; a detached camera in a
normal game), `--handover-preview` (watching through the players' eyes with the clock frozen during
each camera handover, the same hand-over as [hot-seat](docs/MULTIPLAYER_PLAN.md)), `--no-intro`,
`--warp <seconds>` (with `--shot`: simulate before the first frame), `--moves e2e4,e7e5,...`, `--touch <square>`,
`--mouse fx,fy` (pointer position as fractions of the window; the view follows it, `0.5,0.03` looks
up at the opponent), `--glance` (start looking at the scoresheet), `--calibrate` (the brightness
calibration before the title page, as on a first start) and `--ini <file>`.

Two players on one PC: `--start --hotseat` starts one at once, with `--white-name N`, `--black-name
N`, `--clock-right white|black`, `--rated` and `--handover <seconds>` (0 = instant cut). `--play
e2e4,e7e5,...` plays the human moves by hand, the way a player would (touch, carry, press the
clock): both sides on one PC, the human's side against Stockfish; with `--warp` it takes
screenshots of a game in progress, for instance the hand-over halfway (`--start --hotseat
--no-intro --play e2e4 --warp 2.35`). The UI viewer has the hot-seat screens (`--scene ui
--ui-screen newgame-hotseat|hotseat-hud|hotseat-confirm|hotseat-gameover`).

Coach mode: `--start --coach` starts a coach game at once, at the level and colour of the Coach
page unless `--coach-level 0..6` (0 = the rules lesson) or `--coach-colour white|black` say
otherwise; `--coach-dir <folder>` reads (and downloads) the voice model files in that folder instead
of the default one (`--coach-dir build/coach`: the copy a development build prepares).
`SCACELITH_COACH_SOURCE=github` in the environment skips Hugging Face (to try the fallback), and
`--scene ui --ui-screen coach-flow [--coach-dir <folder>]` runs the download flow over the title
page (`coach-download*` screens: the prompt and the panel with sample figures).
`--coach-stage-test` (alone, or with `--start`) runs a fixed sequence through the scene's coach
stage without the session: a line spoken and subtitled, the coach pointing at g1 and
tracing the knight's jump to f3 on their words, a mark and a highlight, two demonstration moves
taken back by hand, then the takeback card (`--coach-stage-test lesson`: on the lesson's first
position); its log gives the time of each step, for `--warp`. `--coach-auto-answer yes|no` answers
the takeback card by itself after 1.5 s, for runs with `--play`.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the engine is organised.

### Continuous integration and releases

GitHub Actions builds and tests every push to master and every pull request
(`.github/workflows/ci.yml`): the Linux build and its unit tests, the Windows build cross-compiled
with MinGW-w64 and its unit tests under Wine (both as above), and a lint of the workflows
(actionlint and zizmor). The Windows executable of each run is kept for 14 days as a workflow
artifact, for testing. CodeQL (`.github/workflows/codeql.yml`) scans the shipped C++ code
(without `third_party/` and `tests/`) and the workflows; its alerts are in the Security tab.

The version is set in `cmake/version.cmake`: `SCACELITH_VERSION_CORE` (`1.0.0`) and, for a
pre-release, `SCACELITH_VERSION_PRERELEASE` (`beta.2`); `cmake -P cmake/version.cmake` prints it
in full. Pushing the tag `v` + that version (`v1.0.0-beta.2`) publishes a release: the release
workflow (`.github/workflows/release.yml`) checks that the tag matches the version, runs the CI
again on the tagged commit, builds the game from scratch (no compiler cache) for Windows x64 and
for Linux x86-64 (on Ubuntu 22.04, the oldest toolchain supported, where the unit tests run again
and the executable is checked to need only system libraries and glibc 2.34), attests the build
provenance of the files with actions/attest and publishes a GitHub release (a pre-release when the
version has a suffix) with `Scacelith-<version>-windows-x64.zip` (the executable, its licence and
the licence texts of what it embeds), the Windows executable alone,
`Scacelith-<version>-linux-x86_64.tar.gz` (the executable, its launcher and icons, `install.sh`
and the same licences) and `SHA256SUMS`. Started by hand, the
workflow builds the same files and keeps them as workflow artifacts without publishing anything,
unless it runs on a tag with "Publish" ticked. To check a downloaded file with the GitHub CLI:

```sh
gh attestation verify Scacelith-1.0.0-beta.2-windows-x64.zip --repo DarkCenobyte/scacelith-chess
```

Dependabot (`.github/dependabot.yml`) keeps the actions the workflows use up to date (pinned by
commit SHA); the code vendored in `third_party/` is updated by hand.

## Licence

Scacelith is free software under the GNU General Public License v3.0 (see `LICENSE`), because it
embeds [Stockfish](https://stockfishchess.org) (GPL-3.0), whose source is in
`third_party/stockfish/` with its own copyright notices. Stockfish's neural network was trained on
data provided by the Leela Chess Zero project, which is made available under the Open Database
License (ODbL). The Cinzel, EB Garamond and Amiri (Khaled Hosny) interface fonts and the handwriting
fonts Caveat (Impallari Type), Marck Script (Denis Masharov), Bad Script (Gaslight), Aref Ruqaa
(Abdullah Aref, Khaled Hosny), Klee One (Fontworks) and LXGW WenKai / WenKai TC (LXGW) are under the
SIL Open Font License 1.1; the Arabic and CJK fonts (Amiri, Aref Ruqaa, Klee One, LXGW WenKai /
WenKai TC) are subset and renamed from the upstream files by `tools/prepare_fonts.py`, the others
are the upstream files unchanged. The chess figures of the promotion picker come from a subset of
GNU FreeFont FreeSerif (GPL-3.0+ with the font exception). All licence texts are in `assets/fonts/`
and `assets/fonts/hand/`. The coach's voice model (Supertonic 3) is not part of the program nor of
its release package: the game downloads it from its publishers at the player's request. It has its
own licence (BigScience Open RAIL-M), whose use restrictions the download prompt shows; see
`third_party/supertonic3/README.scacelith.md`.

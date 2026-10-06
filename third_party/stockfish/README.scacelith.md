# Stockfish 19 in Scacelith

Scacelith's opponent is [Stockfish](https://stockfishchess.org) 19, compiled into the game
executable and run in-process. Stockfish is free software under the **GNU General Public License
v3.0** (`Copying.txt`, authors in `AUTHORS`), which is why Scacelith as a whole is GPL-3.0.
Its network was trained on data provided by the Leela Chess Zero project, which is made available
under the Open Database License (ODbL).

## Provenance

* Stockfish 19 (release of 2026-09-05): tag `sf_19` (a lightweight tag) of
  <https://github.com/official-stockfish/Stockfish>, commit
  `edb0d9db6731067ec50ce619ff372b463bc4dd5d`, root tree `418af042b3c0aade628c1c98f13942659e67e64d`.
* `src/` is byte-identical to `src/` of that commit (git tree
  `15c9967c8add24a07ef4a34522cb6fb5c71ca580`: `git ls-files -s third_party/stockfish/src` lists the
  same blob ids and modes as `git ls-tree -r sf_19 src` in a Stockfish clone), plus the network
  below. That includes files the game does not build: the upstream `Makefile` and `src/universal/`
  (upstream's own multi-architecture build, see
  [Instruction-set variants](#instruction-set-variants)) are kept so that the whole tree can be
  checked against the tag; the generated `src/.depend` is not part of it.
* `src/nn-1a298aa575a0.nnue` is Stockfish 19's default (and only) network, which upstream does not
  keep in git (`make net` downloads it): 98,511,183 bytes, sha256
  `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2`, whose first 12 digits are its
  name, as upstream's `scripts/net.sh` checks.
* `Copying.txt` and `AUTHORS` come from the same commit.

## Modifications

**No upstream file is modified.** Everything Scacelith-specific lives next to it:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Static library `stockfish_embedded`: all upstream sources except `src/main.cpp` and `src/universal/` (upstream's own multi-architecture build), once per instruction-set variant |
| `cmake/isolate.cmake` | Makes each variant one object with three global symbols, and verifies it |
| `cmake/isolate_llvm.cmake` | The same for an LLVM toolchain (no GNU binutils): the Android build's isolation |
| `scacelith/entry.cpp` | `scacelith_sf_main_<tag>()` (one per variant, e.g. `_x86_64_avx2`): the body of upstream `main()` as a function, with a fixed command line |
| `scacelith/cpu.cpp` | The dispatcher: chooses the variant for the CPU, runs its static initialisers, calls its entry point |
| `scacelith/cpu_arm64.cpp` | The dispatcher of the Android (arm64) build: armv8 and armv8-dotprod, `getauxval(AT_HWCAP)` (below) |
| `scacelith/nnue_incbin.cpp` | The network, embedded once for all variants |
| `scacelith/local_shm.h` | Replaces upstream `src/shm.h`: the network stays in process memory (forced include) |
| `scacelith/win_shims.h` | Windows: redirects `GetNumaProcessorNodeEx` (Wine) and the console code page calls (forced include) |
| `scacelith/win_shims.cpp` | Windows: the `GetNumaProcessorNodeEx` wrapper |
| `scacelith/stockfish_embedded.h` | The only header the game sees |
| `tools/isa_audit.py`, `tools/isa_probe.cpp` | Build check: disassembles a linked program and verifies the variants' isolation |

The shims are headers that CMake force-includes (`-include`) before every Stockfish source file,
so upstream code is compiled as is but sees them first:

* **`local_shm.h`**, all platforms. Upstream's `SystemWideSharedConstant` (`shm.h`) first tries to
  share the unpacked network (115 MB) with every other process running the same executable, and
  no option turns that off: on Linux a directory `/tmp/stockfish-<uid>/sfshm_<hash>/` with a lock
  file and a Unix socket (left behind when the process is killed), a `memfd`, a socket-server
  thread and an `atexit` handler; on Windows a named file mapping and mutex (`Local\sf_...`). It
  only falls back to a private allocation when that fails. The shim defines `shm.h`'s include
  guard and the same public API on top of that private allocation (upstream's own fallback path,
  `make_unique_large_page`), so the engine creates no file, socket, thread or named object.
  Scacelith runs one engine per process, so there is nothing to share. If upstream renames the
  guard or changes the class, both definitions meet and the build fails.
* **`win_shims.h`**, Windows only, with `win_shims.cpp`:
  * `GetNumaProcessorNodeEx` becomes `scacelith_GetNumaProcessorNodeEx`. Wine, and so Proton,
    only stubs that function (`ERROR_CALL_NOT_IMPLEMENTED`, Wine 9.0 to 11.0); upstream then
    builds an empty NUMA configuration while it still finds the L3 caches, and
    `NumaConfig::try_get_l3_aware_config()` calls `std::map::at` on the empty map, which
    terminates the game (`std::terminate`, no exceptions) as soon as the engine starts. The wrapper
    calls the real function and, only when it is not implemented, reports node 0 for every
    existing processor. Windows implements it, so there the wrapper changes nothing.
  * `SetConsoleCP` / `SetConsoleOutputCP` do nothing. Upstream calls them (`set_console_utf8()`) at
    every session start; in a console build (`SCACELITH_CONSOLE`) they would switch the code page
    of the console the game was started from, which outlives the game.

  A macro only redirects direct calls: if upstream ever reached these functions another way (for
  example through `GetProcAddress`), the Wine crash would come back. Hence the Wine check below at
  every Stockfish update.

## Build

* The Stockfish sources are compiled once per instruction-set variant (next section), each with
  the flags of the upstream Makefile for that `ARCH` with GCC: `-O3 -funroll-loops -fno-exceptions
  -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000 -DNDEBUG -DIS_64BIT -DARCH=<arch>` plus the
  variant's instruction set (`SF_ISA_<arch>` in `CMakeLists.txt`, copied from the Makefile). No
  LTO: upstream's LTO builds measured no faster than these, and a partially linked LTO object
  cannot be isolated. Search threads
  are pthreads with 8 MB stacks with Linux GCC and MinGW-w64 alike (`thread_native.h`). The game's
  global Windows definitions (`UNICODE`, `WIN32_LEAN_AND_MEAN`, ...) are not applied. On Windows
  the library links `shell32` (`CommandLineToArgvW`).
* Each variant also gets `-DStockfish=Stockfish_<tag>` (its own namespace, e.g.
  `Stockfish_x86_64_avx2`), `-DUNIVERSAL_BINARY` (upstream's switch for multi-architecture builds:
  `nnue/network.cpp` then only declares the embedded network) and, on Linux, `-fno-gnu-unique`
  (see isolation below). The UCI command `compiler` reports the variant's `ARCH`.
* The network is embedded once, by `scacelith/nnue_incbin.cpp`, with Stockfish's own
  `INCBIN(EmbeddedNNUE, EvalFileDefaultName)` (an assembler `.incbin`, works with Linux GCC and
  MinGW-w64 GCC), which is what `nnue/network.cpp` does in a single-architecture build. The
  assembler resolves the network's relative path itself, so that file gets `-Wa,-I<this>/src`, and
  it depends on the `.nnue` file for rebuilds.
* The dispatcher, the network and the Win32 wrapper are compiled once with the default x86-64
  flags and without the forced includes, so they run on any x86-64 CPU.
* `SCACELITH_SF_VARIANTS` (a CMake cache list) selects the variants; the default is all five. Each
  is a full Stockfish compile: `-DSCACELITH_SF_VARIANTS=x86-64-sse41-popcnt` (or whichever variant
  the developer's CPU runs best) makes quicker local builds. The dispatcher only knows the variants
  that were built; without `x86-64` a CPU below every built variant cannot run the engine, and the
  game logs it and plays random moves as before.
* Clean build of the Stockfish part (`ninja -j2 stockfish_embedded`, no ccache, on the shared
  4-core build machine): Linux 6 min 13 s wall, 4 min 58 s user and 26 s system CPU; MinGW 11 min
  53 s wall, 9 min 16 s user and 53 s system CPU. The machine was busy (load average 6 to 10), so
  the wall times are long; the user CPU time is five to six times that of the single SSE4.1 build
  Stockfish 19 was first embedded with (Linux 52-56 s, MinGW 92-105 s). The audit below adds 7 s
  (Linux) and 10 s (MinGW).

## Instruction-set variants

Stockfish 19 searches about half as many nodes per second as Stockfish 16 with the same
instruction set (SSE4.1, below; its network is 2.5 times larger), and newer instructions recover
part of that. The library therefore holds five builds of the engine and runs the best one the CPU
supports:

| Variant | CPUs | Checked at run time | `bench` nodes/s, 1 thread |
|---|---|---|---:|
| `x86-64` | every x86-64 CPU (Core 2, Phenom II, some virtual machines) | nothing: SSE2 is part of x86-64 | 357 k (-37 %) |
| `x86-64-sse41-popcnt` | Nehalem to Ivy Bridge, AMD Bulldozer to Steamroller, Jaguar | SSE3, SSSE3, SSE4.1, POPCNT | 563 k |
| `x86-64-avx2` | Haswell to Comet Lake, Zen 1 to 3 | everything `x86-64-sse41-popcnt` checks, plus AVX2 (and OS support), BMI1 | 658 k (+17 %) |
| `x86-64-avxvnni` | Alder Lake and later Intel client CPUs | everything `x86-64-avx2` checks, plus BMI2, AVX-VNNI | 770 k (+37 %) |
| `x86-64-avx512icl` | Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids and later, Zen 4 and 5 | everything `x86-64-avx2` checks, plus BMI2, AVX-512 F, BW, VL, DQ, CD, VNNI, IFMA, VBMI, VBMI2, VPOPCNTDQ, BITALG, VPCLMULQDQ, GFNI, VAES (and OS support); not AVX-VNNI, which Ice Lake, Tiger Lake, Rocket Lake and Zen 4 lack | 833 k (+48 %) |

The speeds are `bench 16 1 13` (upstream's benchmark positions, 16 MB hash, 1 thread, depth 13) run
by the game's library with the variant forced, on the build machine's Intel CPU (AVX-512 and
AVX-VNNI): the median of 4 interleaved rounds, CPU time without the start-up, on a busy machine
(load 5 to 7, hence +-10 %). Every variant searches the same 2,497,913 nodes. Stockfish 16's former
game build did 1,138 k with SSE4.1 on the same machine (separate run, same method). Upstream's
other x86 variants were left out: `bmi2` measured the same as `avx2`, and `avx512` / `vnni512`
(Skylake-X, Cascade Lake: workstations and servers) fall back to `avx2`, which also avoids their
AVX-512 clock penalty. Adding a variant takes its name, flags and audit level in `CMakeLists.txt`
and its level and requirements in `cpu.cpp`.

**Dispatch** (`scacelith/cpu.cpp`). The rules follow upstream's `universal/entry_x86.cpp`,
restricted to these variants, and check every extension a variant's flags enable, which upstream
does not quite do: SSE3 and SSSE3 for `sse41-popcnt`, BMI1 for `avx2` (`-mbmi`), AVX512DQ and
AVX512CD for `avx512icl`. Upstream's slow-PDEP rule (AMD Excavator to Zen 2) only concerns its
`bmi2` variant: those CPUs get `avx2` here, which does not use PDEP/PEXT. The CPU features come
from libgcc's CPU model (`__builtin_cpu_supports`), which also checks the OS: it reports AVX2 only
when XCR0 shows that the OS saves the YMM registers, and the AVX-512 features only when it also
saves the opmask and ZMM registers (`and $0x6` / `and $0xe6` in `__cpu_indicator_init`); every VEX
variant requires AVX2 and the EVEX one AVX-512. The variants are not a strict chain (a CPU can
have AVX-512 but not AVX-VNNI), so each one is checked on its own and the highest built one the
CPU runs wins.

* **`engine.arch`** in `Scacelith.ini` (default `auto`), for troubleshooting: a variant name caps
  the choice at that variant, never above what the CPU runs; an unknown name is logged and
  ignored. It is applied at the next engine start.
* **Log.** Every engine start writes the variant, e.g. `ai: starting embedded Stockfish
  (x86-64-avx512icl)`, or `ai: starting embedded Stockfish (x86-64-sse41-popcnt, limited by
  engine.arch = x86-64-sse41-popcnt; this CPU runs x86-64-avx512icl)`.
* **Static initialisers.** A variant's static initialisers may use its instructions, so they
  cannot run at program start. `stockfish_embedded_supported()` runs those of the variant it
  chooses, once per process (`std::call_once`), in the order the C runtime would have used
  (`.init_array` forwards on Linux, `.ctors` backwards on MinGW). They register the variant's
  static destructors with `atexit`; the host registers its own exit handler (which stops a running
  engine) after them, once per variant, so it runs first.

**Isolation** (`cmake/isolate.cmake`). Every variant compiles the same inline functions and C++
standard library templates (`std::vector<...>`, `std::string`, ...) with its own instruction set.
Left alone, those copies are weak (COMDAT) symbols that the linker merges program-wide: the game,
or the baseline variant, could then call an AVX2 copy on a CPU without AVX2 and crash. This is why
upstream's `x86-64-universal` build is not used: its variants are only renamed, not isolated, and
it is only safe with `--allow-multiple-definition` and a favourable link order (its MinGW cross
build fails with multiple definitions). Each variant is instead turned into one object:

1. `ld -r` links its objects into one relocatable object, which resolves its COMDAT groups inside
   the variant (ELF: `--force-group-allocation`, no group left; PE: twice, as one pass leaves an
   undefined symbol next to each kept COMDAT definition). PE: the unwind table entries go to the
   section `.pdata$<tag>`, not `.pdata`, as `ld -r` sorts a `.pdata` by its raw contents (offsets
   into each input's own sections) without moving the relocations: the executables would get
   overlapping entries and bad unwind data, also over libgcc's unwinder, and a C++ throw could
   loop forever or unwind into a wrong frame. The final link takes `.pdata*` into `.pdata` and
   sorts the relocated entries.
2. PE only: the COMDAT flag is cleared on every `.text$*`, `.rdata$*`, `.data$*`, `.bss$*`,
   `.xdata$*` and `.pdata$*` section, because COFF merges COMDATs by name even when their symbol
   is local.
3. The initialiser table (`.init_array` or `.ctors`) becomes the section `sfinit_<tag>` between the
   symbols `sfinit_<tag>_start` and `sfinit_<tag>_end`, so the C runtime no longer runs it.
4. `objcopy` makes every defined symbol local except the entry point `scacelith_sf_main_<tag>` and
   the two bounds. Undefined references (libstdc++, the C runtime, pthreads, Win32) stay external
   and bind to the single copies in the executable, so the engine still uses the game's
   `std::cin` / `std::cout`. On Linux `-fno-gnu-unique` is needed first: GCC otherwise emits the
   guard variables of inline statics as `STB_GNU_UNIQUE` symbols, which `objcopy` cannot make
   local.

The script then verifies its output and fails the build if anything could leak or go missing:
exactly those three global symbols, bounds that span the whole initialiser table, no symbol both
defined and undefined, no COMDAT left, on Windows no `.pdata` left for `ld -r` to sort, and no
section the scheme does not handle (initialiser priorities, destructor tables, thread-local
storage; Stockfish 19 has none). The exception table of each Windows executable is then checked
after its link (`tools/check_pdata.py` at the repository root).

**Verification.**

* **Audit at every build** (`tools/isa_audit.py`, target `stockfish_isa_audit`, Linux and MinGW
  builds when Python 3 is found). A small program (`tools/isa_probe.cpp`) links the library the way
  the game does, next to ordinary code that instantiates the same standard library templates. The
  script disassembles it, attributes every function to its object file with the link map, and
  classifies every instruction (baseline, SSE3 to SSE4.2 and POPCNT, VEX, EVEX). The build fails if
  a function outside the variants uses anything above baseline, if a variant holds code above its
  own level, or if anything but the dispatcher refers to a variant's code (calls, jumps,
  RIP-relative operands, and pointers in data from the relocations). Result: no finding in the
  Linux and MinGW builds. Run by hand on the game executables relinked with a map (Linux: 7,944
  functions, 292,899 references; Windows: 11,819 functions, 330,384 references), it finds nothing
  either; it does fail when given a variant level that is too low or no exemption for the
  dispatcher.
* **Same play in every variant** (`ai_variants_play_identically` in `tests/ai_tests.cpp`): each
  variant the CPU runs, forced through the arch limit, searches four positions to depth 11 with
  identical node counts and moves. Stockfish's search is deterministic with one thread, so any
  difference would reveal a miscompiled variant.
* **Selection on other CPUs** (by hand, at every Stockfish update). `qemu-x86_64 -cpu <model>`
  runs the Linux build, and `WINELOADERNOEXEC=1 qemu-x86_64 -cpu <model> /usr/lib/wine/wine64` the
  Windows build, on an emulated CPU: qemu64 and Penryn choose `x86-64`, Nehalem
  `x86-64-sse41-popcnt`, Haswell and EPYC-Rome `x86-64-avx2`, and Icelake-Server `x86-64-avx2` as
  well (qemu does not emulate AVX-512), all with the same `bench` node counts. The Windows AI tests
  pass on Penryn and Nehalem, and `ai_variants_play_identically` on Haswell. The build machine's
  CPU gets `x86-64-avx512icl`, natively and under Wine, and runs all five variants.

## Running in-process

`src/ai/uci_host.cpp` redirects `std::cin` / `std::cout` to thread-safe in-memory stream buffers
(a blocking line queue for input, a line splitter for output), calls
`stockfish_embedded_supported()` (which chooses the variant) and runs `stockfish_embedded_main()`
on a `std::thread`. Nothing else in the game may use `std::cout` / `std::cin` (see
`docs/ARCHITECTURE.md`). `src/ai/engine.cpp` is the UCI client.

Checked properties:

* **Embedded network only, no file access.** `EvalFile` defaults to `nn-1a298aa575a0.nnue`, which
  the engine loads from its embedded copy first; the binary and working directories are only tried
  if that failed. `strace -f` of engine sessions (Linux) shows only reads of
  `/sys/devices/system/{cpu,node}` (CPU and NUMA topology): no `.nnue` file, no `/tmp` directory,
  no `memfd`, no socket.
* **Fixed command line.** The entry function passes `argc = 1`, so the UCI loop reads `std::cin`
  until `quit`. On Windows upstream's `CommandLine` ignores the arguments it is given and re-reads
  the process command line (`GetCommandLineW`): the game's own arguments (`--start`, ...) would
  become a one-shot UCI command and the engine would never answer. The entry function sets them
  back after construction.
* **Sessions.** `quit` (or end of input) makes the UCI loop return; destroying the `UCIEngine`
  then joins the search threads and frees the hash table and the network. A new session can be
  started after `quit` (tested repeatedly on Linux and under Wine), in the same variant or
  another. Only the attack and Zobrist tables are process-wide (filled again, with the same
  values, by every session), plus a few static objects that hold no game state (the processor
  affinity read by the initialisers, the output mutex, ...). Only one session can run at a time
  (the standard streams are shared).
* **Restart cost.** Every session parses the network again: `uci` + `isready` until `readyok`
  takes 0.38-0.55 s on Linux and 0.45-0.7 s under Wine on the build machine with the
  `x86-64-avx512icl` variant, up to 1.3 s for the first session of a process (the network's pages
  of the executable are read) and up to 2.7 s when the machine is overloaded (load average 9 on 4
  cores). The former single `x86-64-sse41-popcnt` build took 0.45-1.25 s on Linux and 0.5-0.75 s
  under Wine; Stockfish 16, whose network stayed in globals, 0.4-0.75 s for the first session and
  40-110 ms for later ones. The game starts the engine once per process, asynchronously.
* **Illegal input ends the process.** Stockfish 19 calls `std::exit(1)` on a `position` command
  with an illegal move or FEN, a malformed `go` argument or a failed `flip`. That exit would run on
  the engine thread, so the host's `atexit` handler (below) would wait for itself and the game
  would vanish without a message. `ai::Engine` therefore replays every move list with the game's
  own rules (`chess::Position::parseUCI`) before sending it; a list that fails is never sent and
  the request fails (empty move, neutral evaluation, failed analysis), which the game already
  handles. A start FEN (tutorial positions, analyses) is parsed by `chess::Position::setFEN`,
  checked for Stockfish's own limit on promoted pieces, and sent as the game re-emits it, with the
  move counters within Stockfish's range; analysis search moves are checked in the position
  (Stockfish would silently search every move instead). It only sends numbers it formats itself in
  `go`, and never `flip`. The tests play every kind of special move through the engine (both
  castlings of each side, en passant, the four promotions) and check that illegal lines and
  invalid FENs are refused.
* **Process exit without shutdown.** The host registers an `atexit` handler that sends `quit` and
  joins, after the initialisers of each variant it starts (so it runs before the destructors of
  that variant's static objects). The wait is bounded (2 s): on Windows `exit()` holds the CRT's
  atexit lock while running handlers, and an engine thread that is still initialising can block on
  it (registering the destructor of a function-local static such as the `sync_cout` mutex); the
  thread is then left parked until the process ends. Found and verified with the Windows build
  under Wine.
* **Threads, hash, NUMA.** `engine.threads` and `engine.hash_mb` (`Scacelith.ini`) reach `Threads`
  and `Hash` as before. With more than one thread the client first sends `NumaPolicy none` (one
  node with every CPU): on a machine with several NUMA nodes (for Stockfish 19, also several
  groups of L3 caches on large CPUs) Stockfish would otherwise bind its threads to nodes and copy
  the network to each. With one thread (the default) it does neither, and the option is not sent:
  it would cost a copy of the network per session (0.16-0.19 s and 50 MB more peak memory).
* **Wine and Proton.** The Windows build runs under Wine (unit tests and `tools/shot_win.sh` on a
  game against Stockfish) thanks to `win_shims.h`. Check it again at every Stockfish update.
* Remaining `exit()` calls in Stockfish are failure paths only: allocation failures (hash table,
  network), a network that fails to load (impossible for the embedded default; the game never sets
  `EvalFile`), thread creation failure, NUMA binding failures (only with bound threads, never with
  `NumaPolicy none`), corrupt Syzygy files (the game never sets `SyzygyPath`), debug log and
  benchmark file errors (never used).

Memory (Linux, in-process, 1 thread, 64 MB hash): 266 MB resident once the engine is ready, of
which 101 MB are the embedded network's pages of the executable image and 115 MB its unpacked copy;
315 MB while searching; 327 MB at the peak of the network load. After `quit` 102 MB remain, all of
them pages of the executable image that the OS can drop (Stockfish 16: about 150 MB in all). The
variants the CPU does not run cost no memory: these figures are the same as with the single
`x86-64-sse41-popcnt` build.

Executable size: the Windows build is 129.2 MB, 63.0 MB more than with Stockfish 16. The network
grew from 40.1 to 98.5 MB and the code by 4.6 MB, of which 3.95 MB for the four variants added to
the former single one (about 0.8 MB of code each; Linux: +3.6 MB). Each variant also has 0.3 MB
(variants with BMI2) or 1.2 MB of zero-initialised tables, only touched in the variant that runs.

## Strength presets

See `src/ai/presets.cpp`. Stockfish's handicap (`Skill Level`, or `UCI_LimitStrength` +
`UCI_Elo` 1320..3190, which maps onto a fractional skill level) searches at least 4 principal
variations and, after iteration `1 + int(level)` (or after the last one, if a depth cap stops the
search earlier), picks one of them at random, biased towards the better ones. `UCI_Elo 1320 ==
Skill Level 0` is its floor. Stockfish 19 kept Stockfish 16's Elo-to-level fit and its pick, with
one difference that matters here: the random term is scaled by the spread of the candidates'
scores capped at `PawnValue` (208 internal units, 0.6-0.7 pawn) instead of Stockfish 16's
`PawnValueMg` (126, about 0.4 pawn), so every handicapped setting is noisier than before.
Stockfish 16.1 also removed the classical evaluation (`Use NNUE`), which the weakest preset used.

### Calibration of the weak presets

Stockfish offers nothing below `UCI_Elo 1320` (= Skill Level 0), so the three presets under it
combine Stockfish's own knobs. They were first calibrated with Stockfish 16 self-play (the labels'
scale, below); for Stockfish 19 each new setting was matched directly against the Stockfish 16
preset it replaces (target 0 Elo), so the presets keep the strengths their labels were chosen for.
All matches use standalone `x86-64-sse41-popcnt` builds of the two tags and `tools/sf_match.py`
(which documents the protocol): games from the start position, colours alternating, Threads 1 and
Hash 64 (as in the game), python-chess applies the rules, and a Stockfish 19 referee adjudicates
games still running at 300 plies with a depth-12 evaluation (|eval| > 3 pawns = win). Results are
Elo(A) - Elo(B) with 95% intervals; "S0"/"S1" = Skill Level 0/1, "d1" = `go depth 1`, "MPV" =
MultiPV.

Stockfish 19 against Stockfish 16:

| A (Stockfish 19) | B (Stockfish 16) | games | Elo A-B |
|---|---|---:|---:|
| S0 d1 | Casual: S0 d1 | 400 | -53 [-88, -20] |
| S0 d1, MPV 6 | Beginner: S0 d1, MPV 6 | 400 | -109 [-146, -76] |
| S0 d1, MPV 9 | Novice: S0 d1, MPV 7, classical eval | 1000 | -2 [-23, +19] |
| S0 d1, MPV 5 | Beginner | 1000 | -31 [-53, -10] |
| S1 d1, MPV 5 / S2 d1, MPV 5 | Beginner | 1000 each | +63 [+41, +84] / +113 [+91, +136] |
| S1 d2, MPV 7 | Beginner | 1000 | +41 [+20, +63] |
| S1 d1, MPV 6 | Beginner | 2 x 1000 | -6 [-21, +8] (-6, -7) |
| S1 d2, MPV 5 | Casual | 1000 | +13 [-8, +34] |
| S0, movetime 250 ms | S0, movetime 250 ms | 160 | -120 [-180, -65] |
| UCI_Elo 1500 / 1800 / 2100, movetime 250 ms | the same | 420 / 300 / 300 | -22 [-55, +11] / +12 [-27, +50] / +22 [-16, +61] |

Stockfish 19 alone:

| A | B | games | Elo A-B |
|---|---|---:|---:|
| S0, movetime 30 ms / 250 ms | S0 d1 | 400 / 200 | +116 [+82, +153] / +102 [+54, +154] |
| S1, movetime 30 ms | S1 d2 | 400 | +85 [+51, +121] |
| S1 d2 | S1 d1 | 600 | +97 [+69, +126] |
| S0 d1, MPV 5 / 6 / 7 | S0 d1 | 400 each | -85 / -152 / -209 |
| S0 d1, MPV 8 / 9 / 10 / 12 / 14 / 16 / 20 | S0 d1 | 400 each | -310 / -315 / -317 / -389 / -405 / -407 / -449 |
| S0 d1, MPV 8 / 9 / 10 / 12 / 14 / 16 / 20 | S0 d1, MPV 7 | 400 each | -63 / -70 / -99 / -142 / -167 / -212 / -245 |
| Novice (S0 d1, MPV 9) | Beginner (S1 d1, MPV 6) | 600 | -186 [-219, -157] |
| Beginner | Casual (S1 d2, MPV 5) | 600 | -217 [-251, -185] |
| Casual | Club Player (UCI_Elo 1500, movetime 100 ms) | 400 | -298 [-349, -255] |

Findings:

* **Stockfish 16's settings are weaker with Stockfish 19**, because of the larger random term:
  S0 d1 loses 53 against its Stockfish 16 self, S0 d1 MPV 6 loses 109. The old settings could not
  simply be kept.
* **Depth caps weaken the handicap, but indirectly.** The level picks after iteration
  `1 + int(level)`, yet with a normal search budget the deeper iterations (of this and earlier
  moves) fill the hash table that the shallow multi-PV scores then use: capping the search at the
  pick iteration costs ~105 (Stockfish 16: ~156), the same at 30 and 250 ms. A cap one ply short of
  the pick iteration (Skill Level 1 at depth 1) makes the pick after the last iteration, on
  shallower scores: another ~100. So the UCI_Elo presets are never depth-capped (that would
  silently weaken them), and the weak ones use `depth 1` or `depth 2` on purpose. A node cap is no
  alternative: one that interrupts an iteration leaves unsearched root moves and yields arbitrary,
  not human-like, moves.
* **The level still counts at a capped depth**: the pick keeps (8 + 2 x level) / 128 of the score
  differences against its random term (1/16 at level 0, 1/13 at level 1), so Skill Level 1 at
  depth 1 is ~95 stronger than Skill Level 0 at depth 1 with the same MultiPV. That is what makes
  the Beginner possible: at level 0 it would fall between MultiPV 4 (the same as 1 under the
  handicap, 50-100 stronger than the Stockfish 16 Beginner) and MultiPV 5 (-31).
* **MultiPV** widens the list the random pick is made from: 5 costs ~85, 6 ~150, 7 ~230 (about as
  with Stockfish 16), and it no longer saturates near 10: 9 ~300, 10 ~330, 14 ~410, 20 ~470
  (weighted least squares over all the MultiPV pairings, +-20-40). `estimateElo` uses these
  values, interpolated in between.
* **Stockfish 19's lowest levels are weaker than Stockfish 16's at the same level; the higher
  ones are not.** Against its Stockfish 16 self at 250 ms per move, Skill Level 0 scores -120
  (about 1200 on the labels' scale), UCI_Elo 1500 -22, UCI_Elo 1800 and 2100 +12 and +22
  (+-35-40). So the UCI_Elo presets keep `UCI_Elo` = label (Stockfish's calibration). For
  depth-capped settings at level 0-1, `estimateElo` subtracts 185 instead of the ~105 the cap
  itself costs, so that its estimates stay on the labels' scale; an uncapped custom setting at
  level 0 or 1 is rated ~65-120 too high.
* The game plays the UCI_Elo presets at 1 s per move untimed, or on the clock; 250 ms stands for
  that because the pick iteration does not depend on the budget and the hash-table effect is
  already saturated (above). Stockfish 19 searched ~37 % fewer nodes per move than Stockfish 16 in
  these games, so a longer budget can only favour it relatively. Those matches depend on the
  machine's load (a shared build machine, load average 2-12); splits by load and colour showed no
  systematic effect.
* Chains of these measurements are not perfectly transitive (the players are very random), so
  derived numbers are good to roughly +-50; the presets' own strengths rest on the direct matches
  against the Stockfish 16 presets (+-15-21).

#### Stockfish 16 (the labels' scale)

Stockfish 16 self-play (a standalone build of its sources; 200-600 games per pairing, colours
alternating, adjudicated at 300 plies by a depth-12 referee as above):

| A | B | games | Elo A-B |
|---|---|---:|---:|
| S0, movetime 30 ms | S0 d1 | 200 | +156 [+106, +212] |
| UCI_Elo 1500, movetime 30 ms | S0, movetime 30 ms | 200 | +109 [+61, +163] |
| S0 d1, MPV 5 | S0 d1 | 300 | -72 [-112, -33] |
| S0 d1, MPV 6 | S0 d1 | 300 + 600 | -111, -167 |
| S0 d1, MPV 7 | S0 d1 | 300 + 600 | -235, -216 |
| S0 d1, MPV 8 / 9 / 10 / 12 | S0 d1, MPV 7 | 300 each | -21 / -64 / -99 / -101 |
| S0 d1, MPV 10 | S0 d1 | 600 | -304 [-345, -268] |
| S0 d1, MPV 10 | S0 d1, MPV 6 | 600 | -135 [-165, -106] |
| S0 d1, classical eval (`Use NNUE` false) | S0 d1 | 600 | -207 [-241, -176] |
| S0 d1, MPV 6, classical | S0 d1, MPV 6 | 2 x 600 | -179, -189 |
| S0 d1, MPV 7, classical | S0 d1, MPV 7 | 600 | -192 [-226, -162] |
| S0 d1, MPV 7, classical | S0 d1, MPV 6 | 600 | -179 [-212, -150] |
| S0, movetime 100 ms | S0, movetime 30 ms | 150 | +40 [-15, +96] |

With Stockfish 16's Skill Level 0 at normal time as 1320 (Stockfish's anchor), these gave its
presets their estimates: Novice (S0 d1, MPV 7, classical eval) 750-840, Beginner (S0 d1, MPV 6)
~1015, Casual (S0 d1) ~1165. The hash-table effect is mostly saturated at 30 ms per move (+40 at
100 ms), so taking "S0 at 30 ms" as Stockfish's 1320 may put them up to ~40 Elo too high.

#### Resulting presets

| Preset | Settings | Estimate |
|---|---|---|
| Novice (~800) | Skill 0, depth 1, MultiPV 9 | 750-840 |
| Beginner (~1000) | Skill 1, depth 1, MultiPV 6 | ~1010 |
| Casual (~1200) | Skill 1, depth 2, MultiPV 5 | ~1175 |
| Club Player ... Grandmaster | UCI_Elo 1500 / 1800 / 2100 / 2400 / 2700 | Stockfish's calibration (as with Stockfish 16 within ~40, measured from 1500 to 2100) |
| Stockfish Max | full strength | ~3500 (1 thread) |

These are engine ratings on Stockfish's CCRL-anchored scale; human (FIDE or online) ratings are
not the same scale, so the labels are approximate by nature.

## Android (arm64-v8a)

The Android port (`docs/ANDROID.md`) embeds the same engine with one change of shape, built by the
Android build's own CMakeLists (`android/app/src/main/cpp/CMakeLists.txt`), not by this one:

* **Two variants, `armv8` and `armv8-dotprod`** (upstream's `ARCH=armv8` and `ARCH=armv8-dotprod`:
  baseline AArch64 with NEON, and `-march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD` on top). NEON and
  POPCNT are what the arm64-v8a ABI guarantees; FEAT_DOTPROD is not, and the linker would merge
  the standard-library copies both variants compile — so the desktop's machinery carries over:
  each variant is isolated by `cmake/isolate_llvm.cmake`, the LLVM-toolchain counterpart of
  `cmake/isolate.cmake` (the NDK has lld and llvm-objcopy, no GNU binutils), and
  `cmake/isa_check_arm64.cmake` checks the result where the desktop audits with
  `tools/isa_audit.py` (no `sdot`/`udot` in the baseline, at least one in the dotprod variant).
* **Clang's two flag adaptations** (the NDK toolchain is clang): `-fconstexpr-steps=500000000`
  instead of GCC's `-fconstexpr-ops-limit`, and no `-fno-ipa-cp-clone`, which clang does not know.
* **The dispatcher is `scacelith/cpu_arm64.cpp`**, the counterpart of `scacelith/cpu.cpp`: a
  variant table, the CPU check `getauxval(AT_HWCAP) & HWCAP_ASIMDDP` (the bit upstream's own
  `universal/entry_arm64.cpp` tests), `std::call_once` running the chosen variant's initialisers
  from its `sfinit_<tag>` table, and `engine.arch` accepting `auto`, `armv8` or `armv8-dotprod`
  (an x86 name left over in a copied `Scacelith.ini` is refused and logged by the caller, never
  fatal). The initialisers are not run at library load: the isolation moves them out of
  `.init_array`, and the dispatcher runs those of the chosen variant, exactly as `cpu.cpp` does.
* Everything else is byte-identical to this build: the upstream sources, `entry.cpp`,
  `local_shm.h` (the network stays in process memory), `nnue_incbin.cpp` — whose `.incbin` is
  resolved by clang's integrated assembler (`-Wa,-I<this>/src`; the NDK ships no binutils) — and
  the game side (`src/ai/uci_host.cpp`).
* Verified at build level only: the `.so` of both APKs holds the 98,511,183 network bytes at
  `gEmbeddedNNUEData` (hashing to the network's sha256), both entry points (`scacelith_sf_main_armv8`,
  `scacelith_sf_main_armv8_dotprod`), the dotprod code (184 `sdot`/`udot` in the release build),
  the two `sfinit` tables with their run-time relocations, and no variant symbol beyond the three
  each isolation keeps global. No
  engine move has been searched on arm64: no device was available, so the arm64 node rate, the
  dotprod dispatch on real hardware and the
  presets' Elo mapping there are unmeasured.

// Scacelith glue for the embedded Stockfish 19 (GPL-3.0). This is the only header the game sees;
// Stockfish's own headers stay private to the stockfish_embedded library.
//
// The library holds several builds of the engine ("variants", one per instruction set: on x86-64
// from the baseline up to AVX-512, on arm64 armv8 and armv8-dotprod; CMakeLists.txt,
// SCACELITH_SF_VARIANTS) and a dispatcher that chooses the best one this CPU runs. A session is started by calling stockfish_embedded_supported()
// and then stockfish_embedded_main() on a dedicated thread after std::cin / std::cout have been
// redirected to in-memory stream buffers (see src/ai/uci_host.cpp); it returns after the "quit"
// command. The engine's state (options, thread pool, hash table, network) belongs to the session
// and is freed when it ends, so sessions can be run again sequentially (start, quit, start, ...),
// each one loading the network again. Only one session may run at a time: they share the
// process's std::cin / std::cout and a few tables. The functions below are called from one thread
// (the client's); stockfish_embedded_supported() never while a session runs.
#pragma once

// Variants built into this executable, from the baseline up, by Stockfish ARCH name (e.g.
// "x86-64-avx2"): index 0 .. stockfish_embedded_variant_count() - 1; nullptr out of range.
int stockfish_embedded_variant_count();
const char* stockfish_embedded_variant(int index);

// Caps the variant chosen by the next stockfish_embedded_supported(), for troubleshooting: "auto"
// (or empty, the default) chooses the best variant this CPU runs; a variant name, built or not
// (x86-64, x86-64-sse41-popcnt, x86-64-avx2, x86-64-avxvnni or x86-64-avx512icl; or the Android
// build's armv8 and armv8-dotprod), chooses the best one at or below it. Never raises the choice above what the CPU runs; a running session keeps its
// variant. Returns false, leaving the cap unchanged, for an unknown name.
bool stockfish_embedded_limit_arch(const char* arch);

// Chooses the variant for the next session: the best built one this CPU and OS run, within the
// cap. The first time a variant is chosen in the process, this also runs its static initialisers
// (they run at no other time: they may use the variant's instructions), which register the
// variant's static destructors with atexit: an atexit handler that stops a running session must
// be registered after this call, so it runs before them. Returns false when no built variant fits
// (possible only when the build or the cap leaves out the baseline variant): the engine cannot
// run.
bool stockfish_embedded_supported();

// ARCH name of the variant chosen by the last stockfish_embedded_supported(), "none" before or
// when none fits.
const char* stockfish_embedded_arch();

// ARCH name of the best built variant this CPU runs, whatever the cap, or "none".
const char* stockfish_embedded_best_arch();

// Stockfish 19's main() without the banner, in the chosen variant: initialises the engine (UCI
// options, tables, thread pool, embedded NNUE network), runs the UCI loop on std::cin / std::cout
// until "quit" (or end of input) and frees it all. Returns 0 (1 without a successful
// stockfish_embedded_supported()). Stockfish ends the whole process (std::exit(1)) when a
// "position" command holds an illegal move or FEN, or "go" a malformed number: the client must
// only send what it has checked (src/ai/engine.cpp).
int stockfish_embedded_main();

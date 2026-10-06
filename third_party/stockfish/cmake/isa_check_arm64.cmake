# Scacelith build script (not part of upstream Stockfish): the arm64 counterpart of the desktop's
# tools/isa_audit.py, for one isolated Stockfish variant object. Run as
#   cmake -DOBJ=<sf_<tag>.iso.o> -DLEVEL=<0|1> -DOBJDUMP=llvm-objdump -P isa_check_arm64.cmake
# The build fails when a variant's code is above or below its own instruction level:
#   level 0 (armv8, the arm64-v8a baseline): no FEAT_DOTPROD instructions (SDOT / UDOT) anywhere,
#     so the dispatcher can run it on any arm64 CPU;
#   level 1 (armv8-dotprod): at least one — the proof that the variant really was compiled with
#     -march=armv8.2-a+dotprod (and that the check itself can read the object).
cmake_minimum_required(VERSION 3.21)

execute_process(COMMAND ${OBJDUMP} -d ${OBJ} OUTPUT_VARIABLE dis ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    file(REMOVE ${OBJ})
    message(FATAL_ERROR "isa_check(${OBJ}): '${OBJDUMP} -d' failed: ${err}")
endif()

# The mnemonics sit alone between whitespace ("sdot v0.4s, ..."), never inside addresses or symbol
# names; searching for a tab-anchored mnemonic finds exactly the instructions.
string(FIND "${dis}" "\tsdot" sdot)
string(FIND "${dis}" "\tudot" udot)
if(sdot EQUAL -1 AND udot EQUAL -1)
    set(count 0)
else()
    set(count 1)
endif()

if(LEVEL EQUAL 0 AND NOT count EQUAL 0)
    file(REMOVE ${OBJ})
    message(FATAL_ERROR "isa_check(${OBJ}): the baseline armv8 variant contains dotprod instructions")
endif()
if(LEVEL EQUAL 1 AND count EQUAL 0)
    file(REMOVE ${OBJ})
    message(FATAL_ERROR "isa_check(${OBJ}): the armv8-dotprod variant contains no dotprod instructions "
                        "(is -march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD set?)")
endif()

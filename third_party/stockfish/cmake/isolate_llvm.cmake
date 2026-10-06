# Scacelith build script (not part of upstream Stockfish): the LLVM-toolchain counterpart of
# cmake/isolate.cmake, for builds whose toolchain has LLVM's tools and no GNU binutils (the
# Android NDK; a future Linux/AArch64 port). Run as
#   cmake -DTAG=<variant tag> -DENTRY=<entry symbol> -DOUT=<file.o> -DINPUTS=<obj|obj|...>
#         -DLD=... -DOBJCOPY=... -DOBJDUMP=... -DNM=... -DREADELF=... -P isolate_llvm.cmake
#
# Turns the object files of one instruction-set variant of Stockfish into ONE relocatable object
# whose only global symbols are ENTRY, sfinit_<TAG>_start and sfinit_<TAG>_end, for the same
# reason as on the desktop (cmake/isolate.cmake): every variant compiles the same inline functions
# and C++ standard library templates with its own instruction set, and left alone the linker would
# merge those copies program-wide — the game, or the baseline variant, could call a dotprod copy
# on a CPU without dotprod and crash. The steps, and what differs from the GNU script:
#   1. partial link (ld.lld -r), which resolves the variant's COMDAT groups inside the variant.
#      lld has no --force-group-allocation: the surviving groups stay groups in the output. That
#      is harmless here, as verified with lld 18 end to end: the final link only ever merges
#      groups whose signature symbols are the same symbol, and step 4 makes every definition —
#      every group signature with it — local, so no copy of the variant can be merged into the
#      game's or another variant's std::... templates afterwards (three compiled copies of a
#      shared template symbol stay three copies in the final shared library). The verification
#      below checks the invariant (no group signature left global).
#      The same partial link also gathers the initialiser sections: unlike GNU ld, ld.lld -r does
#      not concatenate the inputs' .init_array sections into one output section (it keeps one per
#      input, the sections of one compile differing from the others' by COMDAT membership), so a
#      two-rule linker script builds the table as .sfinit_<TAG> and gathers its relocations as
#      .rela.sfinit_<TAG> straight away — lld rejects an object whose section is targeted by more
#      than one relocation section ("multiple relocation sections to one section are not
#      supported"), so the relocations must be gathered with it. The link adjusts the entries'
#      offsets to the gathered table (verified: the entries of the second input table shift by its
#      offset in the output). The rule also defines the bounding symbols sfinit_<TAG>_start and
#      sfinit_<TAG>_end inside the section: llvm-objcopy's --add-symbol cannot attach a symbol to
#      a section that a COMDAT group still references (the table gathers the grouped
#      .init_array of some inputs) and would emit it as SHN_ABS, which the final link resolves to
#      an absolute — wrong — address. A priority section (.init_array.N) matches no rule,
#      survives, and fails the scan below, as on the desktop.
#   2. the table bounded by the symbols sfinit_<TAG>_start and sfinit_<TAG>_end: the C runtime no
#      longer runs the initialisers at load (they may use the variant's instructions), the
#      dispatcher (scacelith/cpu_arm64.cpp) runs those of the variant it selects.
#   3. every other defined symbol made local. Undefined references (bionic's libc, libc++,
#      libunwind) stay external and bind to the single copies in the shared library. llvm-objcopy
#      does not accumulate repeated --keep-global-symbol flags, so the symbols to keep go through
#      one --keep-global-symbols=<file> instead.
#   4. verified: exactly the three globals, bounds that span the whole initialiser table, no
#      symbol both defined and undefined, no group signature left global, no section this scheme
#      does not handle (initialiser priorities, destructor tables, thread-local storage), and a
#      relocation for every 8-byte entry of the table (a zeroed entry would make the dispatcher
#      call address 0).
cmake_minimum_required(VERSION 3.21)
string(REPLACE "|" ";" INPUTS "${INPUTS}")

# A failure removes the output, so that the next build runs the script again.
function(fail message)
    file(REMOVE ${OUT} "${OUT}.ld" "${OUT}.keep.txt")
    message(FATAL_ERROR "isolate(${TAG}): ${message}")
endfunction()

function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        fail("'${ARGV}' failed: ${err}")
    endif()
    set(RUN_OUT "${out}" PARENT_SCOPE)
endfunction()

# 1. Partial link, gathering the initialiser table into .sfinit_<TAG> on the way, with the
# bounding symbols defined inside it.
file(WRITE "${OUT}.ld" "SECTIONS { .sfinit_${TAG} : { "
                            "sfinit_${TAG}_start = .; KEEP(*(.init_array .ctors)) sfinit_${TAG}_end = .; } "
                            ".rela.sfinit_${TAG} : { KEEP(*(.rela.init_array .rela.ctors)) } }\n")
run(${LD} -r -T "${OUT}.ld" -o "${OUT}.r.o" ${INPUTS})

# The initialiser table, and no section this scheme does not handle. (The section index may have
# 3 digits.)
run(${OBJDUMP} -h "${OUT}.r.o")
string(REPLACE "\n" ";" lines "${RUN_OUT}")
set(initSize "")
foreach(line IN LISTS lines)
    if(line MATCHES "^ *[0-9]+ ([^ ]+) +([0-9a-f]+) ")
        set(name "${CMAKE_MATCH_1}")
        set(size "0x${CMAKE_MATCH_2}")
        if(name STREQUAL ".sfinit_${TAG}")
            if(NOT initSize STREQUAL "")
                fail("several sfinit_${TAG} sections")
            endif()
            set(initSize "${size}")
        elseif(name MATCHES "^\\.(init_array|ctors)(\\.|$)|^\\.(fini_array|dtors|tbss|tdata|tls)")
            fail("unhandled initialiser section ${name} (not gathered by the partial link, or destructor tables or TLS)")
        endif()
    endif()
endforeach()
if(initSize STREQUAL "")
    fail("no static initialisers found (Stockfish has some: the object list is wrong)")
endif()

# 2. The bounds come from the partial link itself (see above); the section keeps the .init_array
#    type and W+A flags, which is all the dispatcher needs (it reads pointers).

# 3. Localise everything else. llvm-objcopy does not accumulate repeated --keep-global-symbol
# flags on one command line: one --keep-global-symbols=<file> with one symbol per line does.
file(WRITE "${OUT}.keep.txt" "${ENTRY}\nsfinit_${TAG}_start\nsfinit_${TAG}_end\n")
run(${OBJCOPY} --keep-global-symbols=${OUT}.keep.txt "${OUT}.r.o" ${OUT})
file(REMOVE "${OUT}.r.o" "${OUT}.ld" "${OUT}.keep.txt")

# 4. Verification: exactly the three globals.
run(${NM} --defined-only -g ${OUT})
string(REGEX MATCHALL "[^\n]+" symbols "${RUN_OUT}")
set(names "")
foreach(symbol IN LISTS symbols)
    string(REGEX REPLACE "^.* " "" name "${symbol}")
    list(APPEND names "${name}")
endforeach()
list(SORT names)
set(expected ${ENTRY} sfinit_${TAG}_end sfinit_${TAG}_start)
list(SORT expected)
if(NOT names STREQUAL expected)
    fail("unexpected global symbols: ${names}")
endif()
# The bounds span the whole initialiser table (the dispatcher runs what lies between them).
foreach(symbol IN LISTS symbols)
    if(symbol MATCHES "^([0-9a-f]+) [A-Za-z] sfinit_${TAG}_(start|end)$")
        set(bound_${CMAKE_MATCH_2} "0x${CMAKE_MATCH_1}")
    endif()
endforeach()
math(EXPR tableSize "${bound_end} - ${bound_start}")
if(tableSize EQUAL 0 OR NOT tableSize EQUAL initSize)
    fail("the initialiser table bounds ${bound_start}..${bound_end} do not span its ${initSize} bytes")
endif()
# Every 8-byte entry of the table is relocated: without its relocations the table is zeros and the
# dispatcher calls address 0. The partial link may leave more than one .rela.sfinit_<TAG> section
# (one per gathered input .init_array); their entries all apply to the same table.
run(${READELF} -r ${OUT})
string(REGEX MATCHALL "Relocation section '\\.rela\\.?sfinit_${TAG}'[^\\n]* contains ([0-9]+) entr" relocations "${RUN_OUT}")
set(relocationCount 0)
foreach(relocation IN LISTS relocations)
    string(REGEX REPLACE "^.* contains ([0-9]+) entr.*$" "\\1" n "${relocation}")
    math(EXPR relocationCount "${relocationCount} + ${n}")
endforeach()
math(EXPR entries "${initSize} / 8")
if(NOT relocationCount EQUAL entries)
    fail("the initialiser table has ${relocationCount} relocations for ${entries} entries")
endif()
# No symbol both undefined and defined: it would stay unresolved, or bind to another copy.
run(${NM} ${OUT})
string(REGEX MATCHALL "[^\n]+" symbols "${RUN_OUT}")
set(undefined "")
set(defined "")
foreach(symbol IN LISTS symbols)
    if(symbol MATCHES "^ +U (.+)$")
        list(APPEND undefined "${CMAKE_MATCH_1}")
    elseif(symbol MATCHES "^[0-9a-f]+ [A-Za-z] (.+)$")
        list(APPEND defined "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES undefined)
list(REMOVE_DUPLICATES defined)
set(both ${undefined} ${defined})
list(LENGTH both total)
list(REMOVE_DUPLICATES both)
list(LENGTH both distinct)
if(NOT total EQUAL distinct)
    math(EXPR n "${total} - ${distinct}")
    fail("${n} symbols are both undefined and defined")
endif()
# No group signature left global (step 1): a global signature could be merged into the same-named
# group of the game or another variant at the final link. After the exactly-three-globals check
# above, every global name is one of the three kept symbols, none of which heads a group.
run(${READELF} -g ${OUT})
string(REGEX MATCHALL "group section \\[[ 0-9]+\\] `\\.group' \\[[^\\]]+\\]" groups "${RUN_OUT}")
foreach(group IN LISTS groups)
    string(REGEX REPLACE "^.*\\.group' \\[|\\]$" "" signature "${group}")
    if(signature IN_LIST names)
        fail("the COMDAT group signature ${signature} is still global")
    endif()
endforeach()

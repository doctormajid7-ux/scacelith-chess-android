# The game's version (Semantic Versioning): SCACELITH_VERSION_CORE is MAJOR.MINOR.PATCH, the version
# of project() and of the Windows version resource; SCACELITH_VERSION_PRERELEASE is the pre-release
# label ("beta.1"; empty for a final release). SCACELITH_VERSION, the version the game shows and
# sends, joins them ("1.0.0-beta.1"); a release is tagged v<SCACELITH_VERSION>, which the release
# workflow checks.
#
# Included by CMakeLists.txt before project(); `cmake -P cmake/version.cmake` prints
# SCACELITH_VERSION.
set(SCACELITH_VERSION_CORE 1.0.0)
set(SCACELITH_VERSION_PRERELEASE beta.2-android.1)

if(SCACELITH_VERSION_PRERELEASE)
    set(SCACELITH_VERSION ${SCACELITH_VERSION_CORE}-${SCACELITH_VERSION_PRERELEASE})
else()
    set(SCACELITH_VERSION ${SCACELITH_VERSION_CORE})
endif()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo ${SCACELITH_VERSION})
endif()

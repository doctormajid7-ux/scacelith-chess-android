#include "settings.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cmath>

namespace game {

Settings& settings() {
    static Settings s;
    return s;
}

render::RenderSettings Settings::renderSettings() const {
    render::RenderSettings r;
    r.applyPreset(render::Quality(std::clamp(quality, 0, 3)));
    r.renderScale = std::clamp(renderScale, 0.5f, 2.0f);
    if (!motionBlur) r.motionBlur = false;
    if (!depthOfField) r.dof = false;
    if (simpleRenderer) {
        // The simple renderer draws none of these: do not allocate or bake them either.
        r.simple = true;
        r.planarReflections = r.ssr = r.volumetrics = r.taa = r.motionBlur = r.dof = r.ssao = false;
        r.lightProbes = simpleIndirect;
        r.probeResolution = 64;   // the probes' capture size; its bake runs once, at load
        r.probeBounces = 1;
        r.simpleMaterials = std::clamp(simpleMaterials, 0, 2);
        r.simpleMsaa = simpleMsaa;
    }
    return r;
}

namespace {
// The fallback of a settings file, "" when it has none: the file of a portable install, next to
// the executable, falls back to the user data directory when it cannot be written (a read-only
// folder); any other file (the user data directory's, an explicit --ini) has none, so its reads
// and writes never go to two different files.
std::string fallbackFor(const std::string& p) {
    if (!p.empty() && p != plat::exeDirectory() + "Scacelith.ini") return std::string();
    std::string alt = plat::userDataDirectory() + "Scacelith.ini";
    return alt != p ? alt : std::string();
}
}  // namespace

bool Settings::load(const std::string& p) {
    path = p;
    IniFile ini;
    // Where save() writes when p cannot be written (none next to the executable in a read-only
    // folder, a read-only one there): that copy is the one read back. 'path' stays the first choice.
    std::string alt = fallbackFor(p);
    if (!alt.empty() && !IniFile::writable(p) && ini.load(alt)) {
        LOGI("settings read from %s (%s cannot be written)", alt.c_str(), p.c_str());
    } else if (!ini.load(p)) {
        if (!alt.empty() && ini.load(alt)) {
            LOGI("settings read from %s", alt.c_str());
        } else {
            LOGI("no settings file at %s, using defaults", p.c_str());
            brightnessCalibrated = false;  // a first start: the brightness calibration comes first
            applyLanguage();
            return false;
        }
    }
    displayWidth = ini.getInt("display.width", displayWidth);
    displayHeight = ini.getInt("display.height", displayHeight);
    fullscreen = ini.getBool("display.fullscreen", fullscreen);
    vsync = ini.getBool("display.vsync", vsync);
    renderScale = ini.getFloat("display.render_scale", renderScale);
    if (std::isnan(renderScale)) renderScale = 1.0f;  // passes std::clamp, then int(w * NaN) is undefined
    textScale = ini.getFloat("display.text_scale", textScale);
    if (std::isnan(textScale)) textScale = 1.0f;
    textScale = std::clamp(textScale, 0.8f, 1.6f);
    quality = ini.getInt("graphics.quality", quality);
    motionBlur = ini.getBool("graphics.motion_blur", motionBlur);
    depthOfField = ini.getBool("graphics.depth_of_field", depthOfField);
    simpleRenderer = ini.getBool("graphics.simple_renderer", simpleRenderer);
    simpleMaterials = std::clamp(ini.getInt("graphics.simple_materials", simpleMaterials), 0, 2);
    simpleIndirect = ini.getBool("graphics.simple_indirect", simpleIndirect);
    simpleMsaa = ini.getBool("graphics.simple_msaa", simpleMsaa);
    brightness = ini.getFloat("graphics.brightness", brightness);
    // Absent from a file written before the calibration existed: its player has chosen already.
    brightnessCalibrated = ini.getBool("graphics.brightness_calibrated", true);
    masterVolume = ini.getFloat("audio.master_volume", masterVolume);
    effectsVolume = ini.getFloat("audio.effects_volume", effectsVolume);
    ambienceVolume = ini.getFloat("audio.ambience_volume", ambienceVolume);
    ambience = ini.getBool("audio.ambience", ambience);
    voiceVolume = std::clamp(ini.getFloat("audio.voice_volume", voiceVolume), 0.0f, 1.0f);
    showLegalMoves = ini.getBool("gameplay.show_legal_moves", showLegalMoves);
    showCoordinates = ini.getBool("gameplay.show_coordinates", showCoordinates);
    mouseSensitivity = ini.getFloat("gameplay.mouse_sensitivity", mouseSensitivity);
    invertLook = ini.getBool("gameplay.invert_look", invertLook);
    gameCursor = ini.getBool("gameplay.game_cursor", gameCursor);
    touchDirect = ini.getBool("gameplay.touch_direct", touchDirect);
    autoPressClock = ini.getBool("gameplay.auto_press_clock", autoPressClock);
    ignoreOpponentHead = ini.getBool("gameplay.ignore_opponent_head", ignoreOpponentHead);
    nextColor = ini.getInt("gameplay.next_color", nextColor);
    handoverSeconds = std::clamp(ini.getFloat("gameplay.handover_seconds", handoverSeconds), 0.0f, 2.0f);
    if (handoverSeconds > 0.0f && handoverSeconds < 0.8f) handoverSeconds = 0.8f;
    opponent = std::clamp(ini.getInt("newgame.opponent", opponent), 0, 1);
    difficultyPreset = ini.getInt("newgame.difficulty", difficultyPreset);
    timeControlPreset = ini.getInt("newgame.time_control", timeControlPreset);
    customBaseSeconds = ini.getInt("newgame.custom_base_seconds", customBaseSeconds);
    customIncrementSeconds = ini.getInt("newgame.custom_increment_seconds", customIncrementSeconds);
    customDelaySeconds = ini.getInt("newgame.custom_delay_seconds", customDelaySeconds);
    customSkillLevel = ini.getInt("engine.skill_level", customSkillLevel);
    customLimitElo = ini.getBool("engine.limit_elo", customLimitElo);
    customElo = ini.getInt("engine.elo", customElo);
    customDepth = ini.getInt("engine.depth", customDepth);
    customMoveTimeMs = ini.getInt("engine.move_time_ms", customMoveTimeMs);
    customNodes = ini.getInt("engine.nodes", customNodes);
    engineThreads = ini.getInt("engine.threads", engineThreads);
    engineHashMB = ini.getInt("engine.hash_mb", engineHashMB);
    engineArch = ini.getString("engine.arch", engineArch);
    humanizeThinking = ini.getBool("engine.humanize", humanizeThinking);
    setPlayerRecord(elo::readRecord(ini, "player"));
    for (int i = 0; i < 2; ++i) {
        const char* side = i == 0 ? "white" : "black";
        hotseatNames[i] = ini.getString(std::string("hotseat.") + side + "_name", hotseatNames[i]);
        hotseatHands[i] = std::clamp(ini.getInt(std::string("hotseat.") + side + "_hand", hotseatHands[i]), -1,
                                     int(ui::font::HAND_STYLE_COUNT) - 1);
    }
    hotseatClockRightOf = std::clamp(ini.getInt("hotseat.clock_right_of", hotseatClockRightOf), 0, 1);
    hotseatRated = ini.getBool("hotseat.rated", hotseatRated);
    localPlayers.clear();
    // Every player save() wrote. A section removed by hand leaves a gap: skipped within the first
    // 256 entries (beyond, the entries are read as long as they follow one another, as save()
    // writes them). The bound only stops a hand-edited file: the duplicate check is linear.
    for (int n = 1; n <= 10000; ++n) {
        std::string sec = "local_player_" + std::to_string(n) + ".";
        if (!ini.has(sec + "name")) {
            if (n <= 256) continue;
            break;
        }
        LocalPlayer p;
        p.name = ini.getString(sec + "name");
        p.record = elo::readRecord(ini, "local_player_" + std::to_string(n));
        if (!p.name.empty() && !findLocalPlayer(p.name)) localPlayers.push_back(p);
    }
    viewerWhitePreset = ini.getInt("viewer.white_preset", viewerWhitePreset);
    viewerBlackPreset = ini.getInt("viewer.black_preset", viewerBlackPreset);
    viewerTimeControl = ini.getInt("viewer.time_control", viewerTimeControl);
    viewerCustomBaseSeconds = ini.getInt("viewer.custom_base_seconds", viewerCustomBaseSeconds);
    viewerCustomIncrementSeconds = ini.getInt("viewer.custom_increment_seconds", viewerCustomIncrementSeconds);
    viewerCustomDelaySeconds = ini.getInt("viewer.custom_delay_seconds", viewerCustomDelaySeconds);
    viewerShowControls = ini.getBool("viewer.show_controls", viewerShowControls);
    onlineCustomServer = ini.getBool("online.custom_server", onlineCustomServer);
    onlineHost = ini.getString("online.host", onlineHost);
    onlineApiPort = std::clamp(ini.getInt("online.api_port", onlineApiPort), 1, 65535);
    onlineWsPort = std::clamp(ini.getInt("online.ws_port", onlineWsPort), 0, 65535);
    onlinePin = ini.getString("online.pinned_sha256", onlinePin);
    onlineCategory = ini.getString("online.category", onlineCategory);
    onlineRated = ini.getBool("online.rated", onlineRated);
    onlineColor = std::clamp(ini.getInt("online.color", onlineColor), 0, 2);
    onlineCustomBaseSeconds = ini.getInt("online.custom_base_seconds", onlineCustomBaseSeconds);
    onlineCustomIncrementSeconds = ini.getInt("online.custom_increment_seconds", onlineCustomIncrementSeconds);
    directPort = std::clamp(ini.getInt("direct.port", directPort), 1, 65535);
    directUpnp = ini.getBool("direct.upnp", directUpnp);
    directTimeControl = ini.getInt("direct.time_control", directTimeControl);
    directBaseSeconds = ini.getInt("direct.base_seconds", directBaseSeconds);
    directIncrementSeconds = ini.getInt("direct.increment_seconds", directIncrementSeconds);
    directColor = std::clamp(ini.getInt("direct.color", directColor), 0, 2);
    directAutoPress = ini.getBool("direct.auto_press_clock", directAutoPress);
    directAddress = ini.getString("direct.address", directAddress);
    directJoinPort = std::clamp(ini.getInt("direct.join_port", directJoinPort), 1, 65535);
    readCoachSettings(ini, *this);  // [coach], [tts] (settings_coach.cpp)
    // [archive] saved games
    saveGames = ini.getBool("archive.save_games", saveGames);
    language = ini.getString("interface.language", language);
    subtitles = std::clamp(ini.getInt("interface.subtitles", subtitles), 0, 2);
    playerName = ini.getString("player.name", playerName);
    if (playerName.empty()) playerName = "Human";
    handStyle = ui::font::HandStyle(std::clamp(ini.getInt("player.hand_style", int(handStyle)), 0, int(ui::font::HAND_STYLE_COUNT) - 1));
    applyLanguage();
    return true;
}

LocalPlayer* Settings::findLocalPlayer(const std::string& name) {
    for (LocalPlayer& p : localPlayers)
        if (p.name == name) return &p;
    return nullptr;
}

const LocalPlayer* Settings::findLocalPlayer(const std::string& name) const {
    for (const LocalPlayer& p : localPlayers)
        if (p.name == name) return &p;
    return nullptr;
}

elo::Record Settings::playerRecord() const {
    elo::Record r;
    r.rating = playerElo;
    r.games = playerGames;
    r.wins = playerWins;
    r.draws = playerDraws;
    r.losses = playerLosses;
    r.peak = std::max(playerPeakElo, playerElo);
    r.rated = playerRated;
    r.countedGames = playerCountedGames;
    r.unratedGames = playerUnratedGames;
    r.unratedOpponents = playerUnratedOpponents;
    r.unratedHalfPoints = playerUnratedHalfPoints;
    return r;
}

void Settings::setPlayerRecord(const elo::Record& r) {
    playerElo = r.rating;
    playerGames = r.games;
    playerWins = r.wins;
    playerDraws = r.draws;
    playerLosses = r.losses;
    playerPeakElo = r.peak;
    playerRated = r.rated;
    playerCountedGames = r.countedGames;
    playerUnratedGames = r.unratedGames;
    playerUnratedOpponents = r.unratedOpponents;
    playerUnratedHalfPoints = r.unratedHalfPoints;
}

LocalPlayer& Settings::localPlayer(const std::string& name) {
    if (LocalPlayer* p = findLocalPlayer(name)) return *p;
    LocalPlayer p;
    p.name = name;
    localPlayers.push_back(p);
    return localPlayers.back();
}

void Settings::applyLanguage() {
    if (language.empty() || i18n::languageIndex(language) < 0) {
        std::string os = plat::systemLanguage();
        language = i18n::matchLocale(os);
        LOGI("language: %s from the system locale '%s'", language.c_str(), os.c_str());
    }
    std::string code = language;
    const std::vector<std::string> args = plat::commandLine();
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--lang") code = i18n::matchLocale(args[i + 1]);
    i18n::setLanguage(code);
}

bool Settings::save() const {
    if (readOnly) return true;
    IniFile ini;
    ini.setInt("display.width", displayWidth);
    ini.setInt("display.height", displayHeight);
    ini.setBool("display.fullscreen", fullscreen);
    ini.setBool("display.vsync", vsync);
    ini.setFloat("display.render_scale", renderScale);
    ini.setFloat("display.text_scale", textScale);
    ini.setInt("graphics.quality", quality);
    ini.setBool("graphics.motion_blur", motionBlur);
    ini.setBool("graphics.depth_of_field", depthOfField);
    ini.setBool("graphics.simple_renderer", simpleRenderer);
    ini.setInt("graphics.simple_materials", simpleMaterials);
    ini.setBool("graphics.simple_indirect", simpleIndirect);
    ini.setBool("graphics.simple_msaa", simpleMsaa);
    ini.setFloat("graphics.brightness", brightness);
    ini.setBool("graphics.brightness_calibrated", brightnessCalibrated);
    ini.setFloat("audio.master_volume", masterVolume);
    ini.setFloat("audio.effects_volume", effectsVolume);
    ini.setFloat("audio.ambience_volume", ambienceVolume);
    ini.setBool("audio.ambience", ambience);
    ini.setFloat("audio.voice_volume", voiceVolume);
    ini.setBool("gameplay.show_legal_moves", showLegalMoves);
    ini.setBool("gameplay.show_coordinates", showCoordinates);
    ini.setFloat("gameplay.mouse_sensitivity", mouseSensitivity);
    ini.setBool("gameplay.invert_look", invertLook);
    ini.setBool("gameplay.game_cursor", gameCursor);
    ini.setBool("gameplay.touch_direct", touchDirect);
    ini.setBool("gameplay.auto_press_clock", autoPressClock);
    ini.setBool("gameplay.ignore_opponent_head", ignoreOpponentHead);
    ini.setInt("gameplay.next_color", nextColor);
    ini.setFloat("gameplay.handover_seconds", handoverSeconds);
    ini.setInt("newgame.opponent", opponent);
    ini.setInt("newgame.difficulty", difficultyPreset);
    ini.setInt("newgame.time_control", timeControlPreset);
    ini.setInt("newgame.custom_base_seconds", customBaseSeconds);
    ini.setInt("newgame.custom_increment_seconds", customIncrementSeconds);
    ini.setInt("newgame.custom_delay_seconds", customDelaySeconds);
    ini.setInt("engine.skill_level", customSkillLevel);
    ini.setBool("engine.limit_elo", customLimitElo);
    ini.setInt("engine.elo", customElo);
    ini.setInt("engine.depth", customDepth);
    ini.setInt("engine.move_time_ms", customMoveTimeMs);
    ini.setInt("engine.nodes", customNodes);
    ini.setInt("engine.threads", engineThreads);
    ini.setInt("engine.hash_mb", engineHashMB);
    ini.set("engine.arch", engineArch);
    ini.setBool("engine.humanize", humanizeThinking);
    elo::writeRecord(ini, "player", playerRecord());
    ini.set("hotseat.white_name", hotseatNames[0]);
    ini.set("hotseat.black_name", hotseatNames[1]);
    ini.setInt("hotseat.white_hand", hotseatHands[0]);
    ini.setInt("hotseat.black_hand", hotseatHands[1]);
    ini.setInt("hotseat.clock_right_of", hotseatClockRightOf);
    ini.setBool("hotseat.rated", hotseatRated);
    for (size_t i = 0; i < localPlayers.size(); ++i) {
        const LocalPlayer& p = localPlayers[i];
        std::string sec = "local_player_" + std::to_string(i + 1);
        ini.set(sec + ".name", p.name);
        elo::writeRecord(ini, sec, p.record);
    }
    ini.setInt("viewer.white_preset", viewerWhitePreset);
    ini.setInt("viewer.black_preset", viewerBlackPreset);
    ini.setInt("viewer.time_control", viewerTimeControl);
    ini.setInt("viewer.custom_base_seconds", viewerCustomBaseSeconds);
    ini.setInt("viewer.custom_increment_seconds", viewerCustomIncrementSeconds);
    ini.setInt("viewer.custom_delay_seconds", viewerCustomDelaySeconds);
    ini.setBool("viewer.show_controls", viewerShowControls);
    ini.setBool("online.custom_server", onlineCustomServer);
    ini.set("online.host", onlineHost);
    ini.setInt("online.api_port", onlineApiPort);
    ini.setInt("online.ws_port", onlineWsPort);
    ini.set("online.pinned_sha256", onlinePin);
    ini.set("online.category", onlineCategory);
    ini.setBool("online.rated", onlineRated);
    ini.setInt("online.color", onlineColor);
    ini.setInt("online.custom_base_seconds", onlineCustomBaseSeconds);
    ini.setInt("online.custom_increment_seconds", onlineCustomIncrementSeconds);
    ini.setInt("direct.port", directPort);
    ini.setBool("direct.upnp", directUpnp);
    ini.setInt("direct.time_control", directTimeControl);
    ini.setInt("direct.base_seconds", directBaseSeconds);
    ini.setInt("direct.increment_seconds", directIncrementSeconds);
    ini.setInt("direct.color", directColor);
    ini.setBool("direct.auto_press_clock", directAutoPress);
    ini.set("direct.address", directAddress);
    ini.setInt("direct.join_port", directJoinPort);
    writeCoachSettings(ini, *this);  // [coach], [tts] (settings_coach.cpp)
    // [archive] saved games
    ini.setBool("archive.save_games", saveGames);
    ini.set("interface.language", language);
    ini.setInt("interface.subtitles", subtitles);
    ini.set("player.name", playerName);
    ini.setInt("player.hand_style", int(handStyle));
    if (!path.empty() && ini.save(path)) return true;
    std::string alt = fallbackFor(path);
    if (!alt.empty() && ini.save(alt)) return true;
    LOGW("could not save settings to %s", path.empty() ? alt.c_str() : path.c_str());
    return false;
}

}  // namespace game

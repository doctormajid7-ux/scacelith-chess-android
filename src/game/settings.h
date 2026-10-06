// Persistent player settings (Scacelith.ini). Shared by the menus (ui/), the game and main.cpp.
#pragma once
#include "../render/renderer.h"
#include "../ui/ui_font.h"
#include "elo.h"
#include "../core/ini.h"
#include "../coach/subtitles.h"
#include <string>
#include <vector>

namespace game {

// A player of rated hot-seat games on this PC, by name (New Game > Human, same PC > Rated game):
// one [local_player_N] section each (name and the record's keys, elo::readRecord). Separate from
// the [player] rating, which only games against Stockfish change.
struct LocalPlayer {
    std::string name;
    elo::Record record;
};

struct Settings {
    // [display]
    int displayWidth = 1600;
    int displayHeight = 900;
    bool fullscreen = true;       // borderless fullscreen
    bool vsync = true;
#ifdef __ANDROID__
    // A phone screen has 1.5-2x the pixels of a 1080p monitor and a fraction of a desktop GPU's
    // bandwidth: start at the Low preset (no planar reflections, SSR, volumetrics, DOF) and 60 %
    // resolution, TAA reconstructing the rest. Options > Graphics raises both.
    float renderScale = 0.6f;
    float textScale = 1.2f;       // interface text size (a phone's screen is small and close)
    // [graphics]
    int quality = 0;              // 0 Low, 1 Medium, 2 High, 3 Ultra
    bool motionBlur = false;
    bool depthOfField = false;
#else
    float renderScale = 1.0f;
    float textScale = 1.0f;       // interface text size
    // [graphics]
    int quality = 2;              // 0 Low, 1 Medium, 2 High, 3 Ultra
    bool motionBlur = true;
    bool depthOfField = true;
#endif
    // render::RenderSettings::simple, the light renderer, on every platform by default: it keeps
    // modest machines (phones, integrated GPUs, a GTX 1050) fluid. Options > Graphics turns the
    // full renderer back on.
    bool simpleRenderer = true;
    // The simple renderer's options (Options > Graphics, under the simple renderer's switch).
    int simpleMaterials = 1;      // 0 plain colours, 1 baked textures, 2 procedural per pixel
    bool simpleIndirect = true;   // indirect lighting (the light probes, ~3.5 ms on a phone)
#ifdef __ANDROID__
    bool simpleMsaa = false;      // 4x MSAA (~6 ms on a phone at full resolution)
#else
    bool simpleMsaa = true;
#endif
    float brightness = 0.0f;      // exposure compensation (EV)
    // The brightness calibration was completed (Continue or Esc on its page). Until then every
    // start opens on it (screenshot runs excepted), however the previous runs ended. A settings
    // file from before the calibration existed counts as completed.
    bool brightnessCalibrated = false;
    // [audio]
    float masterVolume = 0.9f;
    float effectsVolume = 1.0f;
    float ambienceVolume = 0.7f;
    bool ambience = true;
    float voiceVolume = 1.0f;     // the coach's voice (Coach mode), before the master volume
    // [gameplay]
    bool showLegalMoves = true;   // highlight the legal destinations of the touched piece
    bool showCoordinates = false; // board has no printed coordinates by default (tournament boards)
    float mouseSensitivity = 1.0f;
    bool invertLook = false;
    bool gameCursor = true;       // the game's own pointer at the table instead of the system arrow
    bool touchDirect = false;     // Android: the finger is the pointer (else a touchpad moves an arrow)
    // The player's robot presses the clock by itself once the move is on the board (games on
    // this PC: against Stockfish and two players; online, the server or the host decides).
    bool autoPressClock = false;
    // Online: the opponent's robot ignores the opponent's head movements (automatic gaze instead).
    bool ignoreOpponentHead = false;
    int nextColor = -1;           // -1 = random (first game), 0 = white, 1 = black
    // Hot-seat: the view goes from one player's eyes to the other's after each move, in a camera
    // flight of this length (0.8 to 2 s), or 0 = an instant cut through black (motion sickness).
    float handoverSeconds = 1.6f;
    // [newgame] last choices on the new game screen
    int opponent = 0;             // 0 Stockfish, 1 a second human on this PC (hot-seat)
    int difficultyPreset = 3;     // index into ai::presets()
    int timeControlPreset = 5;    // index into chess::timeControlPresets()
    int customBaseSeconds = 600;
    int customIncrementSeconds = 5;
    int customDelaySeconds = 0;
    // Custom engine settings (used when the "Custom" difficulty preset is selected)
    int customSkillLevel = 10;
    bool customLimitElo = false;
    int customElo = 1800;
    int customDepth = 0;
    int customMoveTimeMs = 0;
    int customNodes = 0;
    int engineThreads = 1;
    int engineHashMB = 64;
    // Stockfish instruction-set variant: "auto" = the best this CPU runs, or a variant name capping
    // it, e.g. "x86-64-sse41-popcnt" (troubleshooting; ai::Engine::setArchLimit)
    std::string engineArch = "auto";
    bool humanizeThinking = true; // spend realistic time before moving
    // [player] the human's rating (elo.h), updated after every rated game against Stockfish
    int playerElo = 1500;
    int playerGames = 0, playerWins = 0, playerDraws = 0, playerLosses = 0;
    int playerPeakElo = 1500;
    // Its FIDE unrated phase (elo::Record): false until the first rating, the sums of those games;
    // the games counted in the rating.
    bool playerRated = false;
    int playerCountedGames = 0;
    int playerUnratedGames = 0, playerUnratedOpponents = 0, playerUnratedHalfPoints = 0;
    elo::Record playerRecord() const;  // the [player] fields above as one record
    void setPlayerRecord(const elo::Record& r);
    // [hotseat] last choices of the two-player game (New Game > Human, same PC), by colour
    std::string hotseatNames[2];  // "" = the Options > Player name for White, "Player 2" (translated) for Black
    int hotseatHands[2] = {-1, -1};  // ui::font::HandStyle; -1 = the Options > Player hand / another one
    int hotseatClockRightOf = 0;  // the clock stands at White's (0) or Black's (1) right
    bool hotseatRated = false;    // rated between the two names (localPlayers), friendly by default
    // [local_player_N] ratings of the rated hot-seat games, by name
    std::vector<LocalPlayer> localPlayers;
    LocalPlayer* findLocalPlayer(const std::string& name);          // nullptr when unknown
    const LocalPlayer* findLocalPlayer(const std::string& name) const;
    LocalPlayer& localPlayer(const std::string& name);              // found, or added (1500, no games)
    // [viewer] last choices on the Watch a Game page (Stockfish vs Stockfish)
    int viewerWhitePreset = 5;    // index into ai::presets() (Custom excluded)
    int viewerBlackPreset = 4;
    int viewerTimeControl = 5;    // index into chess::timeControlPresets(), -1 = custom
    int viewerCustomBaseSeconds = 300;
    int viewerCustomIncrementSeconds = 3;
    int viewerCustomDelaySeconds = 0;
    bool viewerShowControls = true; // the controls hint overlay (H)
    // [online] the server of online play (Options > Online). Never any token: the network layer
    // keeps the sessions itself, per server.
    bool onlineCustomServer = false;  // false = the official server of this build (when it has one)
    std::string onlineHost;
    int onlineApiPort = 443;          // HTTPS API (Scacelith servers use 443 for both by default)
    int onlineWsPort = 0;             // WSS; 0 = the API port
    std::string onlinePin;            // SHA-256 of a self-signed community server's certificate
    // Last choices of the online pages
    std::string onlineCategory = "5+3";
    bool onlineRated = true;
    int onlineColor = 0;              // challenges and private games: 0 random, 1 White, 2 Black
    int onlineCustomBaseSeconds = 600, onlineCustomIncrementSeconds = 5;
    // [direct] direct match (no server)
    int directPort = 47100;
    bool directUpnp = true;
    static constexpr int kDefaultDirectTimeControl = 7;  // 10+5
    int directTimeControl = kDefaultDirectTimeControl;    // index into chess::timeControlPresets(), -1 = custom
    int directBaseSeconds = 600, directIncrementSeconds = 5;
    int directColor = 0;              // host's colour: 0 random, 1 White, 2 Black
    bool directAutoPress = true;      // host: the robots press the clock by themselves
    std::string directAddress;        // last address joined
    int directJoinPort = 47100;
    // [coach] last choices on the Coach page
    int coachLevel = 1;           // 0 = the rules of chess (interactive lesson), 1.. = the Elo bands
    int coachColour = 2;          // the player's colour: 0 White, 1 Black, 2 alternate from game to game
    int coachNextColour = 0;      // alternate: the player's colour in the next coach game (0 White, 1 Black)
    bool coachRulesDone = false;  // the rules lesson was completed once
    // The player's colour in the next coach game (0 White, 1 Black): the rules lesson is always
    // played with White, else the chosen colour, or the next one of the alternation. The game
    // flips coachNextColour after each alternating game (and saves).
    int coachPlayerColour() const {
        if (coachLevel <= 0) return 0;
        if (coachColour == 0 || coachColour == 1) return coachColour;
        return coachNextColour == 1 ? 1 : 0;
    }
    // What the coach remembers between games (coach::Session's results): the finished coach games,
    // oldest first (the level suggestion reads them; the last kCoachHistoryMax are kept), whether
    // the end-of-game appraisal has explained what accuracy is, and the chapter the rules lesson
    // resumes at (0 = from the start).
    struct CoachGame {
        int level = 1;
        int result = 0;           // +1 the player won, 0 draw, -1 lost
        double accuracy = -1.0;   // the player's game accuracy (percent), -1 unknown
    };
    static constexpr int kCoachHistoryMax = 40;
    std::vector<CoachGame> coachHistory;
    bool coachAccuracyExplained = false;
    int coachLessonChapter = 0;
    // [tts] the coach's voice (src/tts): threads of one synthesis (0 = automatic: 2), the voice
    // (-1 = the default teacher voice, else an index into the model's voices), flow-matching steps
    // (5; 3 is faster and slightly rougher) and the kernels' instruction set ("auto", or a cap for
    // troubleshooting: avx512, avxvnni, avx2, sse2, scalar; tts::setArchCap).
    int ttsThreads = 0;
    int ttsVoice = -1;
    int ttsSteps = 5;
    std::string ttsArch = "auto";
    // [coach] voice: the coach speaks (Options > Audio > Coach voice). Its model is not shipped:
    // the game offers to download it (game/coach_model.h) the first time Coach mode or this
    // option needs it. Off = the coach's words as subtitles only; declining the download prompt
    // switches it off (remembered), switching it back on offers the download again.
    bool coachVoice = true;
    // [archive] saved games (game_archive.h): the games played on this PC and the direct matches
    // are saved as PGN files in the pgn folder of the user data directory when they end.
    bool saveGames = true;
    // [interface]
    std::string language;         // i18n code ("fr", "zh-Hant"...); "" = the OS language (first start)
    // The coach's words at the bottom of the screen (SubtitleMode).
    int subtitles = 0;
    // [player] (written on the scoresheets)
    std::string playerName = "Human";
    ui::font::HandStyle handStyle = ui::font::HAND_CAVEAT;  // Latin/Cyrillic handwriting

    // Selects the UI language (i18n::setLanguage): "--lang <code>" on the command line for this
    // session, else 'language', else the OS language when supported, else English. load() calls
    // it; an empty 'language' receives the language chosen from the OS.
    void applyLanguage();

    render::RenderSettings renderSettings() const;
    // The settings of 'path' (a missing file = defaults). For the default file next to the
    // executable, save() falls back to the user data directory when that folder cannot be written
    // (a read-only install), and load() reads the settings back from there when the file is
    // missing. An explicit --ini file has no fallback: it is read and written there or not at all.
    bool load(const std::string& path);
    bool save() const;
    std::string path;
    // Screenshot runs (--shot) leave the settings file as it is: save() writes nothing.
    bool readOnly = false;
};

Settings& settings();

// The [coach] and [tts] sections of Settings::load / save, apart so that the unit tests can check
// their round trip without the game target (settings_coach.cpp is in the core library). The
// history is one line of "level:result:accuracy" entries, oldest first ("3:1:81.4 3:-1:-").
std::string encodeCoachHistory(const std::vector<Settings::CoachGame>& games);
std::vector<Settings::CoachGame> decodeCoachHistory(const std::string& text);
void readCoachSettings(const IniFile& ini, Settings& s);
void writeCoachSettings(IniFile& ini, const Settings& s);

// [interface] subtitles: the coach's words written at the bottom of the screen.
enum SubtitleMode { SubtitlesAuto = 0, SubtitlesOn = 1, SubtitlesOff = 2 };
// Whether the coach's subtitles are shown: always when its voice cannot be heard (voice files
// missing or failed to load), else per the option, 'Automatic' meaning when the coach does not
// speak the language of the menus (Chinese menus: the coach speaks English). Pass
// i18n::language() (the language in use, not Settings::language) and the coach's speech
// language for it.
inline bool coachSubtitlesShown(int mode, const std::string& uiLanguage, const std::string& speechLanguage,
                                bool voiceAvailable) {
    return coach::subtitlesShown(mode, uiLanguage, speechLanguage, voiceAvailable);
}

}  // namespace game

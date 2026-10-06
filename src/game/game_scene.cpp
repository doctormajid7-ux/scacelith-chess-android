#include "game_scene.h"
#include "coach_model.h"
#include "../audio/audio.h"
#include "../character/skeleton.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../render/post/postfx.h"
#include "../scene/piece_silhouette.h"
#include "elo.h"
#include "game_archive.h"
#include "game_saving.h"
#include "game_scene_detail.h"
#include "../ui/ui_font.h"
#include "../ui/ui_draw.h"
#ifdef __ANDROID__
#include "../platform/platform_android.h"
#endif
#include "layout.h"
#include "look_up.h"
#include "scoresheet_layout.h"
#include "settings.h"
#include "scacelith_version.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace m;
using namespace chess;

namespace game {

using namespace scene_detail;

namespace {

constexpr float kFadeOut = 0.8f;          // menu -> black
constexpr float kFadeIn = 1.6f;           // black -> seated at the table
constexpr float kFov = 52.0f * DEG;
constexpr float kEyeLimit = 18.0f * DEG;
constexpr float kGlanceFov = 24.0f * DEG;  // looking at one's own scoresheet (S): a closer look
constexpr float kEyeFStop = 11.0f;         // the player's eyes at kFov: a 2.2 mm pupil in a bright hall

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

float wrapAngle(float a) {
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

float yawOf(const mat4& t) {
    vec3 z = t.c[2].xyz();
    return std::atan2(z.x, z.z);
}

// Also the suffix of the "notify.touched.*" translation keys.
const char* pieceName(PieceType t) {
    switch (t) {
    case Pawn: return "pawn";
    case Knight: return "knight";
    case Bishop: return "bishop";
    case Rook: return "rook";
    case Queen: return "queen";
    case King: return "king";
    default: return "piece";
    }
}

// Heavier pieces sound slightly lower.
float piecePitch(PieceType t) {
    static const float kPitch[7] = {1.0f, 1.05f, 1.0f, 1.0f, 0.98f, 0.96f, 0.94f};
    return kPitch[t];
}

float distPointSegment2D(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 1e-9f ? clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

bool parseVec3(const std::string& v, vec3& out) {
    std::vector<std::string> c = split(v, ',');
    if (c.size() != 3) return false;
    out = vec3(float(std::atof(c[0].c_str())), float(std::atof(c[1].c_str())), float(std::atof(c[2].c_str())));
    return true;
}

// The seat whose clock stands on its left plays (and presses the clock) with its left hand.
// White (+Z, facing -Z) has +X on its right, Black the opposite.
character::Side playHandFor(int seat, bool clockPosX) {
    return hotseat::playsLeftHanded(seat, clockPosX) ? character::Side::Left : character::Side::Right;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The human's handwriting style on the scoresheets (Options > Player; the name: localPlayerName).
int humanHandStyle() { return int(settings().handStyle); }

const char* sideKey(Color c) { return c == White ? "viewer.side.white" : "viewer.side.black"; }

}  // namespace

// =============================================================================================
// Setup
// =============================================================================================

bool GameScene::init(AppContext& ctx) {
    ctx_ = &ctx;
    rng_.seedWith(ctx.screenshotMode ? 20260927u : plat::randomSeed());
    startWatching_ = ctx.hasArg("--viewer") || ctx.hasArg("--demo");
    skipIntro_ = ctx.hasArg("--no-intro");
    handoverPreview_ = ctx.hasArg("--handover-preview");
    debugCamera_ = ctx.hasArg("--cam");
    if (ctx.hasArg("--mouse")) {
        std::vector<std::string> c = split(ctx.argValue("--mouse"), ',');
        if (c.size() == 2) {
            mouseOverride_ = true;
            mouseOverridePos_ = vec2(float(std::atof(c[0].c_str())), float(std::atof(c[1].c_str())));
        }
    }

    Settings& s = settings();
    setup_.difficulty = s.difficultyPreset;
    setup_.timeControl = s.timeControlPreset;
    setup_.customBaseSeconds = s.customBaseSeconds;
    setup_.customIncrementSeconds = s.customIncrementSeconds;
    setup_.customDelaySeconds = s.customDelaySeconds;
    setup_.skillLevel = s.customSkillLevel;
    setup_.limitElo = s.customLimitElo;
    setup_.elo = s.customElo;
    setup_.depth = s.customDepth;
    setup_.moveTimeMs = s.customMoveTimeMs;
    setup_.nodes = s.customNodes;
    int presetCount = int(ai::presets().size());
    watch_.whitePreset = std::clamp(s.viewerWhitePreset, 0, presetCount - 1);
    watch_.blackPreset = std::clamp(s.viewerBlackPreset, 0, presetCount - 1);
    watch_.timeControl = s.viewerTimeControl;
    watch_.customBaseSeconds = s.viewerCustomBaseSeconds;
    watch_.customIncrementSeconds = s.viewerCustomIncrementSeconds;
    watch_.customDelaySeconds = s.viewerCustomDelaySeconds;
    hudVisible_ = s.viewerShowControls;
    // Saved games: the title page's "Saved games" lists this folder, games are saved there.
    library_.folder = plat::appDataDirectory() + "pgn/";
    // Command-line games (screenshots, tests).
    if (ctx.hasArg("--white-preset")) watch_.whitePreset = std::clamp(std::atoi(ctx.argValue("--white-preset").c_str()), 0, presetCount - 1);
    if (ctx.hasArg("--black-preset")) watch_.blackPreset = std::clamp(std::atoi(ctx.argValue("--black-preset").c_str()), 0, presetCount - 1);
    if (ctx.hasArg("--tc")) {
        int tc = std::atoi(ctx.argValue("--tc").c_str());
        setup_.timeControl = watch_.timeControl = std::clamp(tc, 0, int(timeControlPresets().size()) - 1);
    }
    initHotSeatArgs();
    initCoachArgs();
    // --replay <file> scripted: --replay-speed x1|x2|x4|x8|instant, --replay-paused.
    const std::string speed = ctx.argValue("--replay-speed");
    replaySpeedArg_ = speed == "x2" ? replay::Speed::X2 : speed == "x4" ? replay::Speed::X4 : speed == "x8" ? replay::Speed::X8
                      : speed == "instant" ? replay::Speed::Instant : replay::Speed::X1;
    replayPausedArg_ = ctx.hasArg("--replay-paused");
    for (const std::string& k : split(ctx.argValue("--replay-keys"), ','))
        if (!k.empty()) replayKeys_.push_back(k);

    if (!audio::init()) LOGW("audio unavailable, continuing silently");
    if (!ui::init()) {
        LOGE("UI initialisation failed");
        return false;
    }
    std::vector<ui::DifficultyInfo> diff;
    for (const ai::Preset& p : ai::presets()) diff.push_back({p.name, p.description, p.approxElo});
    ui::setDifficultyList(diff);
    std::vector<std::string> tcs;
    for (const TimeControl& tc : timeControlPresets()) tcs.push_back(tc.label());
    ui::setTimeControlList(tcs);
    ui::setResolutionList({{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}});
    ui::setVersionString("Scacelith " SCACELITH_VERSION);
    initOnline();
    ui::setSoundCallback([](ui::Sound snd) {
        switch (snd) {
        case ui::Sound::Hover: audio::playUI(audio::Sfx::UIHover, 0.35f); break;
        case ui::Sound::Tick: audio::playUI(audio::Sfx::UIHover, 0.25f); break;
        default: audio::playUI(audio::Sfx::UIClick, 0.6f); break;
        }
    });
    applySettings(false);

    state_ = State::Loading;
    fade_ = 1.0f;
    if (ctx.screenshotMode) {
        // Deterministic runs load everything up front.
        while (!world_.loadStep(true)) {}
        finishLoading();
    }
    return true;
}

void GameScene::finishLoading() {
    world_.setupRenderer(*ctx_->renderer);
    // The loading screen's frames went through the renderer with an empty scene: the static sun
    // shadows and the light probes are baked again from the world, on the first frame that draws it.
    ctx_->renderer->invalidateStatic();
    if (!scorekeeper_.init(true)) LOGW("scoresheets unavailable");
    ai::Engine::setArchLimit(settings().engineArch);
    engineOk_ = engine_.start();
    if (!engineOk_) LOGW("Stockfish is unavailable: the opponent will play random legal moves");
    initAnimators();
    board_.reset(true);
    world_.setClockSide(true);
    clock_.setup(chosenTimeControl());
    if (!startOnline_.empty()) {
        // --start-online: straight to the table once an opponent is found (at once with the fakes).
        if (startOnline_ != "direct") {
            onlineSession().quickStart(startOnline_, localPlayerName());
        } else if (ctx_->hasArg("--online-mock")) {
            // A direct match joined with a valid code: the fakes' friend hosts it (10+5).
            onlineSession().joinDirect("192.168.1.23", 47100, "ABCD-EFGH-JKMN");
            for (int i = 0; i < 400 && !onlineSession().gameReady(); ++i) onlineSession().runMock(25.0);
        } else {
            LOGW("--start-online direct needs --online-mock");
        }
        if (takeOnlineGame()) {
            fade_ = skipIntro_ ? 0.0f : 1.0f;
            state_ = State::Intro;
            stateTime_ = skipIntro_ ? kFadeIn : 0.0f;
        } else {
            enterMenu();
        }
        return;
    }
    if (ctx_->hasArg("--replay")) {
        // --replay <file> [--game N]: N counts from 1, as the saved games page shows a file's
        // games; the scene's replay setup counts from 0.
        int n = ctx_->hasArg("--game") ? std::max(1, std::atoi(ctx_->argValue("--game").c_str())) : 1;
        if (loadReplay(ctx_->argValue("--replay"), n - 1)) {
            mode_ = GameMode::Replay;
            setupNewGame();
            if (skipIntro_) {
                fade_ = 0.0f;
                startPlaying();
            } else {
                state_ = State::Intro;
                stateTime_ = 0.0f;
            }
            return;
        }
        LOGE("--replay: no game to replay, to the menu");
    }
    if (ctx_->hasArg("--start") || startWatching_ || coachArgs_.start) {
        mode_ = startWatching_          ? GameMode::Watch
                : coachArgs_.start      ? GameMode::Coach
                : ctx_->hasArg("--hotseat") ? GameMode::HotSeat
                                        : GameMode::Play;
        setupNewGame();
        if (skipIntro_) {
            fade_ = 0.0f;
            startPlaying();
        } else {
            state_ = State::Intro;
            stateTime_ = 0.0f;
        }
        std::string touch = ctx_->argValue("--touch");
        if (!touch.empty() && state_ == State::Playing && turn_ == Turn::HumanIdle) {
            int id = board_.idAt(parseSquare(touch));
            if (id >= 0) humanTouch(id);
        }
        glance_ = !watching() && ctx_->hasArg("--glance");
    } else {
        // Until the player completes it (not in deterministic screenshot runs), or --calibrate:
        // the brightness calibration comes before the title page.
        if ((!settings().brightnessCalibrated && !ctx_->screenshotMode) || ctx_->hasArg("--calibrate")) ui::openBrightnessCalibration();
        enterMenu();
    }
}

void GameScene::initAnimators() {
    const character::Skeleton& sk = character::robotSkeleton();
    for (int seat = 0; seat < 2; ++seat) {
        float zs = seat == 0 ? 1.0f : -1.0f;  // White at +Z faces -Z
        // The hand on the clock side plays and presses the clock; the other one writes.
        seats_[seat].playHand = playHandFor(seat, world_.clockOnPositiveX());
        anim_[seat] = anim::Animator();
        anim_[seat].init(sk, vec3(0, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), zs, seats_[seat].playHand);
        // The playing hand rests on the table beside the board, on the clock side (White's right
        // is +X, Black's is -X).
        float side = anim_[seat].playHand() == character::Side::Right ? zs : -zs;
        anim_[seat].setRestHand(vec3(side * layout::REST_HAND_X, layout::TABLE_TOP_Y, zs * layout::REST_HAND_Z));
        anim_[seat].pieceTransform = [this](int id) {
            const PieceObject* p = board_.byId(id);
            return p ? p->transform : mat4();
        };
        anim_[seat].pieceGripInfo = [this](int id) {
            const PieceObject* p = board_.byId(id);
            int t = p ? int(p->type) : int(Pawn);
            // (height, grip height above the base, grip radius), metres
            return vec3(layout::PIECE_HEIGHT[t], layout::PIECE_HEIGHT[t] * layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
        };
        // Exact obstacle heights for the hand paths: standing pieces only (not those in a hand).
        anim_[seat].pathObstacleTop = [this](vec3 from, vec3 to) {
            float top = layout::BOARD_TOP_Y;
            vec2 a(from.x, from.z), b(to.x, to.z);
            for (const PieceObject& p : board_.pieces()) {
                if (pieceInHand(p)) continue;
                float d = distPointSegment2D(vec2(p.basePos.x, p.basePos.z), a, b);
                if (d < layout::PIECE_BASE_RADIUS[p.type] + 0.02f) top = std::max(top, p.basePos.y + layout::PIECE_HEIGHT[p.type]);
            }
            return top;
        };
        anim_[seat].obstacleTopNear = [this](vec3 pt, float radius, int ignoreId) {
            float top = layout::BOARD_TOP_Y;
            for (const PieceObject& p : board_.pieces()) {
                if (p.id == ignoreId || pieceInHand(p)) continue;
                float d = length(vec2(p.basePos.x - pt.x, p.basePos.z - pt.z));
                if (d < radius + layout::PIECE_BASE_RADIUS[p.type]) top = std::max(top, p.basePos.y + layout::PIECE_HEIGHT[p.type]);
            }
            return top;
        };
        hasPrevGlobals_[seat] = false;
    }
}

bool GameScene::pieceInHand(const PieceObject& p) const {
    return p.held || anim_[0].holding(p.id) || anim_[1].holding(p.id);
}

void GameScene::enterMenu() {
    cancelAiSearch();
    // A game still unsaved (the window of a game left in an unusual way): saved now, while link_
    // and the game are still there (archiveGame does nothing when it ran already).
    archiveGame(game_.isOver());
    if (online()) {
        mode_ = GameMode::Play;
        link_ = nullptr;
    }
    if (replaying()) {
        // Back from a replay: the menu opens on the saved games, where it was chosen.
        mode_ = GameMode::Play;
        ui::openSavedGames();
    }
    if (hotSeat()) mode_ = GameMode::Play;  // the New Game page chooses again
    if (coach()) {
        leaveCoachGame();
        mode_ = GameMode::Play;
    }
    world_.setCoachSeat(-1);
    world_.setBoardCoordinates(settings().showCoordinates);
    refreshCoachVoice();  // the Coach page says whether the coach can be heard
    handover_.cancel();
    inputGate_.reset();
    inputBlocked_ = false;
    state_ = State::Menu;
    stateTime_ = 0.0f;
    turn_ = Turn::None;
    paused_ = false;
    game_.setEndDetection(true);
    game_.reset();
    arbiter_.reset(game_);
    board_.reset(true);
    world_.setClockSide(true);
    clock_.setup(chosenTimeControl());
    leverSide_ = leverTarget_ = -1.0f;
    dest_.clear();
    initAnimators();
    newScoresheets();
    for (auto& a : anim_) a.setHeadOverride(false);
    menuAngle_ = 0.9f;
    cameraCut_ = true;
    plat::setMouseCaptured(false);
    dragging_ = false;
    glance_ = false;
    glanceBlend_ = 0.0f;
    pressTouched_ = false;
    observerPlaced_ = false;
    followEyes_ = false;
    clockFrozen_ = false;
}

TimeControl GameScene::chosenTimeControl() const {
    if (online()) {
        TimeControl tc;
        tc.unlimited = false;
        tc.baseMs = og_.baseMs;
        tc.incrementMs = og_.incMs;
        return tc;
    }
    if (coach()) {
        TimeControl tc;  // a coach game has no clock to watch (W1: the move completes on release)
        tc.unlimited = true;
        return tc;
    }
    if (replaying()) return replayTimeControl();
    const auto& presets = timeControlPresets();
    // Watching uses the choice of the Watch a Game page ([viewer] in the .ini).
    int index = watching() ? watch_.timeControl : setup_.timeControl;
    int base = watching() ? watch_.customBaseSeconds : setup_.customBaseSeconds;
    int inc = watching() ? watch_.customIncrementSeconds : setup_.customIncrementSeconds;
    int delay = watching() ? watch_.customDelaySeconds : setup_.customDelaySeconds;
    if (index >= 0 && index < int(presets.size())) return presets[size_t(index)];
    TimeControl tc;
    tc.unlimited = false;
    tc.baseMs = int64_t(std::max(10, base)) * 1000;
    tc.incrementMs = int64_t(std::max(0, inc)) * 1000;
    tc.delayMs = int64_t(std::max(0, delay)) * 1000;
    return tc;
}

ai::EngineSettings GameScene::engineSettingsFor(int preset) const {
    const auto& presets = ai::presets();
    int idx = std::clamp(preset, 0, int(presets.size()) - 1);
    ai::EngineSettings es = presets[size_t(idx)].settings;
    if (idx == int(presets.size()) - 1) {  // "Custom"
        es.skillLevel = setup_.skillLevel;
        es.limitStrength = setup_.limitElo;
        es.elo = setup_.elo;
        es.depth = setup_.depth;
        es.moveTimeMs = setup_.moveTimeMs;
        es.nodes = setup_.nodes;
    }
    es.threads = std::max(1, settings().engineThreads);
    es.hashMB = std::max(16, settings().engineHashMB);
    es.humanize = settings().humanizeThinking && !ctx_->screenshotMode;
    return es;
}

void GameScene::setupNewGame() {
    Settings& s = settings();
    ++round_;
    // Saved games: a new record of the moves' times; nothing saved yet.
    moveElapsedMs_.clear();
    moveClockMs_.clear();
    plyElapsedMs_ = 0.0;
    gameStartedAt_ = 0;
    archived_ = false;
    directMatch_ = false;
    if (online()) {
        setupOnlineGame();  // the game announced by the session: colours, link, server state
    } else if (replaying()) {
        humanColor_ = White;  // nobody: keeps the human-game helpers well defined
        LOGI("Replay: %s vs %s, %s, %d plies, %s", replayRecord_.tag("White", "?").c_str(), replayRecord_.tag("Black", "?").c_str(),
             replayRecord_.tag("Date", "?").c_str(), int(replayRecord_.plies.size()), replayRecord_.result.c_str());
    } else if (watching()) {
        humanColor_ = White;  // nobody: keeps the human-game helpers well defined
        LOGI("New game (watching): %s vs %s, %s", ai::presets()[size_t(watch_.whitePreset)].name,
             ai::presets()[size_t(watch_.blackPreset)].name, chosenTimeControl().label().c_str());
    } else if (coach()) {
        setupCoachGame();  // colours, level; the session starts with the game
    } else if (hotSeat()) {
        humanColor_ = White;  // both seats are human: the helpers of the one-human game see White
        // The players as set up on the New Game page (an empty name or a hand < 0: the defaults,
        // Options > Player for White, "Player 2" in another hand for Black).
        int mine = std::clamp(humanHandStyle(), 0, int(ui::font::HAND_STYLE_COUNT) - 1);
        for (int i = 0; i < 2; ++i) {
            std::string n = trimmed(setup_.names[i]);
            hsPlayers_.names[i] = !n.empty() ? n : i == 0 ? localPlayerName() : std::string(i18n::tr("hotseat.player2"));
            int hand = setup_.hands[i];
            hsPlayers_.hands[i] = hand >= 0 ? std::clamp(hand, 0, int(ui::font::HAND_STYLE_COUNT) - 1)
                                            : i == 0 ? mine : (mine + 1) % int(ui::font::HAND_STYLE_COUNT);
        }
        hsPlayers_.clockRightOf = std::clamp(setup_.clockRightOf, 0, 1);
        hsPlayers_.rated = setup_.rated;
        LOGI("New game (hot-seat): %s (White) vs %s (Black), %s, clock at %s's right, %s", hsPlayers_.names[0].c_str(),
             hsPlayers_.names[1].c_str(), chosenTimeControl().label().c_str(), hsPlayers_.names[hsPlayers_.clockRightOf].c_str(),
             hsPlayers_.rated ? "rated" : "friendly");
    } else {
        humanColor_ = s.nextColor < 0 ? (rng_.uniform() < 0.5f ? White : Black) : Color(s.nextColor & 1);
        std::string forced = ctx_->argValue("--human");
        if (forced == "white") humanColor_ = White;
        if (forced == "black") humanColor_ = Black;
        LOGI("New game: human plays %s, %s, difficulty %d", humanColor_ == White ? "White" : "Black",
             chosenTimeControl().label().c_str(), setup_.difficulty);
    }

    game_.reset();
    // The rules lesson's positions may be over on load (two kings alone) and its exercises go on
    // after a mate: its game never ends by itself. Nor does a replay: the record says when and how
    // it ended (moves after a dead position are played as recorded).
    game_.setEndDetection(!lesson() && !replaying());
    // A replay starts from the record's position (a FEN game), set up before the board is.
    if (replaying() && !replayRecord_.fen.empty() && !game_.resetFromFEN(replayRecord_.fen))
        LOGW("replay: the start position '%s' cannot be set up", replayRecord_.fen.c_str());
    arbiter_.reset(game_);
    // At the human player's right hand; at White's right when watching; where the New Game page
    // put it in a hot-seat game.
    bool clockPosX = hotSeat() ? hotseat::clockOnPositiveX(hsPlayers_.clockRightOf) : (watching() || humanColor_ == White);
    world_.setClockSide(clockPosX);
    board_.reset(clockPosX);
    if (replaying() && !replayRecord_.fen.empty()) board_.syncTo(game_.position());  // a FEN game
    clock_.setup(chosenTimeControl());
    clockAccumMs_ = 0.0;
    // White's clock runs first, as if Black had pressed: Black's half of the lever is down.
    leverSide_ = leverTarget_ = world_.clockHalfForSeat(-1.0f) == 1 ? 1.0f : -1.0f;

    dest_.clear();
    turn_ = Turn::None;
    paused_ = false;
    touchedId_ = -1;
    touchedSq_ = placedTo_ = NoSquare;
    pressQueued_ = false;
    drawOfferPending_ = false;
    drawOfferPly_ = -1;
    lastAiEval_ = 0;
    for (int i = 0; i < 2; ++i) {
        lastEval_[i] = 0;
        hasEval_[i] = false;
        lastOfferPly_[i] = -1;
    }
    pendingOffer_ = -1;
    clockFrozen_ = false;
    gameOverShown_ = false;
    showMoveList_ = false;
    rated_ = eloCounted_ = false;
    eloBefore_ = eloAfter_ = s.playerElo;

    initAnimators();
    configureSeats();
    newScoresheets();
    if (online()) scorekeeper_.setDetails(onlineSheetDetails());
    if (hotSeat()) {
        Scorekeeper::Details d;
        d.note = i18n::tr(hsPlayers_.rated ? "hotseat.sheet.rated" : "hotseat.sheet.friendly");
        scorekeeper_.setDetails(d);
    }
    if (replaying()) scorekeeper_.setDetails(replaySheetDetails());  // the record's event and round
    // The players filled in their header before sitting down at the board, as in a tournament
    // round: the pens only record the moves. The rules lesson records nothing.
    if (!lesson()) scorekeeper_.writeHeaderInstantly();
    // The coach's seat wears its marking (W3); the others none.
    world_.setCoachSeat(coach() ? aiSeat() : -1);
    world_.setBoardCoordinates(settings().showCoordinates || (coach() && coachLevel_ <= 2));
    if (engineOk_ && !online() && !hotSeat() && !replaying()) {  // a replay's robots never search
        engine_.newGame();
        engine_.configure(seats_[seats_[0].human() ? 1 : 0].engine);
    }
    if (watching()) {
        anim_[0].setHeadOverride(false);
        anim_[1].setHeadOverride(false);
    } else if (!hotSeat()) {  // hot-seat: below, once the side to move is known
        anim_[humanSeat()].setHeadOverride(true, 0.0f, kBaseGazePitch);
        anim_[aiSeat()].setHeadOverride(false);
    }
    for (Look& l : look_) l = Look();
    cameraCut_ = true;
    handover_.cancel();
    inputGate_.reset();
    inputBlocked_ = false;
    drawOfferBy_ = drawCardFor_ = -1;
    writeGrace_ = 0.0f;
    captionAge_ = 0.0f;
    scriptPos_ = scriptThenPos_ = 0;
    scriptPromo_ = NoPiece;
    scriptMenu_ = ui::MenuAction::None;
    for (int i = 0; i < 2; ++i) hsEloBefore_[i] = hsEloAfter_[i] = seats_[i].elo;

    if (watching()) {
        // Observer: --cam / --look / --fov, else --viewpoint N, else beside the table. The jump
        // happens once the robots are posed (face viewpoints need their heads).
        if (!followEyes_) eyesSeat_ = -1;
        // "Watch again" keeps the observer where it is.
        if (observerPlaced_) {
            if (followEyes_) pendingViewpoint_ = 0;  // into the eyes of White, who moves first
        } else {
            observerPlaced_ = true;
            pendingCamArg_ = ctx_->hasArg("--cam") && round_ == 1;
            pendingViewpoint_ = handoverPreview_ ? 0 : 1;
            if (ctx_->hasArg("--viewpoint") && round_ == 1)
                pendingViewpoint_ = std::clamp(std::atoi(ctx_->argValue("--viewpoint").c_str()), 0, 9);
        }
        viewpointShown_ = -1;
    }

    if (online()) {
        rebuildOnline();  // moves already made (a fast opponent, a reconnection)
        return;
    }
    if (replaying()) {
        setupReplay();  // the replay clock, from the start position just set up
        return;
    }
    std::string moves = ctx_->argValue("--moves");
    if (!moves.empty()) applyMovesInstantly(split(moves, ','));
    if (hotSeat()) {
        // The view starts in the eyes of the player to move.
        viewSeat_ = seatOf(game_.position().sideToMove());
        anim_[viewSeat_].setHeadOverride(true, 0.0f, kBaseGazePitch);
        anim_[1 - viewSeat_].setHeadOverride(false);
    }
}

void GameScene::configureSeats() {
    if (online()) {
        configureOnlineSeats();
        return;
    }
    if (hotSeat()) {
        configureHotSeatSeats();
        return;
    }
    if (coach()) {
        configureCoachSeats();
        return;
    }
    if (replaying()) {
        configureReplaySeats();
        return;
    }
    const Settings& s = settings();
    for (int i = 0; i < 2; ++i) {
        Seat& st = seats_[i];
        character::Side hand = st.playHand;  // set by initAnimators()
        st = Seat();
        st.color = colorOfSeat(i);
        st.playHand = hand;
        if (!watching() && st.color == humanColor_) {
            st.controller = Controller::Human;
            st.name = localPlayerName();
            st.elo = s.playerElo;
            st.provisional = s.playerRecord().provisional();
        } else {
            st.controller = Controller::Stockfish;
            st.name = "Stockfish";
            st.preset = watching() ? (i == 0 ? watch_.whitePreset : watch_.blackPreset) : setup_.difficulty;
            st.preset = std::clamp(st.preset, 0, int(ai::presets().size()) - 1);
            st.engine = engineSettingsFor(st.preset);
            st.elo = ai::presetElo(st.preset, st.engine);
            st.presetName = ai::presets()[size_t(st.preset)].name;
        }
    }
}

void GameScene::applyMovesInstantly(const std::vector<std::string>& uci) {
    for (const std::string& u : uci) {
        Move mv = game_.position().parseUCI(u);
        if (!mv.valid()) {
            LOGW("--moves: '%s' is not legal here", u.c_str());
            break;
        }
        game_.play(mv);
        moveElapsedMs_.push_back(-1);  // no time for a move set up (saved games)
        moveClockMs_.push_back(-1);
    }
    board_.syncTo(game_.position());
    arbiter_.reset(game_);
    // The moves are on the scoresheets already, as if the game had been adjourned and resumed.
    if (!game_.sanMoves().empty()) scorekeeper_.writeMovesInstantly(game_.sanMoves());
}

void GameScene::newScoresheets() {
    Scorekeeper::Player p[2];
    for (int i = 0; i < 2; ++i) {
        p[i].name = seats_[i].name;
        p[i].elo = seats_[i].elo;
        p[i].rating = seats_[i].ratingText;
        p[i].handStyle = handStyleOf(i);
        p[i].blueInk = seats_[i].human() || i == 0;  // Stockfish as Black writes in black
    }
    // A replay's sheets bear the date the game was played.
    std::string date = replaying() ? replaySheetDate() : scoresheetDate(ctx_->screenshotMode);
    scorekeeper_.newGame(anim_, world_.clockOnPositiveX(), p, std::max(1, round_), date);
}

int GameScene::handStyleOf(int seat) const {
    // Hot-seat: each player writes their own sheet in the hand chosen on the New Game page.
    if (hotSeat()) return std::clamp(hsPlayers_.hands[seat & 1], 0, int(ui::font::HAND_STYLE_COUNT) - 1);
    // Every sheet is written in its owner's hand; the two players never share one.
    int human = std::clamp(humanHandStyle(), 0, int(ui::font::HAND_STYLE_COUNT) - 1);
    if (seats_[seat].human()) return human;
    int other = seats_[1 - seat].human() ? human : -1;
    int want = watching() ? (seat == 0 ? ui::font::HAND_MARCK : ui::font::HAND_BADSCRIPT) : ui::font::HAND_MARCK;
    if (want == other) want = (want + 1) % ui::font::HAND_STYLE_COUNT;
    return want;
}

void GameScene::startPlaying() {
    state_ = State::Playing;
    stateTime_ = 0.0f;
    if (!watching() && !online() && !hotSeat() && !coach()) {
        // Colours alternate from one game to the next (coach games alternate their own, [coach]).
        settings().nextColor = int(opposite(humanColor_));
        settings().save();
    }
    if (game_.status() != GameStatus::Ongoing && !online()) {
        endGame();
        return;
    }
    gameStartedAt_ = std::time(nullptr);  // the saved game's Date and Time
    // Online, the server keeps the clocks (the display reads them); a replay's come from its
    // record (replayClock_).
    if (!online() && !replaying()) clock_.start(game_.position().sideToMove());
    // The authority of this game leaves the clock press to the players: say so once.
    if (online() && !og_.autoPress) ui::notify(i18n::tr("notify.manual_clock"), 5.0f);
    captionAge_ = 0.0f;  // hot-seat: "Alice, your move"
    audio::playUI(audio::Sfx::GameStart, 0.6f);
    // Both players take their pen while White thinks (the rules lesson has no scoresheet).
    if (!lesson()) scorekeeper_.startRecording();
    if (coach()) startCoachGame();
    beginTurn();
}

void GameScene::beginTurn() {
    Color stm = game_.position().sideToMove();
    plyElapsedMs_ = 0.0;  // the time this move takes (saved games)
    touchedId_ = -1;
    touchedSq_ = placedTo_ = NoSquare;
    pressQueued_ = false;
    moveStaged_ = false;
    hoverId_ = -1;
    aimSq_ = NoSquare;
    aimLegal_ = false;
    pressTouched_ = false;
    scriptWait_ = kScriptThink;
    if (online() && game_.status() != GameStatus::Ongoing) {
        turn_ = Turn::None;  // the server's GameEnd follows
    } else if (replaying()) {
        turn_ = Turn::None;  // the replay clock says when the next move begins (updateReplay)
    } else if (isHumanSeat(seatOf(stm))) {
        turn_ = Turn::HumanIdle;
    } else if (lesson()) {
        turn_ = Turn::LessonWait;  // the lesson's moves for Black come from its script (playLessonMove)
    } else if (online()) {
        turn_ = Turn::RemoteWaiting;
        // The opponent may already hold a piece (their gestures, while my robot was pressing).
        anim_[seatOf(stm)].setThinking(remoteLive_.pieceId < 0);
    } else {
        turn_ = Turn::AiThinking;
        aiRequested_ = false;
    }
}

void GameScene::cancelAiSearch() {
    if (engineOk_ && !coach() && turn_ == Turn::AiThinking && aiRequested_ && !aiHasMove_) engine_.cancelMove();
}

void GameScene::endGame() {
    cancelAiSearch();
    // A piece still gripped on its square is let go.
    if (turn_ == Turn::HumanTouched && touchedId_ >= 0) {
        PieceObject* p = board_.byId(touchedId_);
        if (p) {
            dest_[p->id].push_back({touchedSq_, p->basePos, false});
            anim_[inputSeat()].enqueue({task(anim::TaskType::Place, p->id, p->basePos), task(anim::TaskType::Retract)});
        }
    }
    // So is the online opponent's piece held live (a move they put down is taken back).
    if (online()) cancelRemoteLive();
    clock_.stop();
    turn_ = Turn::None;
    state_ = State::GameOver;
    stateTime_ = 0.0f;
    gameOverShown_ = false;
    endHandshakeDone_ = false;
    paused_ = false;
    for (auto& a : anim_) a.setThinking(false);
    if (online()) {
        onlineResult();  // the server's result and reason
    } else if (replaying()) {
        // The record's ending: game_ never ends by itself in a replay, and a game resigned, lost
        // on time or drawn by agreement still looks ongoing on the board.
        const std::string& r = replayRecord_.result;
        // An unfinished game ("*") shows a dash on the card and nothing on the sheets.
        resultText_ = r == "1-0" ? "1-0" : r == "0-1" ? "0-1" : r == "1/2-1/2" ? "\xC2\xBD-\xC2\xBD" : "\xE2\x80\x94";
        // As the saved games page says it (ui_library.cpp reasonText): nothing for a plain
        // "Normal" termination (a resignation or an agreed draw in a file from elsewhere).
        const std::string key = replay::endReasonKey(replayRecord_);
        const std::string term = replayRecord_.tag("Termination");
        reasonText_ = !key.empty() && i18n::has(key.c_str()) ? std::string(i18n::tr(key.c_str()))
                      : r == "*"                              ? std::string(i18n::tr("library.unfinished"))
                      : term == "?" || term == "normal" || term == "Normal" ? std::string()
                                                                             : term;
        isDraw_ = r == "1/2-1/2";
        playerWon_ = false;
    } else {
        GameStatus st = game_.status();
        resultText_ = st == GameStatus::WhiteWins ? "1-0" : st == GameStatus::BlackWins ? "0-1" : "\xC2\xBD-\xC2\xBD";
        reasonText_ = endReasonText(game_.endReason());
        isDraw_ = st == GameStatus::Draw;
        playerWon_ = (st == GameStatus::WhiteWins && humanColor_ == White) || (st == GameStatus::BlackWins && humanColor_ == Black);
    }
    if (replaying()) {
        LOGI("Replay over: %s (%s), %d plies", replayRecord_.result.c_str(), reasonText_.c_str(), int(game_.moves().size()));
    } else if (coach()) {
        // The coach's name in the interface language, its level's rating in the event tag.
        PgnTags tags;
        tags.event = "Coach level " + std::to_string(coachLevel_) + " (" + std::to_string(seats_[aiSeat()].elo) + ")";
        tags.timeControl = clock_.timeControl().pgnTag();
        LOGI("Game over: %s (%s)\n%s", resultText_.c_str(), reasonText_.c_str(), game_.pgn(seats_[0].name, seats_[1].name, tags).c_str());
    } else {
        LOGI("Game over: %s (%s)\n%s", resultText_.c_str(), reasonText_.c_str(), game_.pgn(seats_[0].name, seats_[1].name).c_str());
    }
    pendingOffer_ = -1;
    drawOfferPending_ = false;
    clockFrozen_ = false;
    drawOfferBy_ = drawCardFor_ = -1;
    writeGrace_ = 0.0f;
    rateGame();
    // Into the saved games (an aborted online game, a server game, the lesson, the viewer and a
    // replay excepted: archiveGame decides).
    archiveGame(online() || game_.isOver());
    // Both players write the result and lay their pen down before shaking hands (an aborted
    // online game has no result).
    endPending_ = false;
    bool noResult = (online() && og_.status == 4) || (replaying() && replayRecord_.result == "*");
    // The moves still owed (the coach's write limit included) are written before the result.
    scorekeeper_.finishGame(noResult ? std::string() : resultText_);
    audio::playUI(audio::Sfx::GameEnd, 0.7f);
    if (coach()) coachGameOver();  // the coach's closing words, then the handshake (simulate)
}

void GameScene::rateGame() {
    // A game with the coach is never rated.
    if (rated_ || watching() || online() || coach() || game_.status() == GameStatus::Ongoing) return;
    rated_ = true;
    if (hotSeat()) {
        rateHotSeat();  // never the rating against Stockfish
        return;
    }
    Settings& s = settings();
    eloBefore_ = eloAfter_ = s.playerElo;
    // As in FIDE rating, a game counts once both players have made a move (a game abandoned or
    // lost on time before that is not rated). Without Stockfish the opponent plays random moves.
    if (game_.moves().size() < 2 || !engineOk_) {
        LOGI("Elo: game not rated (%d plies%s)", int(game_.moves().size()), engineOk_ ? "" : ", no engine");
        return;
    }
    GameStatus st = game_.status();
    double score = st == GameStatus::Draw ? 0.5 : ((st == GameStatus::WhiteWins) == (humanColor_ == White) ? 1.0 : 0.0);
    elo::Record r = s.playerRecord();
    int opponent = seats_[aiSeat()].elo;
    elo::Change c = elo::applyResult(r, opponent, score);
    s.setPlayerRecord(r);
    s.save();
    eloCounted_ = true;
    eloBefore_ = c.before;
    eloAfter_ = c.after;
    LOGI("Elo: %d -> %d (%+d; score %.1f against %d, expected %.2f, K %d%s)", c.before, c.after, c.delta(), score, opponent,
         c.expected, c.k, r.rated ? "" : ", unrated phase");
}

void GameScene::archiveGame(bool finished) {
    // Screenshot runs leave the player's saved games alone.
    if (archived_ || ctx_->screenshotMode) return;
    archived_ = true;
    const archive::Mode mode = saving::archiveMode(mode_, directMatch_);
    // A direct match: the authority's moves, times and ending (the local game may lag behind).
    saving::DirectRecord direct;
    const Game* game = &game_;
    archive::GameInfo info;
    if (mode == archive::Mode::Direct) {
        if (!saving::directMatchRecord(og_, direct)) {
            LOGI("saved games: the direct match is not saved (aborted)");
            return;
        }
        game = &direct.game;
        info = direct.info;
        finished = direct.finished;
    }
    const int plies = int(game->moves().size());
    if (!archive::shouldSave(mode, coach() ? coachLevel_ : -1, plies, finished, settings().saveGames)) {
        LOGI("saved games: not saved (mode %s, %d plies%s%s)", archive::modeName(mode),
             plies, finished ? ", over" : "", settings().saveGames ? "" : ", saving off");
        return;
    }
    info.mode = mode;
    info.white = seats_[0].name;
    info.black = seats_[1].name;
    // Ratings: Stockfish's and the coach's level rating; the player's in a game that counts for
    // it (against Stockfish, a rated hot-seat game); none in a direct match or for the player of
    // a coach game.
    for (int i = 0; i < 2; ++i) {
        const Seat& st = seats_[i];
        bool shown = st.controller == Controller::Stockfish || (mode == archive::Mode::Play && engineOk_) ||
                     (mode == archive::Mode::HotSeat && hsPlayers_.rated);
        (i == 0 ? info.whiteElo : info.blackElo) = shown && mode != archive::Mode::Direct ? st.elo : 0;
    }
    info.started = gameStartedAt_;
    if (mode != archive::Mode::Direct) {
        info.timeControl = clock_.timeControl().pgnTag();
        info.elapsedMs = moveElapsedMs_;
        info.clockMs = moveClockMs_;
    }
    if (mode == archive::Mode::Coach) info.coachLevel = coachLevel_;
    chess::pgn::Record rec = archive::makeRecord(*game, info);
    archive::SaveResult r = archive::save(library_.folder, rec, gameStartedAt_);
    if (r.ok) LOGI("saved game: %s", r.path.c_str());
    else LOGW("game not saved: %s", r.error.c_str());
}

ui::GameOverExtras GameScene::gameOverExtras() const {
    if (online()) return onlineGameOverExtras();
    if (hotSeat()) return hotSeatGameOverExtras();
    ui::GameOverExtras x;
    if (coach()) {
        if (lesson()) {
            x.line = i18n::tr("coach.lesson.done.line");
            x.primaryLabel = i18n::tr("coach.lesson.next");
        } else {
            x.detail = i18n::tr("coach.gameover.unrated");
            x.primaryLabel = i18n::tr("coach.gameover.again");
        }
        return x;
    }
    if (replaying()) return replayGameOverExtras();
    if (watching()) {
        int moveNo = std::max(1, int(game_.moves().size() + 1) / 2);
        GameStatus st = game_.status();
        const char* key = st == GameStatus::WhiteWins   ? "viewer.gameover.white_wins"
                          : st == GameStatus::BlackWins ? "viewer.gameover.black_wins"
                                                        : "viewer.gameover.draw";
        x.line = i18n::trf(key, {std::to_string(moveNo)});
        auto label = [this](int i) { return ui::presetName(seats_[i].presetName) + " (" + std::to_string(seats_[i].elo) + ")"; };
        x.detail = i18n::trf("viewer.gameover.players", {label(0), label(1)});
        x.primaryLabel = i18n::tr("viewer.watch_again");
        return x;
    }
    if (eloCounted_) {
        std::string delta = signedDelta(eloAfter_ - eloBefore_);
        x.detail = i18n::trf("elo.change", {std::to_string(eloBefore_), std::to_string(eloAfter_), i18n::ltr(delta)});
    } else if (rated_) {
        x.detail = i18n::trf("elo.unrated", {std::to_string(eloBefore_)});
    }
    return x;
}

void GameScene::applySettings(bool displayToo) {
    Settings& s = settings();
    if (ctx_ && ctx_->renderer) {
        ctx_->renderer->setSettings(s.renderSettings());
        PostSettings& ps = ctx_->renderer->post().settings;
        // A seated player's eyes: gentle depth of field, only far objects soften (the exposure and
        // the f-number are set each frame by render()).
        applyDofPreset(ps, s.depthOfField ? DofPreset::Subtle : DofPreset::Off);
        ps.dofMaxRadius = 8.0f;
    }
    audio::setMasterVolume(s.masterVolume);
    audio::setEffectsVolume(s.effectsVolume);
    audio::setAmbienceVolume(s.ambienceVolume);
    audio::setAmbienceEnabled(s.ambience);
    audio::setVoiceVolume(s.voiceVolume);
    // Board coordinates: the option, and always for the rules lesson and the first coach levels.
    world_.setBoardCoordinates(s.showCoordinates || (coach() && coachLevel_ <= 2 && state_ != State::Menu));
    if (displayToo) {
        plat::setDisplayMode(s.fullscreen ? plat::DisplayMode::Borderless : plat::DisplayMode::Windowed, s.displayWidth,
                             s.displayHeight);
        plat::setVsync(s.vsync);
    }
    refreshCoachVoice();   // Options > Audio > Coach voice
}

void GameScene::shutdown(AppContext& ctx) {
    // Closing the window during a game leaves it the way the menu does: an online game is resigned
    // (aborted before my first move), a game against Stockfish resigned and rated (screenshot runs
    // excepted: they stop wherever the capture happens).
    if (!ctx.screenshotMode && online() && link_ && state_ == State::Playing && og_.status == 0) {
        leaveOngoingOnlineGame();
    } else if (!ctx.screenshotMode && !watching() && !hotSeat() && !coach() && state_ == State::Playing &&
               game_.status() == GameStatus::Ongoing) {
        game_.resign(humanColor_);
        rateGame();
    }
    // The game being played goes to the saved games as it stands (resigned above; a hot-seat or
    // coach game unfinished; nothing for a game closed before its first move, its resignation
    // included). Screenshot runs stop wherever the capture happens: not saved then.
    if (!ctx.screenshotMode && (state_ == State::Playing || state_ == State::Intro || state_ == State::Handshake))
        archiveGame(game_.isOver() && !game_.moves().empty());
    if (onlineSession().directActive()) onlineSession().closeDirect();
    if (osCursorHidden_) plat::setCursorVisible(true);
    osCursorHidden_ = false;
    shutdownCoach();  // the voice worker and the coach's analyses, before the engine and the audio
    coachModelShutdown();   // a voice model download in progress stops (its .part files stay)
    engine_.shutdown();
    scorekeeper_.shutdown();
    ui::shutdown();
    audio::shutdown();
}

// =============================================================================================
// Frame update
// =============================================================================================

bool GameScene::update(AppContext& ctx, float dt) {
    if (!warpDone_) {
        warpDone_ = true;
        float warp = float(std::atof(ctx.argValue("--warp", "0").c_str()));
        if (warp > 0.0f && state_ != State::Loading) runWarp(warp);
    } else if (warpLeft_ > 0.0f && scriptMenu_ == ui::MenuAction::None) {
        runWarp(warpLeft_);   // the rest of a warp that stopped for a --play-then menu choice
    }
    const plat::Input& in = plat::input();
    ui::gfx::setTextScale(settings().textScale);
#ifdef __ANDROID__
    android_plat::setTouchDirect(settings().touchDirect);
#endif
    ui::beginFrame(plat::width(), plat::height(), dt);
    bool keepRunning = true;
    // Hot-seat: nobody acts during the handover, and buttons still held by the previous player
    // are ignored until released (in every state: a game can end while the view goes over).
    if (hotSeat()) inputBlocked_ = handover_.active() ? true : inputGate_.blocked(anyInputHeld());

    switch (state_) {
    case State::Loading:
        ui::loadingScreen(world_.loadProgress(), world_.loadLabel());
        if (world_.loadStep()) finishLoading();
        break;
    case State::Menu: {
        fade_ = std::max(0.0f, fade_ - dt / kFadeIn);
        coachSetup_.voiceAvailable = coachVoiceExpected();
        ui::MenuAction a = ui::mainMenu(setup_, watch_, coachSetup_, library_);
        if (a == ui::MenuAction::StartReplay) {
            // A saved game chosen on the "Saved games" page (it loaded there already: a failure
            // here means the file changed meanwhile).
            if (loadReplay(library_.replay.path, library_.replay.game)) {
                mode_ = GameMode::Replay;
                state_ = State::FadeToGame;
                stateTime_ = 0.0f;
            } else {
                ui::openSavedGames();
            }
        } else if (a == ui::MenuAction::StartCoach) {
            // The Coach page saved its choice in the .ini ([coach]): the level and the colour come
            // from there (the command line's forced ones no longer apply).
            mode_ = GameMode::Coach;
            coachArgs_.level = coachArgs_.colour = -1;
            state_ = State::FadeToGame;
            stateTime_ = 0.0f;
        } else if (a == ui::MenuAction::StartWatching) {
            // The page saved the choice in the .ini ([viewer]); watch_ holds it.
            mode_ = GameMode::Watch;
            state_ = State::FadeToGame;
            stateTime_ = 0.0f;
        } else if (a == ui::MenuAction::StartGame) {
            // Against Stockfish, or two players on this PC (the page saved its choices).
            mode_ = setup_.opponent == 1 ? GameMode::HotSeat : GameMode::Play;
            state_ = State::FadeToGame;
            stateTime_ = 0.0f;
        } else if (a == ui::MenuAction::Quit) {
            keepRunning = false;
        } else if (a == ui::MenuAction::OptionsChanged) {
            applySettings(true);
        }
        // An online game was found (matchmaking, challenge, direct match): to the table.
        if (state_ == State::Menu && onlineSession().gameReady()) {
            mode_ = GameMode::Online;
            state_ = State::FadeToGame;
            stateTime_ = 0.0f;
        }
        break;
    }
    case State::Playing: {
        if (watching()) {
            updateWatchInput();
            break;
        }
        if (online()) {
            updateOnlineInput();
            break;
        }
        if (coach()) {
            updateCoachInput();
            break;
        }
        // Esc opens the pause menu; once open, the menu handles Esc itself (back / resume).
        if (!paused_ && in.keyPressed[plat::KEY_ESCAPE] && turn_ != Turn::HumanPromotion) {
            paused_ = true;
            if (dragging_) {
                dragging_ = false;
                plat::setMouseCaptured(false);
            }
        }
        if (paused_) {
            // Not while a hand carries out a move, which the end of the game would cut off (turn.h).
            const bool mayEnd = menuMayEndGame(turn_);
            bool canClaim = mayEnd && (game_.canClaimThreefold() || game_.canClaimFiftyMove());
            bool canOffer = drawOfferPly_ != int(game_.moves().size());
            // Hot-seat: the menu belongs to the player to move (resignation named, offer with the move).
            std::string resignQuestion;
            if (hotSeat()) {
                canOffer = canOffer && drawOfferBy_ < 0 && drawCardFor_ < 0;
                resignQuestion = i18n::trf("hotseat.confirm.resign", {seats_[inputSeat()].name, seats_[1 - inputSeat()].name});
            } else {
                // Answered at once, so not with a move on its way, except my move made on the board
                // and waiting for the clock press: the offer goes with it (FIDE 9.1.2).
                canOffer = canOffer && !drawOfferPending_ && mayEnd;
            }
            switch (menuChoice(ui::pauseMenu(canClaim, canOffer, resignQuestion, mayEnd))) {
            case ui::MenuAction::Resume: paused_ = false; break;
            case ui::MenuAction::Resign:
                paused_ = false;
                game_.resign(inputColor());
                endGame();
                break;
            case ui::MenuAction::OfferDraw:
                paused_ = false;
                if (hotSeat()) offerDrawHotSeat();
                else if (turn_ == Turn::HumanPlaced) drawOfferPending_ = true;
                else offerDraw();
                break;
            case ui::MenuAction::ClaimDraw:
                paused_ = false;
                game_.claimDraw();
                if (game_.status() != GameStatus::Ongoing) endGame();
                else ui::notify(i18n::tr("notify.no_draw_claim"));
                break;
            case ui::MenuAction::BackToMainMenu:
                paused_ = false;
                // Leaving resigns: the game is rated as a loss. A hot-seat game is abandoned
                // without a result (and never rated).
                if (!hotSeat()) {
                    if (game_.status() == GameStatus::Ongoing) game_.resign(humanColor_);
                    rateGame();
                }
                // Saved as it stands: the resignation, or a hot-seat game unfinished ("*"). Left
                // before any move, the automatic resignation is not a game: nothing is saved.
                archiveGame(game_.isOver() && !game_.moves().empty());
                clock_.stop();
                state_ = State::FadeToMenu;
                stateTime_ = 0.0f;
                break;
            case ui::MenuAction::OptionsChanged: applySettings(true); break;
            default: break;
            }
        } else {
            if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
            if (isHumanTurn() && !(hotSeat() && inputBlocked_)) updateHumanInput();
        }
        break;
    }
    case State::Intro:
    case State::Handshake:
        if (watching()) updateWatchInput();
        break;
    case State::GameOver:
        if (online()) {
            updateOnlineGameOver();
            break;
        }
        if (watching()) updateWatchInput();
        if (coach()) updateCoachGameOver();
        if (coach() ? coachEndCardReady() : stateTime_ > 1.2f && (endHandshakeDone_ || stateTime_ > 5.0f)) {
            gameOverShown_ = true;
            ui::MenuAction a = ui::gameOver(resultText_, reasonText_, playerWon_, isDraw_, int(game_.moves().size() + 1) / 2,
                                            gameOverExtras());
            if (a == ui::MenuAction::Rematch && replaying()) {
                // "Replay again": from the start, at once (the observer stays where it is).
                replayClock_.jumpTo(0);
                replayClock_.resume();
            } else if (a == ui::MenuAction::Rematch) {
                if (hotSeat()) swapHotSeatColours();  // the rematch swaps colours
                if (coach()) {
                    // "Play again": the same level (the Coach page offers the one the coach
                    // suggested); "First game" after the lesson: level 1.
                    if (!lesson()) coachArgs_.level = coachLevel_;
                    leaveCoachGame();
                }
                state_ = State::FadeToGame;
                stateTime_ = 0.0f;
            } else if (a == ui::MenuAction::BackToMainMenu) {
                if (coach()) leaveCoachGame();
                state_ = State::FadeToMenu;
                stateTime_ = 0.0f;
            }
        }
        // The card holds the keyboard even folded ("View the board"): Tab works once it is folded.
        if (in.keyPressed[plat::KEY_TAB] && (gameOverShown_ ? ui::gameOverFolded() : !ui::wantsKeyboard()))
            showMoveList_ = !showMoveList_;
        break;
    default: break;
    }
    if (online() && (state_ == State::Intro || state_ == State::Handshake || state_ == State::Playing || state_ == State::GameOver))
        drawOnlineHud();
    if (hotSeat() && (state_ == State::Intro || state_ == State::Handshake || state_ == State::Playing || state_ == State::GameOver))
        drawHotSeatHud();
    if (!isHumanTurn() || (hotSeat() && inputBlocked_)) {
        // Nobody aims: the opponent's turn, or (hot-seat) the view going over to the next player
        // and the buttons still held by the previous one.
        hoverId_ = -1;
        aimSq_ = NoSquare;
        clockHover_ = false;
    }
    // The game's pointer replaces the system arrow at the table (not over menus and cards).
#ifdef __ANDROID__
    // There is no system arrow to fall back to here (plat::setCursorVisible is a no-op): the
    // drawn arrow is the only pointer, so it stays over the menus, the cards and the pause too --
    // it only goes during the two-finger look, where the view itself is the pointer.
    bool hideArrow = !dragging_;
#else
    bool hideArrow = gameCursorShown() && !ui::wantsMouse();
#endif
    if (hideArrow != osCursorHidden_) {
        plat::setCursorVisible(!hideArrow);
        osCursorHidden_ = hideArrow;
    }

    clockDt_ = ctx.clockDt;
    simulate(dt);
    return keepRunning;
}

void GameScene::runWarp(float seconds) {
    const float step = 1.0f / 60.0f;
    LOGI("warping %.1f s of game time", seconds);
    warpLeft_ = 0.0f;
    for (float t = 0.0f; t < seconds; t += step) {
        // A --play-then choice is made in the Esc menu, which only a frame shows: the warp goes on
        // after that frame (update).
        if (paused_ && scriptMenu_ != ui::MenuAction::None) {
            warpLeft_ = seconds - t;
            break;
        }
        // The engine searches in real time: wait for it so the warp stays deterministic.
        if (state_ == State::Playing && turn_ == Turn::AiThinking && aiRequested_ && !aiHasMove_ && engineOk_) {
            for (int i = 0; i < 6000 && !engine_.moveReady(); ++i) plat::sleepMs(5);
        }
        // A coach game goes on to its end card (closing words, handshake, appraisal).
        if (state_ == State::GameOver && stateTime_ > 1.0f && (!coach() || coachEndCardReady()) &&
            !(replaying() && replayKeysPos_ < replayKeys_.size()))
            break;
        clockDt_ = step;
        simulate(step);
    }
}

void GameScene::simulate(float dt) {
    timeSum_ += dt;
    time_ = float(timeSum_);
    stateTime_ += dt;
    board_.beginFrame();
    updateOnline(dt);

    switch (state_) {
    case State::FadeToGame:
        fade_ = std::min(1.0f, fade_ + dt / kFadeOut);
        if (fade_ >= 1.0f && stateTime_ > kFadeOut + 0.25f) {
            if (online() && !onlineSession().gameReady()) {
                enterMenu();  // the online game went away while the lights were down
                break;
            }
            setupNewGame();
            state_ = skipIntro_ ? State::Handshake : State::Intro;
            stateTime_ = 0.0f;
            if (skipIntro_) fade_ = 0.0f;
        }
        break;
    case State::Intro:
        fade_ = std::max(0.0f, 1.0f - stateTime_ / kFadeIn);
        if (online() && stateTime_ >= kFadeIn * 0.3f) {
            // Online the clock is the server's: the handshake happens while the game begins.
            anim::Task h0 = task(anim::TaskType::Handshake), h1 = h0;
            h0.partner = &anim_[1];
            h1.partner = &anim_[0];
            anim_[0].enqueue(h0);
            anim_[1].enqueue(h1);
            startPlaying();
            break;
        }
        if (stateTime_ >= kFadeIn * 0.75f) {
            // Handshake across the board before the first move.
            anim::Task h0 = task(anim::TaskType::Handshake), h1 = h0;
            h0.partner = &anim_[1];
            h1.partner = &anim_[0];
            anim_[0].enqueue(h0);
            anim_[1].enqueue(h1);
            state_ = State::Handshake;
            stateTime_ = 0.0f;
        }
        break;
    case State::Handshake:
        fade_ = std::max(0.0f, fade_ - dt / kFadeIn);
        if (stateTime_ > 0.3f && !anim_[0].busy() && !anim_[1].busy()) startPlaying();
        break;
    case State::Playing:
        if (!paused_ || online()) updatePlaying(dt);  // an online game goes on behind the menu
        break;
    case State::GameOver:
        // A replay: a step back or a jump from its end card (J, Home, "Replay again") sets the
        // board again and goes on playing.
        if (replaying()) updateReplay(dt);
        if (state_ != State::GameOver) break;
        // The result is written and the pens laid down first (a writing hand may be the right one).
        // The coach shakes hands once its closing words are said (Session::handshakeWanted).
        if (!endHandshakeDone_ && stateTime_ > 0.8f && !anim_[0].busy() && !anim_[1].busy() &&
            !anim_[0].writingBusy() && !anim_[1].writingBusy() && (!coach() || coachHandshakeWanted())) {
            anim::Task h0 = task(anim::TaskType::Handshake), h1 = h0;
            h0.partner = &anim_[1];
            h1.partner = &anim_[0];
            anim_[0].enqueue(h0);
            anim_[1].enqueue(h1);
            endHandshakeDone_ = true;
        }
        break;
    case State::FadeToMenu:
        fade_ = std::min(1.0f, fade_ + dt / kFadeOut);
        if (fade_ >= 1.0f && stateTime_ > kFadeOut + 0.25f) enterMenu();
        break;
    default: break;
    }

    // Clock lever swings quickly to the pressed side.
    float leverSpeed = 1.0f / 0.07f;
    leverSide_ += clamp(leverTarget_ - leverSide_, -leverSpeed * dt, leverSpeed * dt);

    // Characters (frozen while the game is paused).
    bool frozen = paused_ && state_ == State::Playing && !online();
    bool firstPerson = state_ != State::Menu && state_ != State::Loading && state_ != State::FadeToGame;
    updateCamera(dt, firstPerson);  // sets the player's head override before the animation update
    updateGaze(dt);
    // Reading one's own scoresheet (S): the writing hand waits off the page meanwhile (the sheet
    // of the player whose eyes are the view, and online the opponent's while their gestures say so).
    int reader = firstPersonSeat();
    for (int seat = 0; seat < 2; ++seat)
        scorekeeper_.setHandAside(seat, (glance_ && !watching() && firstPerson && seat == reader) ||
                                            (online() && seat == aiSeat() && remoteGlancing_));
    if (!frozen && state_ != State::Loading) {
        for (int seat = 0; seat < 2; ++seat) {
            events_.clear();
            anim_[seat].update(dt, events_);
            handleEvents(seat, events_);
        }
        scorekeeper_.update();
    }
    // Coach mode: the session, the coach's voice and body, its hands on the table.
    if (coach() && state_ != State::Loading && state_ != State::Menu) updateCoach(dt);
    // Pieces in a hand follow it; the others rest where they were put.
    for (PieceObject& p : board_.pieces()) {
        if (!p.held) continue;
        mat4 t;
        if (anim_[0].heldPieceTransform(p.id, t) || anim_[1].heldPieceTransform(p.id, t)) p.transform = t;
    }
    board_.updateRestingTransforms();
    // Hot-seat: the view going over to the next player (their eyes are posed now).
    if (hotSeat()) updateHandover(dt);

    // Viewer: initial camera, once the robots are posed (face viewpoints need their heads), then
    // the observer's frame.
    if (observerView()) {
        if (pendingCamArg_) {
            pendingCamArg_ = false;
            pendingViewpoint_ = -1;
            vec3 p, t;
            parseVec3(ctx_->argValue("--cam"), p);
            if (!parseVec3(ctx_->argValue("--look"), t)) t = vec3(0.0f, layout::BOARD_TOP_Y, 0.0f);
            float fov = float(std::atof(ctx_->argValue("--fov", "0").c_str()));
            observer_.setPose(CameraPose::looking(p, t, fov > 1.0f ? fov * DEG : kFov));
            cameraCut_ = true;
        }
        if (pendingViewpoint_ >= 0) {
            selectViewpoint(pendingViewpoint_, true);
            pendingViewpoint_ = -1;
        }
        updateObserver(dt);
    }
}

bool GameScene::isHumanTurn() const {
    return turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing ||
           turn_ == Turn::HumanPromotion || turn_ == Turn::HumanPlaced || turn_ == Turn::HumanPressing;
}

void GameScene::updatePlaying(float dt) {
    // The handover between two players (hot-seat) freezes the clock between a clock press and the
    // moment the next player can act: nothing counts and nobody acts.
    if (clockFrozen_) return;
    // A replay: the record's pace, its clocks (no clock_, no engine, no input).
    if (replaying()) {
        updateReplay(dt);
        return;
    }
    // Clock (online: the server's, see onlineClockDisplay()): the real time, not the capped dt
    if (clock_.isRunning() && !online()) {
        hotseat::advanceClock(clock_, clockAccumMs_, clockDt_, clockFrozen_);
        plyElapsedMs_ += double(clockDt_) * 1000.0;  // what the clock counted: the move's time (saved games)
        Color r = clock_.running();
        if (!clock_.timeControl().unlimited && clock_.flagged(r)) {
            game_.flagFall(r);
            if (watching()) {
                ui::notify(i18n::tr(r == White ? "viewer.flag.white" : "viewer.flag.black"), 4.0f);
                endGame();
                return;
            }
            if (hotSeat()) ui::notify(i18n::trf("hotseat.flag", {seats_[seatOf(r)].name}), 4.0f);
            else ui::notify(i18n::tr(r == humanColor_ ? "notify.flag_you" : "notify.flag_opponent"), 4.0f);
            endGame();
            return;
        }
    }
    if (hotSeat()) updateHotSeatTurn(dt);
    if (!watching() && !(online() && (resync_ || endPending_))) updateScript(dt);
    switch (turn_) {
    case Turn::HumanPlacing:
        if (dest_.empty() && !anim_[inputSeat()].busy()) {
            if (arbiter_.pendingNeedsPromotion(game_)) {
                turn_ = Turn::HumanPromotion;
            } else {
                turn_ = Turn::HumanPlaced;
                if (untimed()) {
                    // No clock to press: the move is made now that its last piece is released
                    // (the promotion's new piece included) and the hand goes back meanwhile.
                    // The seat is taken first: in a hot-seat game the turn passes in completeMove.
                    int seat = inputSeat();
                    anim_[seat].enqueue(task(anim::TaskType::Retract));
                    completeMove(seat);
                } else if (pressQueued_) {
                    humanPressClock();
                }
            }
        }
        break;
    case Turn::HumanPromotion: {
        if (paused_) break;
        // The piece of a scripted promotion (--play e7e8n), else the picker of the player to move.
        int choice = scriptPromo_ != NoPiece ? int(scriptPromo_) : ui::promotionPicker(inputColor() == White);
        scriptPromo_ = NoPiece;
        if (choice >= Knight && choice <= Queen && online() && promoTo_ != NoSquare) {
            // Online the piece is chosen before the pawn moves: the move goes out complete.
            Square to = promoTo_;
            promoTo_ = NoSquare;
            if (endPending_ || resync_ || og_.status != 0) {   // too late: the game ended, or a resync
                humanRelease();
                break;
            }
            Move mv = game_.position().findLegal(touchedSq_, to, PieceType(choice));
            if (!mv.valid()) break;
            PieceObject* occupant = board_.at(to);
            int moverId = touchedId_;
            placeOnlineMove(mv);
            std::vector<anim::Task> tasks;
            planPlacement(tasks, moverId, to, occupant ? occupant->id : -1, NoSquare, NoSquare);
            planPromotionSwap(tasks, moverId, to, PieceType(choice));
            anim_[inputSeat()].enqueue(tasks);
            placedTo_ = to;
            pressQueued_ = pressQueued_ || autoPressClock();
            turn_ = Turn::HumanPlacing;
            break;
        }
        if (choice >= Knight && choice <= Queen) {
            PieceType t = PieceType(choice);
            arbiter_.choosePromotion(game_, t);
            std::vector<anim::Task> tasks;
            planPromotionSwap(tasks, board_.idAt(placedTo_), placedTo_, t);
            anim_[inputSeat()].enqueue(tasks);
            turn_ = Turn::HumanPlacing;
        }
        break;
    }
    case Turn::AiThinking: updateAi(dt); break;
    case Turn::AiMoving:
        // Untimed: the robot's move is completed once its last piece is released (dest_ holds only
        // its pieces now); its hand goes back meanwhile. Timed: at its clock press.
        if (untimed() && dest_.empty()) completeMove(seatOf(game_.position().sideToMove()));
        break;
    default: break;
    }
}

// =============================================================================================
// Human player
// =============================================================================================

void GameScene::updateHumanInput() {
    const plat::Input& in = plat::input();
    Ray ray = mouseRay();
    float tPiece = 1e30f;
    int pid = pickPiece(ray, &tPiece);
    PieceObject* p = pid >= 0 ? board_.byId(pid) : nullptr;
    const Color me = inputColor();  // the human, or in a hot-seat game the player to move
    // Coach mode: not while the coach's hands are on the table, the takeback card is up, or (the
    // rules lesson) the coach has the floor between two exercises.
    bool ownPiece = p && p->color == me && (!coach() || turn_ != Turn::HumanIdle || coachMayTouch());
    hoverId_ = ownPiece && turn_ == Turn::HumanIdle ? pid : -1;
    float tClock = 1e30f;
    clockHover_ = !untimed() && world_.rayHitsClock(ray, &tClock) && tClock < tPiece;  // untimed: no press
    // While a piece is in hand the pointer designates a square (shown on the board, see markers()).
    bool castling = false;
    aimSq_ = turn_ == Turn::HumanTouched && !clockHover_ ? aimSquare(ray, &castling) : NoSquare;
    aimLegal_ = aimSq_ != NoSquare && legalDestination(aimSq_);
    // A touched piece without a legal move may be let go: pointing at another of your pieces then
    // offers it instead of a square.
    // The rules lesson relaxes touch-move: any piece may be put back.
    bool canSwitch = turn_ == Turn::HumanTouched && ownPiece && pid != touchedId_ && !castling &&
                     (lesson() || !arbiter_.touchedHasLegalMove(game_));
    if (canSwitch) {
        aimSq_ = NoSquare;
        hoverId_ = pid;
    }

    // Space presses the clock (a coach game has none: Space skips what the coach says).
    if (in.keyPressed[plat::KEY_SPACE] && !ui::wantsKeyboard() && !coach()) {
        humanPressClock();
        return;
    }
    // Drag and drop: the piece touched by this press goes to the square where the button is
    // released (a release on its own square keeps it in hand, a click then chooses the square).
    if (pressTouched_ && !in.mouseDown[plat::MOUSE_LEFT]) {
        pressTouched_ = false;
        float moved = length(cursorPixels() - pressPos_);
        if (in.mouseReleased[plat::MOUSE_LEFT] && turn_ == Turn::HumanTouched && !dragging_ && !ui::wantsMouse() &&
            moved > 0.012f * float(std::max(1, plat::height())) && aimSq_ != NoSquare && aimSq_ != touchedSq_) {
            const PieceObject* occupant = board_.at(aimSq_);
            if (castling || !occupant || occupant->color != me) humanPlace(aimSq_);
        }
        return;
    }
    if (!in.mousePressed[plat::MOUSE_LEFT] || ui::wantsMouse() || dragging_) return;

    if (clockHover_) {
        humanPressClock();
        return;
    }

    switch (turn_) {
    case Turn::HumanIdle:
        if (ownPiece) {
            humanTouch(p->id);
            if (turn_ == Turn::HumanTouched) {
                pressTouched_ = true;
                pressPos_ = cursorPixels();
            }
        }
        break;
    case Turn::HumanTouched: {
        PieceObject* touched = board_.byId(touchedId_);
        if (canSwitch) {
            humanRelease();
            humanTouch(p->id);
            break;
        }
        if (castling) {
            humanPlace(aimSq_);
            break;
        }
        const PieceObject* occupant = aimSq_ != NoSquare ? board_.at(aimSq_) : nullptr;
        bool ownSquare = occupant && occupant->color == me && occupant->id != touchedId_;
        if (aimSq_ == touchedSq_) {
            if (lesson() || !arbiter_.touchedHasLegalMove(game_)) {
                humanRelease();
            } else if (touched) {
                ui::notify(i18n::tr(std::string("notify.touched.") + pieceName(touched->type)), 3.0f);
            }
        } else if (ownSquare || (aimSq_ == NoSquare && ownPiece)) {
            if (touched) ui::notify(i18n::tr(std::string("notify.touched.") + pieceName(touched->type)), 3.0f);
        } else if (aimSq_ != NoSquare) {
            humanPlace(aimSq_);
        }
        break;
    }
    case Turn::HumanPlaced:
        if (!untimed()) ui::notify(i18n::tr("notify.press_clock"), 2.5f);
        break;
    default: break;
    }
}

bool GameScene::legalDestination(Square to) const {
    const PieceObject* mover = board_.byId(touchedId_);
    if (!mover || touchedSq_ == NoSquare || to == NoSquare) return false;
    bool promo = mover->type == Pawn && (rankOf(to) == 7 || rankOf(to) == 0);
    return game_.position().findLegal(touchedSq_, to, promo ? Queen : NoPiece).valid();
}

Square GameScene::aimSquare(const Ray& ray, bool* castling) const {
    if (castling) *castling = false;
    if (touchedSq_ == NoSquare) return NoSquare;
    const PieceObject* touched = board_.byId(touchedId_);
    Square under = pickSquare(ray);
    float tPiece = 1e30f;
    int pid = pickPiece(ray, &tPiece);
    const PieceObject* p = pid >= 0 && pid != touchedId_ ? board_.byId(pid) : nullptr;
    if (touched) {
        // Pointing at the piece in hand (gripped on its square) designates its own square.
        const float t = rayHitsPiece(*touched, ray, 0.003f);
        if (t >= 0.0f && (!p || t < tPiece)) return touchedSq_;
    }
    if (p && p->color == inputColor()) {
        // Castling by pointing at the rook once the king is in hand.
        if (touched && touched->type == King && p->type == Rook && rankOf(p->square) == rankOf(touchedSq_)) {
            Square to = makeSquare(fileOf(p->square) > fileOf(touchedSq_) ? 6 : 2, rankOf(touchedSq_));
            if (game_.position().findLegal(touchedSq_, to).valid()) {
                if (castling) *castling = true;
                return to;
            }
        }
        // A piece never goes onto one of its own side: look through them at the square behind,
        // which they often hide from a seated player.
        return under;
    }
    if (p && under != NoSquare && under != p->square && legalHints()) {
        // An opposing piece stands in front of the square under the pointer: take whichever of
        // the two the touched piece can go to (hints shown only, else this would tell).
        if (!legalDestination(p->square) && legalDestination(under)) return under;
    }
    return p ? p->square : under;
}

void GameScene::humanTouch(int pieceId) {
    PieceObject* p = board_.byId(pieceId);
    if (!p || p->square == NoSquare) return;
    if (!arbiter_.touch(game_, p->square)) {
        Square committed = arbiter_.touchedSquare();
        ui::notify(i18n::trf("notify.touched_square", {squareName(committed)}), 3.0f);
        return;
    }
    anim_[inputSeat()].enqueue(task(anim::TaskType::Reach, pieceId));
    touchedId_ = pieceId;
    touchedSq_ = p->square;
    turn_ = Turn::HumanTouched;
    if (coach()) coachPlayerTouched();
    // Hot-seat: touching a piece declines a draw offer, and the recording of the opponent's move
    // waits until after this move (updateHotSeatTurn).
    if (hotSeat() && drawCardFor_ == inputSeat()) answerHotSeatDraw(false);
}

void GameScene::humanRelease() {
    PieceObject* p = board_.byId(touchedId_);
    if (!p) return;
    vec3 pos = board_.squareBase(touchedSq_);
    dest_[p->id].push_back({touchedSq_, pos, false});
    anim_[inputSeat()].enqueue({task(anim::TaskType::Place, p->id, pos), task(anim::TaskType::Retract)});
    // The rules lesson relaxes touch-move (lesson.cpp): a piece put back is released for real.
    if (lesson()) arbiter_.reset(game_);
    else arbiter_.cancelTouch();
    touchedId_ = -1;
    touchedSq_ = NoSquare;
    turn_ = Turn::HumanIdle;
}

void GameScene::humanPlace(Square to) {
    const Position& pos = game_.position();
    PieceObject* mover = board_.byId(touchedId_);
    if (!mover) return;
    PieceObject* occupant = board_.at(to);
    if (occupant && occupant->color == inputColor()) return;
    bool promo = mover->type == Pawn && (rankOf(to) == 7 || rankOf(to) == 0);
    Move mv = pos.findLegal(touchedSq_, to, promo ? Queen : NoPiece);
    if (!mv.valid() && (settings().showLegalMoves || online() || coach())) {
        // Online there is no arbiter penalty: the piece cannot be released on an illegal square.
        // Nor with the coach, who explains instead (the rules lesson says why, it has no notice).
        if (!lesson()) ui::notify(i18n::tr("notify.illegal"), 2.0f);
        if (coach()) coachIllegalAttempt(touchedSq_, to);
        return;
    }
    if (online() && promo) {
        promoTo_ = to;  // the new piece is chosen first, then the move is sent and played
        turn_ = Turn::HumanPromotion;
        return;
    }
    if (!online() && !arbiter_.place(game_, to, NoPiece)) {
        ui::notify(i18n::tr("notify.cannot_move"), 2.0f);
        return;
    }
    int victimId = occupant ? occupant->id : -1;
    Square rookFrom = NoSquare, rookTo = NoSquare;
    if (mv.valid()) {
        if (mv.flags & MoveEnPassant) victimId = board_.idAt(Square(to + (inputColor() == White ? -8 : 8)));
        if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
            int rank = rankOf(touchedSq_);
            bool king = (mv.flags & MoveCastleKing) != 0;
            rookFrom = makeSquare(king ? 7 : 0, rank);
            rookTo = makeSquare(king ? 5 : 3, rank);
        }
    }
    // Online the move goes to the authority now, before the hand moves, when the robots press the
    // clock by themselves; otherwise it waits on the board for the player's press.
    if (online()) placeOnlineMove(mv);
    std::vector<anim::Task> tasks;
    planPlacement(tasks, mover->id, to, victimId, rookFrom, rookTo);
    anim_[inputSeat()].enqueue(tasks);
    placedTo_ = to;
    turn_ = Turn::HumanPlacing;
    // Auto-press: the robot presses once the placement (capture, castling rook) is done, or the
    // promotion swap after the piece is chosen, whatever the legality of the placement when the
    // hints are off (the arbiter's verdict comes at the press, as for a press by hand).
    if (autoPressClock()) pressQueued_ = true;
}

bool GameScene::autoPressClock() const { return online() ? og_.autoPress : settings().autoPressClock; }

void GameScene::humanPressClock() {
    // Untimed: nothing to press (Space, a click on the clock, --play); the move completes itself.
    if (untimed()) return;
    if (turn_ == Turn::HumanPlacing || turn_ == Turn::HumanPromotion) {
        // Pressed as soon as the pieces are down: after the promotion swap, and online, where the
        // new piece is chosen before the pawn moves, once the move it completes is placed.
        pressQueued_ = true;
        return;
    }
    if (turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched) {
        ui::notify(i18n::tr("notify.move_first"), 2.0f);
        return;
    }
    if (turn_ != Turn::HumanPlaced) return;
    int seat = inputSeat();
    int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
    anim_[seat].enqueue({task(anim::TaskType::PressClock, -1, world_.clockPressPoint(half)), task(anim::TaskType::Retract)});
    pressQueued_ = false;
    turn_ = Turn::HumanPressing;
}

void GameScene::offerDraw() {
    int ply = int(game_.moves().size());
    if (drawOfferPly_ == ply) {
        ui::notify(i18n::tr("notify.draw_already_offered"), 2.5f);
        return;
    }
    drawOfferPly_ = ply;
    bool accept = engineOk_ ? engine_.acceptsDraw(lastAiEval_, ply) : false;
    if (accept) {
        ui::notify(i18n::tr("notify.draw_accepted"), 3.0f);
        game_.agreeDraw();
        endGame();
    } else {
        ui::notify(i18n::tr("notify.draw_declined"), 3.0f);
    }
}

// =============================================================================================
// Opponent (Stockfish)
// =============================================================================================

ai::ClockInfo GameScene::clockInfo() const {
    ai::ClockInfo ci;
    const TimeControl& tc = clock_.timeControl();
    ci.timed = !tc.unlimited;
    ci.whiteMs = clock_.remainingMs(White);
    ci.blackMs = clock_.remainingMs(Black);
    ci.whiteIncMs = ci.blackIncMs = tc.incrementMs;
    // Arm movement + clock press of a typical move (anim::Timing), spent on the AI's clock.
    // Stockfish deducts the overhead of its next 52 moves from the time left (timeman.cpp), which
    // left it no time at all below 78 s (depth-1 moves in bullet and time trouble): scaled down
    // so that about half of the time stays for the search. The clock itself is charged the
    // humanised thinking time, not the search time.
    int64_t budget = clock_.remainingMs(game_.position().sideToMove()) + int64_t(tc.incrementMs) * 49;
    ci.moveOverheadMs = int(std::clamp<int64_t>(budget / 104, 10, 1500));
    return ci;
}

void GameScene::updateAi(float dt) {
    Color side = game_.position().sideToMove();
    int seat = seatOf(side);
    const Position& pos = game_.position();
    if (!aiRequested_ && coach()) {
        // The coach: not while it reviews the player's move or its hands are busy; its teaching
        // repertoire first, else its Stockfish level. It does not strike a thinking pose (its
        // gestures go on) and never claims or offers a draw.
        if (coachHoldsMove()) return;
        aiElapsed_ = 0.0f;
        aiRequested_ = true;
        aiHasMove_ = false;
        aiThinkMs_ = -1;
        Move book;
        if (coachBookMove(book)) {
            aiMove_ = book;
            aiHasMove_ = true;
            aiThinkMs_ = rng_.rangeInt(900, 1700);
            lastAiEval_ = 0;
            LOGI("coach: repertoire move %s", pos.toUCI(book).c_str());
        } else if (engineOk_) {
            engine_.configure(seats_[seat].engine);
            engine_.requestMove(game_.uciMoves(), clockInfo());
        }
        return;
    }
    if (!aiRequested_) {
        if (engineOk_) {
            // One Stockfish for both sides when watching: each search uses its side's settings
            // (the engine clears its hash when they differ from the previous search's).
            engine_.configure(seats_[seat].engine);
            engine_.requestMove(game_.uciMoves(), clockInfo());
        }
        aiElapsed_ = 0.0f;
        aiThinkMs_ = -1;
        aiRequested_ = true;
        aiHasMove_ = false;
        anim_[seat].setThinking(true);
        return;
    }
    aiElapsed_ += dt;
    if (!aiHasMove_) {
        if (engineOk_ && !engine_.moveReady()) return;
        int evalCp = 0;
        std::string uci = engineOk_ ? engine_.takeMove(&evalCp) : std::string();
        lastAiEval_ = evalCp;
        lastEval_[seat] = evalCp;
        hasEval_[seat] = engineOk_;
        aiMove_ = pos.parseUCI(uci);
        if (!aiMove_.valid()) {
            std::vector<Move> legal = pos.legalMoves();
            if (legal.empty()) return;  // game end is detected when the previous move was played
            if (engineOk_) LOGW("engine returned '%s', playing a random move", uci.c_str());
            aiMove_ = legal[size_t(rng_.rangeInt(0, int(legal.size()) - 1))];
        }
        // Human-like thinking time, counted from the request (the search ran concurrently).
        int legalCount = int(pos.legalMoves().size());
        aiThinkMs_ = engineOk_ ? engine_.thinkTimeMs(clockInfo(), int(game_.moves().size()), legalCount, pos.inCheck()) : 900;
        // The coach takes a moment over its move (its searches are short and not humanised).
        if (coach()) aiThinkMs_ = std::max(aiThinkMs_, rng_.rangeInt(700, 1500));
        aiHasMove_ = true;
    }
    if (aiElapsed_ * 1000.0f < float(aiThinkMs_)) return;
    if (coach() && coachHoldsMove()) return;  // it began to speak meanwhile: the move waits
    Move mv = aiMove_;
    int evalCp = lastAiEval_;

    // The opponent claims a draw by repetition / fifty moves when it is not better.
    if (!coach() && (game_.canClaimThreefold() || game_.canClaimFiftyMove()) && engineOk_ &&
        engine_.acceptsDraw(evalCp, int(game_.moves().size()))) {
        game_.claimDraw();
        if (game_.status() != GameStatus::Ongoing) {
            if (watching()) {
                ui::notify(i18n::trf("viewer.claims_draw", {i18n::tr(sideKey(side))}), 3.0f);
                endGame();
                return;
            }
            ui::notify(i18n::tr("notify.draw_claimed"), 3.0f);
            endGame();
            return;
        }
    }
    // Between two AIs, a draw may be offered along with the move (FIDE 9.1.2: make the move, offer,
    // press the clock); the opponent answers once the clock is pressed.
    int ply = int(game_.moves().size());
    if (watching() && engineOk_ &&
        engine_.offersDraw(evalCp, ply, lastOfferPly_[seat] < 0 ? -1 : ply - lastOfferPly_[seat])) {
        lastOfferPly_[seat] = ply;
        pendingOffer_ = seat;
    }
    playRobotMove(seat, mv);
}

void GameScene::playRobotMove(int seat, const Move& mv) {
    Color side = colorOfSeat(seat);
    arbiter_.touch(game_, mv.from);
    arbiter_.place(game_, mv.to, mv.promotion);
    std::vector<anim::Task> tasks;
    tasks.push_back(task(anim::TaskType::Reach, board_.idAt(mv.from)));
    planMove(tasks, mv, side);
    if (!untimed()) {  // untimed: completed as the last piece is released (updatePlaying)
        int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
        tasks.push_back(task(anim::TaskType::PressClock, -1, world_.clockPressPoint(half)));
    }
    tasks.push_back(task(anim::TaskType::Retract));
    anim_[seat].setThinking(false);
    anim_[seat].enqueue(tasks);
    aiMoveTo_ = mv.to;
    turn_ = Turn::AiMoving;
}

// =============================================================================================
// Physical move planning
// =============================================================================================

vec3 GameScene::jitteredSquare(Square sq) {
    // Players never centre pieces perfectly.
    return board_.squareBase(sq) + vec3(rng_.range(-0.0016f, 0.0016f), 0.0f, rng_.range(-0.0016f, 0.0016f));
}

float GameScene::carryHeight(vec3 from, vec3 to, int ignoreA, int ignoreB) const {
    // Lift just enough to clear the pieces standing along the way.
    vec2 a(from.x, from.z), b(to.x, to.z);
    float top = 0.0f;
    for (const PieceObject& p : board_.pieces()) {
        if (p.id == ignoreA || p.id == ignoreB || p.held || p.square == NoSquare) continue;
        float d = distPointSegment2D(vec2(p.basePos.x, p.basePos.z), a, b);
        if (d < layout::PIECE_BASE_RADIUS[p.type] + 0.024f) top = std::max(top, layout::PIECE_HEIGHT[p.type]);
    }
    return std::max(0.022f, top + 0.014f);
}

void GameScene::planMove(std::vector<anim::Task>& tasks, const Move& mv, Color side, bool lifted) {
    int moverId = board_.idAt(mv.from);
    int victimId = board_.idAt(mv.to);
    if (mv.flags & MoveEnPassant) victimId = board_.idAt(Square(mv.to + (side == White ? -8 : 8)));
    Square rookFrom = NoSquare, rookTo = NoSquare;
    if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
        int rank = rankOf(mv.from);
        bool king = (mv.flags & MoveCastleKing) != 0;
        rookFrom = makeSquare(king ? 7 : 0, rank);
        rookTo = makeSquare(king ? 5 : 3, rank);
    }
    planPlacement(tasks, moverId, mv.to, victimId, rookFrom, rookTo, lifted);
    if (mv.promotion != NoPiece) planPromotionSwap(tasks, moverId, mv.to, mv.promotion);
}

void GameScene::planPlacement(std::vector<anim::Task>& tasks, int moverId, Square to, int victimId, Square rookFrom,
                              Square rookTo, bool lifted) {
    PieceObject* mover = board_.byId(moverId);
    if (!mover) return;
    vec3 toPos = jitteredSquare(to);
    if (!lifted)
        tasks.push_back(task(anim::TaskType::Lift, moverId, vec3(0), carryHeight(mover->basePos, toPos, moverId, victimId)));
    tasks.push_back(task(anim::TaskType::Carry, moverId, toPos));
    PieceObject* victim = board_.byId(victimId);
    if (victim) tasks.push_back(task(anim::TaskType::TakeCaptured, victimId));
    tasks.push_back(task(anim::TaskType::Place, moverId, toPos));
    dest_[moverId].push_back({to, toPos, false});
    if (victim) {
        vec3 slot = board_.nextCaptureSlot(opposite(victim->color), victimId);
        tasks.push_back(task(anim::TaskType::Discard, victimId, slot));
        dest_[victimId].push_back({NoSquare, slot, true});
    }
    if (rookFrom != NoSquare) {
        int rookId = board_.idAt(rookFrom);
        PieceObject* rook = board_.byId(rookId);
        if (rook) {
            vec3 rp = jitteredSquare(rookTo);
            tasks.push_back(task(anim::TaskType::Reach, rookId));
            tasks.push_back(task(anim::TaskType::Lift, rookId, vec3(0), carryHeight(rook->basePos, rp, rookId, moverId)));
            tasks.push_back(task(anim::TaskType::Carry, rookId, rp));
            tasks.push_back(task(anim::TaskType::Place, rookId, rp));
            dest_[rookId].push_back({rookTo, rp, false});
        }
    }
}

void GameScene::planPromotionSwap(std::vector<anim::Task>& tasks, int pawnId, Square sq, PieceType newType) {
    PieceObject* pawn = board_.byId(pawnId);
    if (!pawn) return;
    Color c = pawn->color;
    // The pawn leaves the board, then the new piece (a captured one, or the spare queen) takes
    // its place. The player sets their own pawn down in their half, among the pieces they
    // captured (PhysicalBoard::syncTo follows the same rule).
    vec3 slot = board_.nextCaptureSlot(c, pawnId);
    vec3 sqPos = board_.squareBase(sq);
    tasks.push_back(task(anim::TaskType::Reach, pawnId));
    tasks.push_back(task(anim::TaskType::Lift, pawnId, vec3(0), 0.03f));
    tasks.push_back(task(anim::TaskType::Carry, pawnId, slot));
    tasks.push_back(task(anim::TaskType::Place, pawnId, slot));
    dest_[pawnId].push_back({NoSquare, slot, true});
    int spareId = board_.takeSpare(newType, c);
    PieceObject* spare = board_.byId(spareId);
    vec3 target = jitteredSquare(sq);
    tasks.push_back(task(anim::TaskType::Reach, spareId));
    tasks.push_back(task(anim::TaskType::Lift, spareId, vec3(0), carryHeight(spare->basePos, sqPos, spareId, pawnId)));
    tasks.push_back(task(anim::TaskType::Carry, spareId, target));
    tasks.push_back(task(anim::TaskType::Place, spareId, target));
    dest_[spareId].push_back({sq, target, false});
}

// =============================================================================================
// Animation events
// =============================================================================================

void GameScene::handleEvents(int seat, std::vector<anim::Event>& events) {
    for (const anim::Event& e : events) {
        scorekeeper_.onEvent(seat, e);
        switch (e.type) {
        case anim::EventType::PieceGripped:
        case anim::EventType::CapturedGripped: {
            PieceObject* p = board_.byId(e.pieceId);
            if (!p) break;
            p->held = true;
            if (e.type == anim::EventType::CapturedGripped) p->square = NoSquare;
            if (e.type == anim::EventType::PieceGripped)
                audio::play(audio::Sfx::PiecePickup, p->transform.c[3].xyz(), 0.8f, piecePitch(p->type));
            else
                audio::play(audio::Sfx::CaptureClick, p->transform.c[3].xyz(), 0.9f, piecePitch(p->type));
            break;
        }
        case anim::EventType::PieceReleased:
        case anim::EventType::CapturedReleased: {
            PieceObject* p = board_.byId(e.pieceId);
            if (!p) break;
            Destination d;
            auto it = dest_.find(e.pieceId);
            if (it != dest_.end() && !it->second.empty()) {
                d = it->second.front();
                it->second.erase(it->second.begin());
                if (it->second.empty()) dest_.erase(it);
            } else {
                d.pos = e.position;
                d.captured = e.type == anim::EventType::CapturedReleased;
            }
            float heldYaw = yawOf(p->transform);
            if (d.captured) {
                board_.setCaptured(p->id, d.pos);
                p->yaw = heldYaw;
            } else if (d.reserve) {
                board_.setInReserve(p->id, d.pos);  // the coach's rewind: a promoted piece goes back
                p->yaw = heldYaw;
            } else if (d.square != NoSquare) {
                board_.setOnSquare(p->id, d.square);
                p->basePos = d.pos;
                // Keep the orientation the hand gave it, close to facing the opponent.
                float def = board_.defaultYaw(p->color);
                p->yaw = def + clamp(wrapAngle(heldYaw - def), -0.14f, 0.14f);
            } else {
                p->held = false;
                p->basePos = d.pos;
                p->yaw = heldYaw;
            }
            p->transform = translate(p->basePos) * rotateY(p->yaw);
            bool onTable = d.captured || d.square == NoSquare;
            audio::play(onTable ? audio::Sfx::TablePlace : audio::Sfx::PiecePlace, p->basePos, 0.9f, piecePitch(p->type));
            break;
        }
        case anim::EventType::ClockPressed: onClockPressed(seat); break;
        case anim::EventType::HandshakeClasp:
            if (seat == 0) audio::play(audio::Sfx::Handshake, vec3(0, layout::BOARD_TOP_Y + 0.22f, 0), 0.9f);
            break;
        default: break;
        }
    }
}

void GameScene::onClockPressed(int seat) {
    int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
    audio::play(audio::Sfx::ClockPress, world_.clockPressPoint(half), 1.0f);
    leverTarget_ = half == 1 ? 1.0f : -1.0f;
    if (state_ != State::Playing) return;
    if (online()) {
        // Animation only: the server has the move already, unless the players press the clock
        // themselves in this game: the move staged on the board goes now.
        if (seat == humanSeat() && turn_ == Turn::HumanPressing) {
            pressOnlineClock();
            pressedPly_ = int(game_.moves().size()) - 1;
            if (pendingPly_ < 0) recordOnline(pressedPly_);  // else once confirmed
            beginTurn();
        } else if (seat != humanSeat() && remotePly_ >= 0) {
            recordOnline(remotePly_);
        }
        return;
    }
    completeMove(seat);
}

void GameScene::completeMove(int seat) {
    // Online the authority completes the moves (onClockPressed above, game_scene_online.cpp).
    if (state_ != State::Playing || online()) return;
    Color mover = colorOfSeat(seat);
    // A move waits for its completion: pressing the clock (by hand or the robot's hand), or in an
    // untimed game put down by the human (HumanPlaced) or by the robot (AiMoving, its last piece
    // released).
    bool waiting = turn_ == Turn::HumanPressing || turn_ == Turn::AiMoving || (untimed() && turn_ == Turn::HumanPlaced);
    if (!waiting || game_.position().sideToMove() != mover) return;
    // The arbiter's verdict: an illegal move is completed too (FIDE 7.5.1), in an untimed game as
    // soon as it is made.
    chess::Arbiter::Verdict v = arbiter_.clockPressed(game_, clock_.timeControl());
    if (replaying()) {
        completeReplayMove(seat, v);  // the record's move: no clock, no offers, no end of its own
        return;
    }
    if (v.legal || v.moveStands) {
        // Untimed: nobody sees this clock, it only switches the side whose used time it counts.
        clock_.press(mover);
        if (v.moveStands) {
            // Art. 7.5.2: pawn left unpromoted: penalised, the move stands with a queen.
            ui::notify(v.message, 6.0f);
            clock_.addTime(opposite(mover), v.opponentBonusMs);
        }
        game_.play(v.move);
        // The saved game: the time the move took, the mover's clock after the press (increment
        // and a penalty bonus included), one entry per move of game_.
        moveElapsedMs_.push_back(int64_t(plyElapsedMs_));
        moveClockMs_.push_back(untimed() ? -1 : clock_.remainingMs(mover));
        if (hotSeat()) {
            // The mover records their move at once (after the opponent's, if that one waited);
            // the next player records it once the view has reached them (updateHotSeatTurn).
            scorekeeper_.setHold(seat, false);
            scorekeeper_.setHold(1 - seat, true);
        }
        // Both players record the move on their scoresheet (their writing hands, off the clock).
        // Coach mode: the player's move and the coach's reply stay off the sheets until the
        // player's next move, while the coach may still take them back; the rules lesson records
        // nothing.
        int ply = int(game_.moves().size()) - 1;
        if (coach() && !lesson() && mover == humanColor_) scorekeeper_.setWriteLimit(ply);
        if (!lesson()) scorekeeper_.recordMove(ply, game_.sanMoves().back());
        LOGI("move %d: %s (%s, clocks %lld / %lld ms)", int(game_.moves().size()), game_.sanMoves().back().c_str(),
             mover == White ? "White" : "Black", (long long)clock_.remainingMs(White), (long long)clock_.remainingMs(Black));
        if (v.moveStands) board_.syncTo(game_.position());
        // ---- Coach hook (move completed) ----
        // game_ holds the move and the scoresheets have it; the game may be over. A coach reacts
        // here (check, mate, opening, review of the move), before the end of the game and before
        // the turn passes.
        if (coach()) {
            coachMoveCompleted();  // the session hears of it, then the end of the game or the turn
            return;
        }
        if (game_.status() != GameStatus::Ongoing) {
            endGame();
            return;
        }
        // No announcement of checks: players don't say "check" in tournaments, the arbiter stays
        // quiet.
        if (pendingOffer_ == seat) {
            pendingOffer_ = -1;
            answerAiDrawOffer(seat);
            if (game_.status() != GameStatus::Ongoing) return;
        }
        // My draw offer made with this move: the opponent answers now that the clock is pressed.
        if (drawOfferPending_) {
            drawOfferPending_ = false;
            offerDraw();
            if (game_.status() != GameStatus::Ongoing) return;
        }
        // Hot-seat: a draw offered with this move (FIDE 9.1.2) is put to the opponent once the view
        // has reached them.
        if (hotSeat() && drawOfferBy_ == seat) {
            drawCardFor_ = 1 - seat;
            drawOfferBy_ = -1;
        }
        beginTurn();
        if (hotSeat()) {
            startHandover(seat);
            return;
        }
        followEyesAfterMove(seat);
        return;
    }
    // Illegal move completed: the arbiter restores the position and applies the penalty. A draw
    // offered with it goes with it.
    drawOfferPending_ = false;
    ui::notify(v.message.empty() ? std::string(i18n::tr("notify.illegal")) : v.message, 6.0f);
    if (v.forfeit) {
        game_.forfeitIllegal(mover);
        endGame();
        return;
    }
    // The clock was not switched: the offender's time keeps running while the position is
    // restored, and the lever goes back (untimed: it never moved).
    clock_.addTime(opposite(mover), v.opponentBonusMs);
    board_.syncTo(game_.position());
    if (!untimed()) leverTarget_ = -leverTarget_;
    // Still the offender's turn: the time their move takes (saved games) goes on from there.
    const double spent = plyElapsedMs_;
    beginTurn();
    plyElapsedMs_ = spent;
}

void GameScene::answerAiDrawOffer(int offeringSeat) {
    // The other AI judges the offer on its own last evaluation (its point of view).
    int other = 1 - offeringSeat;
    int ply = int(game_.moves().size());
    bool accept = engineOk_ && hasEval_[other] && engine_.acceptsDraw(lastEval_[other], ply);
    std::string offering = i18n::tr(sideKey(colorOfSeat(offeringSeat)));
    std::string answering = i18n::tr(sideKey(colorOfSeat(other)));
    LOGI("%s offers a draw (%d cp), %s %s (%d cp)", offering.c_str(), lastEval_[offeringSeat], answering.c_str(),
         accept ? "accepts" : "declines", lastEval_[other]);
    if (accept) {
        ui::notify(i18n::trf("viewer.offer_accepted", {offering, answering}), 4.0f);
        game_.agreeDraw();
        endGame();
    } else {
        ui::notify(i18n::trf("viewer.offer_declined", {offering, answering}), 3.0f);
    }
}

// =============================================================================================
// Camera, gaze, picking
// =============================================================================================

vec2 GameScene::cursorPixels() const {
    if (mouseOverride_)
        return vec2(mouseOverridePos_.x * float(std::max(1, plat::width())), mouseOverridePos_.y * float(std::max(1, plat::height())));
    const plat::Input& in = plat::input();
    return vec2(in.mouseX, in.mouseY);
}

Ray GameScene::mouseRay() const {
    vec2 c = cursorPixels();
    return camera_.screenRay(c.x, c.y, std::max(1, plat::width()), std::max(1, plat::height()));
}

float GameScene::rayHitsPiece(const PieceObject& p, const Ray& ray, float margin, float maxT) const {
    const PieceSilhouette* outline = world_.pieceSilhouette(p.type);
    if (!outline) {
        const float t = rayCylinderY(ray, p.basePos, layout::PIECE_BASE_RADIUS[p.type] * 1.12f, layout::PIECE_HEIGHT[p.type]);
        return t <= maxT ? t : -1.0f;
    }
    // Into the object space of the piece standing at its resting place (distances are kept).
    const mat4 inv = rotateY(-p.yaw) * translate(-p.basePos);
    return outline->intersect(transformPoint(inv, ray.o), transformDir(inv, ray.d), margin, maxT);
}

int GameScene::pickPiece(const Ray& ray, float* tOut) const {
    // The piece whose outline the ray meets first: the one the pointer is on, not one standing in
    // front of it or behind it. Off every outline, the nearest piece whose outline grown by 3 mm
    // the ray meets (a thin neck or a crown stays easy to catch).
    constexpr float kNear = 0.003f;
    int best = -1, nearBest = -1;
    float bestT = 1e30f, nearT = 1e30f;
    for (const PieceObject& p : board_.pieces()) {
        if (p.held || p.square == NoSquare) continue;
        float t = rayHitsPiece(p, ray, 0.0f, bestT);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = p.id;
        }
        if (best < 0) {
            t = rayHitsPiece(p, ray, kNear, nearT);
            if (t >= 0.0f && t < nearT) {
                nearT = t;
                nearBest = p.id;
            }
        }
    }
    if (best < 0 && nearBest >= 0) {
        best = nearBest;
        bestT = nearT;
    }
    if (tOut) *tOut = bestT;
    return best;
}

Square GameScene::pickSquare(const Ray& ray) const {
    float t = rayPlane(ray, vec3(0, layout::BOARD_TOP_Y, 0), vec3(0, 1, 0));
    if (t < 0.0f) return NoSquare;
    vec3 p = ray.o + ray.d * t;
    int file = int(std::floor(p.x / layout::SQUARE_SIZE + 4.0f));
    int rank = int(std::floor(4.0f - p.z / layout::SQUARE_SIZE));
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return NoSquare;
    return makeSquare(file, rank);
}

void GameScene::updateCamera(float dt, bool firstPerson) {
    // The viewer's observer camera moves after the animation (simulate()): it can look through a
    // robot's eyes.
    if (observerView()) return;
    const plat::Input& in = plat::input();
    Settings& s = settings();
    camera_.fovY = kFov;
    camera_.nearZ = 0.02f;
    if (!firstPerson) glance_ = false;
    if (!firstPerson) {
        // Title screen: slow cinematic drift around the table.
        menuAngle_ += dt * 0.035f;
        float a = menuAngle_;
        vec3 eye(std::sin(a) * 2.7f, 1.45f + 0.08f * std::sin(time_ * 0.21f), std::cos(a) * 2.7f);
        camera_.position = eye;
        camera_.lookAt(vec3(0.0f, 0.92f, 0.0f));
        return;
    }
    // The seat whose head follows the first-person look: the human, or in a hot-seat game the
    // player at the table (during a flight, the next player's head turns to their view).
    int seat = firstPersonSeat();
    if (seat < 0) return;
    Look& L = look_[seat];
    // Hot-seat: nobody looks around during the handover or with a button held from the previous turn.
    bool handingOver = hotSeat() && handover_.active();
    bool canLook = (state_ == State::Playing || state_ == State::Intro || state_ == State::Handshake || state_ == State::GameOver) &&
                   !handingOver && !(hotSeat() && inputBlocked_);
    // The game over card stops the look until it is folded ("View the board"); it keeps the
    // keyboard even folded.
    bool cardUp = state_ == State::GameOver && gameOverShown_ && !ui::gameOverFolded();
    bool uiBlocks = paused_ || cardUp || turn_ == Turn::HumanPromotion;
    bool keys = state_ == State::GameOver && gameOverShown_ ? ui::gameOverFolded() : !ui::wantsKeyboard();
    if (canLook && !uiBlocks) {
        if (in.mouseDown[plat::MOUSE_RIGHT] && !dragging_ && !ui::wantsMouse()) {
            dragging_ = true;
            plat::setMouseCaptured(true);
            // The look goes on from the height the pointer at the top of the window gave it.
            L.pitch += L.lookUpLift;
            L.lookUpLift = 0.0f;
            L.lookUpArmed = false;
        }
        if (in.mousePressed[plat::MOUSE_MIDDLE] || (in.keyPressed['C'] && keys)) {
            L.yaw = L.pitch = 0.0f;
            L.lookUpArmed = false;
            glance_ = false;
        }
        if (!ui::wantsMouse()) L.lean = clamp(L.lean + in.wheel * 0.2f, 0.0f, 1.0f);
        // S: a look at your own scoresheet, out of sight on the table beside you, and back.
        if (!watching() && in.keyPressed['S'] && keys) glance_ = !glance_;
    }
    if (dragging_ && glanceBlend_ > 0.0f) {
        // Looking around from the scoresheet starts from where the eyes are.
        L.yaw = L.gazeYaw;
        L.pitch = L.gazePitch - kBaseGazePitch;
        glance_ = false;
        glanceBlend_ = 0.0f;
    }
    if (dragging_ && !in.mouseDown[plat::MOUSE_RIGHT]) {
        dragging_ = false;
        plat::setMouseCaptured(false);
        L.lookUpArmed = false;  // the pointer comes back where the press was, maybe at the top
    }
    if (dragging_) {
        float k = 0.0022f * s.mouseSensitivity;
        L.yaw -= in.mouseDX * k;
        L.pitch -= in.mouseDY * k * (s.invertLook ? -1.0f : 1.0f);
    }
    L.yaw = clamp(L.yaw, -1.45f, 1.45f);
    L.pitch = clamp(L.pitch, -1.1f - kBaseGazePitch, 0.75f - kBaseGazePitch);
    // The gaze drifts a little towards the cursor, like eyes following the hand; not while a piece
    // is in hand, when the board must stay still under the pointer that aims at a square, nor
    // during a hot-seat handover. At the top of the window it rises to the opponent's face
    // (look_up.h): once the pointer has been below the band since the look was last reset (a new
    // game, a turn, a look with the right button, C), so that a pointer left there does not lift
    // the view, and not with a menu or a card under the pointer. --mouse (screenshots) arms it.
    float cx = 0.0f, cy = 0.0f, up = 0.0f;
    bool aiming = turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing;
    bool pointer = mouseOverride_ || (in.mouseInWindow && !ctx_->screenshotMode);
    if (!dragging_ && !aiming && !handingOver && pointer && plat::width() > 0) {
        vec2 c = cursorPixels();
        float v = c.y / float(std::max(1, plat::height()));
        cx = clamp(c.x / float(plat::width()) - 0.5f, -0.5f, 0.5f);
        cy = clamp(v - 0.5f, -0.5f, 0.5f);
        if (v > lookUpBandStart(L.pitch) || mouseOverride_) L.lookUpArmed = true;
        if (L.lookUpArmed && canLook && !uiBlocks && !ui::wantsMouse()) up = lookUpWeight(v, L.pitch);
    }
    float targetYaw = L.yaw - cx * 0.11f;
    float basePitch = kBaseGazePitch + L.pitch - cy * 0.08f;
    L.lookUpLift = lookUpLift(up, basePitch, L.leanSmooth);
    // The coach talking to the player: the view rises to its face as the pointer rests (the
    // weight is coachFaceLift_, eased in updateCoach).
    if (coach() && coachFaceLift_ > 0.0f && !dragging_)
        L.lookUpLift = std::max(L.lookUpLift, lookUpLift(smootherstep(coachFaceLift_), basePitch, L.leanSmooth));
    float targetPitch = basePitch + L.lookUpLift;
    glanceBlend_ = clamp(glanceBlend_ + (glance_ ? dt : -dt) / kGlanceTime, 0.0f, 1.0f);
    if (glanceBlend_ > 0.0f) {
        // Eyes on the middle of the scoresheet, from where they are now (the head turns them).
        vec3 eye = anim_[seat].eyeCameraTransform().c[3].xyz();
        vec3 d = glanceTarget() - eye;
        float zs = seat == 0 ? 1.0f : -1.0f;  // the seat faces -Z * zs
        float yaw = std::atan2(-zs * d.x, -zs * d.z);
        float pitch = std::atan2(d.y, length(vec2(d.x, d.z)));
        float b = smootherstep(glanceBlend_);
        targetYaw = lerp(targetYaw, yaw, b);
        targetPitch = lerp(targetPitch, pitch, b);
        camera_.fovY = lerp(kFov, kGlanceFov, b);
    }
    float k = 1.0f - std::exp(-dt * 7.0f);
    L.gazeYaw += (targetYaw - L.gazeYaw) * k;
    L.gazePitch += (targetPitch - L.gazePitch) * k;
    L.leanSmooth += (L.lean - L.leanSmooth) * (1.0f - std::exp(-dt * 5.0f));
    L.headYaw = clamp(L.gazeYaw, -kHeadYawLimit, kHeadYawLimit);
    L.headPitch = clamp(L.gazePitch, kHeadPitchDown, kHeadPitchUp);
    anim_[seat].setHeadOverride(true, L.headYaw, L.headPitch);
}

void GameScene::firstPersonView(int seat, vec3& position, quat& orientation) const {
    const Look& L = look_[seat & 1];
    mat4 e = anim_[seat & 1].eyeCameraTransform();
    quat q = fromMat3(mat3(normalize(e.c[0].xyz()), normalize(e.c[1].xyz()), normalize(e.c[2].xyz())));
    float eyeYaw = clamp(L.gazeYaw - L.headYaw, -kEyeLimit, kEyeLimit);
    float eyePitch = clamp(L.gazePitch - L.headPitch, -kEyeLimit, kEyeLimit);
    q = normalize(q * axisAngle(vec3(0, 1, 0), eyeYaw) * axisAngle(vec3(1, 0, 0), eyePitch));
    if (glanceBlend_ > 0.0f && (seat & 1) == firstPersonSeat()) {
        // Reading the scoresheet beside you, the head tilts a little towards the lines.
        sheet::PadFrame f = sheet::padFrame(seat & 1, world_.clockOnPositiveX());
        vec3 fw = rotate(q, vec3(0, 0, -1)), up = rotate(q, vec3(0, 1, 0)), rt = rotate(q, vec3(1, 0, 0));
        vec3 pageUp = -f.down - fw * dot(-f.down, fw);
        float roll = std::atan2(dot(pageUp, rt), std::max(1e-4f, dot(pageUp, up)));
        roll = clamp(roll * 0.45f, -28.0f * DEG, 28.0f * DEG) * smootherstep(glanceBlend_);
        q = normalize(q * axisAngle(vec3(0, 0, 1), -roll));
    }
    vec3 fwd = rotate(q, vec3(0, 0, -1));
    vec3 flat = normalize(vec3(fwd.x, 0.0f, fwd.z) + vec3(1e-4f, 0, 0));
    position = e.c[3].xyz() + flat * (0.11f * L.leanSmooth) + vec3(0, -0.05f * L.leanSmooth, 0);
    orientation = q;
}

CameraPose GameScene::firstPersonPose(int seat) const {
    vec3 p;
    quat q;
    firstPersonView(seat, p, q);
    return CameraPose::fromOrientation(p, q, kFov);
}

void GameScene::placeFirstPersonCamera() {
    if (hotSeat() && handover_.flying()) {
        // Between the two players' eyes.
        const CameraPose& p = handover_.pose();
        camera_.position = p.position;
        camera_.orientation = p.orientation();
        camera_.fovY = p.fovY;
        return;
    }
    int seat = viewSeat();
    if (seat < 0) return;
    firstPersonView(seat, camera_.position, camera_.orientation);
}

void GameScene::updateGaze(float dt) {
    if (state_ == State::Loading) return;
    gazeTimer_ -= dt;
    // Every robot not driven by a first-person player looks around by itself: the AI in a human
    // game, both players when watching and on the title screen.
    bool menu = state_ == State::Menu || state_ == State::FadeToGame;
    int firstPerson = menu ? -1 : firstPersonSeat();  // that head follows the player's look
    for (int seat = 0; seat < 2; ++seat) {
        if (seat == firstPerson) continue;
        // Online, the opponent's own head turns their robot's while their gestures come.
        if (online() && seat == aiSeat() && driveRemoteHead(dt)) continue;
        int other = 1 - seat;
        vec3 face = anim_[other].eyeCameraTransform().c[3].xyz();
        vec3 target = vec3(0, layout::BOARD_TOP_Y, 0);
        if (state_ == State::Playing) {
            Color stm = game_.position().sideToMove();
            bool myTurn = seatOf(stm) == seat;
            if (myTurn && opponentMoving() && aiMoveTo_ != NoSquare) {
                target = board_.squareBase(aiMoveTo_);
            } else if (myTurn && online() && remoteLive_.pieceId >= 0) {
                target = board_.squareBase(remoteLive_.hover);  // the piece the opponent holds, or where it goes
            } else if (!myTurn && touchedSq_ != NoSquare) {
                target = board_.squareBase(touchedSq_);
            } else if (!myTurn && opponentMoving() && aiMoveTo_ != NoSquare) {
                target = board_.squareBase(aiMoveTo_);  // the other AI's move
            } else {
                target = aiGazeTarget_;
            }
            if (!myTurn && glanceTime_ > 0.0f) target = face;
        } else if (state_ == State::Handshake || state_ == State::Intro || state_ == State::GameOver) {
            target = face;
        } else {
            target = aiGazeTarget_;
        }
        // The coach looks where its words go (the director's Look), and at its hand on the table.
        if (coach() && seat == aiSeat() && state_ != State::Handshake && state_ != State::Intro) coachGazeTarget(target);
        anim_[seat].lookAt(target, 1.0f);
    }
    glanceTime_ -= dt;
    if (gazeTimer_ <= 0.0f) {
        // New point of interest on the board every couple of seconds, and now and then a glance
        // at the opponent.
        int sq = rng_.rangeInt(8, 55);
        aiGazeTarget_ = board_.squareBase(Square(sq)) + vec3(0, 0.02f, 0);
        gazeTimer_ = rng_.range(1.2f, 3.2f);
        if (rng_.uniform() < 0.18f) glanceTime_ = rng_.range(0.8f, 1.6f);
    }
}

std::vector<Marker> GameScene::markers() const {
    std::vector<Marker> out;
    if (state_ != State::Playing || paused_ || (hotSeat() && handover_.active())) return out;
    // The piece under the pointer that a click would take (Idle, or a switch when the touched
    // piece cannot move).
    if ((turn_ == Turn::HumanIdle || turn_ == Turn::HumanTouched) && hoverId_ >= 0) {
        const PieceObject* p = board_.byId(hoverId_);
        if (p && p->square != NoSquare) out.push_back({p->square, 0, 1.0f});
    }
    if (turn_ == Turn::HumanTouched && touchedSq_ != NoSquare) {
        out.push_back({touchedSq_, 1, 1.0f});
        bool hints = legalHints();
        if (hints) {
            const Position& pos = game_.position();
            bool seen[64] = {};
            for (const Move& mv : pos.legalMovesFrom(touchedSq_)) {
                if (seen[mv.to] || mv.to == aimSq_) continue;
                seen[mv.to] = true;
                bool capture = !pos.at(mv.to).empty() || (mv.flags & MoveEnPassant);
                out.push_back({mv.to, capture ? 3 : 2, 1.0f});
            }
        }
        // Where a click would put the piece. With hints, a square it cannot reach stays neutral;
        // without them every square looks the same (nothing tells a legal move).
        if (aimSq_ != NoSquare && aimSq_ != touchedSq_) out.push_back({aimSq_, hints && !aimLegal_ ? 5 : 4, 1.0f});
    }
    return out;
}

bool GameScene::gameCursorShown() const {
    if (!settings().gameCursor || watching() || paused_ || dragging_ || ui::optionsOpen()) return false;
    if (state_ != State::Playing && state_ != State::Intro && state_ != State::Handshake && state_ != State::GameOver) return false;
    if (turn_ == Turn::HumanPromotion) return false;
    return !(state_ == State::GameOver && gameOverShown_ && !ui::gameOverFolded());
}

ui::GameCursor GameScene::gameCursorKind() const {
    // Hot-seat: dimmed until the buttons held by the previous player are released.
    if (state_ != State::Playing || !isHumanTurn() || (hotSeat() && inputBlocked_)) return ui::GameCursor::Waiting;
    switch (turn_) {
    case Turn::HumanIdle: return hoverId_ >= 0 ? ui::GameCursor::Piece : ui::GameCursor::Idle;
    case Turn::HumanTouched:
        if (hoverId_ >= 0) return ui::GameCursor::Piece;
        if (aimSq_ == NoSquare || aimSq_ == touchedSq_) return ui::GameCursor::Holding;
        return legalHints() && !aimLegal_ ? ui::GameCursor::Holding : ui::GameCursor::Square;
    case Turn::HumanPlacing:
    case Turn::HumanPlaced:  // no clock pointer while the press is queued (auto-press), nor untimed
        return clockHover_ && !pressQueued_ && !untimed() ? ui::GameCursor::Clock : ui::GameCursor::Idle;
    default: return ui::GameCursor::Idle;
    }
}

vec3 GameScene::glanceTarget() const {
    // The sheet of the player whose eyes are the view (hot-seat: the player at the table).
    int seat = firstPersonSeat();
    return glanceTarget(seat < 0 ? humanSeat() : seat);
}

vec3 GameScene::glanceTarget(int seat) const {
    sheet::PadFrame f = sheet::padFrame(seat & 1, world_.clockOnPositiveX());
    return f.center + vec3(0.0f, layout::SCORESHEET_THICKNESS, 0.0f);
}

ClockDisplay GameScene::clockDisplay() const {
    if (online() && !link_ && state_ == State::FadeToMenu) {
        ClockDisplay d = leaveClock_;   // the game just left (leaveOnlineGame)
        d.leverSide = leverSide_;
        return d;
    }
    if (online() && link_) return onlineClockDisplay();
    if (replaying() && state_ != State::Menu && state_ != State::Loading) return replayClockDisplay();
    ClockDisplay d;
    int hw = world_.clockHalfForSeat(1.0f), hb = 1 - hw;
    d.ms[hw] = clock_.remainingMs(White);
    d.ms[hb] = clock_.remainingMs(Black);
    d.running = clock_.isRunning() ? (clock_.running() == White ? hw : hb) : -1;
    d.flagged[hw] = clock_.flagged(White);
    d.flagged[hb] = clock_.flagged(Black);
    d.unlimited = clock_.timeControl().unlimited;
    d.paused = paused_ && state_ == State::Playing;
    d.leverSide = leverSide_;
    if (untimed()) {
        // Nobody presses this clock: it shows no time and no side running by itself (clock_ still
        // counts each side's used time, unseen).
        d.dashes = true;
        d.running = -1;
    }
    return d;
}

// =============================================================================================
// Rendering
// =============================================================================================

void GameScene::render(AppContext& ctx, float dt) {
    render::Renderer& r = *ctx.renderer;
    bool firstPerson = !(state_ == State::Menu || state_ == State::Loading || state_ == State::FadeToGame);
    bool observer = observerView();
    if (firstPerson && !observer) placeFirstPersonCamera();

    render::Camera cam = camera_;
    int headless = -1;  // the robot the camera is in: drawn without its head
    if (observer) {
        headless = headNearCamera(cam.position);
    } else if (firstPerson) {
        // In the eyes of the player (hot-seat: of the player at the table); during a hot-seat
        // flight, whichever head the camera passes through.
        headless = hotSeat() && handover_.flying() ? headNearCamera(cam.position) : viewSeat();
        vec3 p, t;
        if (debugCamera_ && parseVec3(ctx.argValue("--cam"), p)) {
            // Detached camera in a human game (screenshots): --cam x,y,z [--look x,y,z] [--fov deg].
            cam.position = p;
            cam.lookAt(parseVec3(ctx.argValue("--look"), t) ? t : vec3(0.0f, layout::BOARD_TOP_Y, 0.0f));
            float fov = float(std::atof(ctx.argValue("--fov", "0").c_str()));
            if (fov > 1.0f) cam.fovY = fov * DEG;
            headless = headNearCamera(cam.position);
        }
    }
    render::Environment env = world_.environment(time_);
    // Eyes focus on what the player looks at: whatever lies under the centre of the view.
    {
        float target;
        bool inEyes = firstPerson && !(hotSeat() && handover_.flying()) && headless == viewSeat();
        if (observer || (firstPerson && !inEyes)) {
            target = observerFocus(cam);
        } else if (firstPerson) {
            // The eyes follow the pointer (they are on the piece or the square being aimed at, or
            // on the scoresheet the pointer rests on); while the player reads his sheet (S) or
            // turns his head, they look straight ahead.
            bool pointer = glanceBlend_ < 0.5f && !dragging_ && !paused_ && !ui::wantsMouse() && state_ == State::Playing;
            target = firstPersonFocus(pointer ? mouseRay() : Ray{cam.position, cam.forward()});
        } else {
            target = length(cam.position - vec3(0, 0.9f, 0));
        }
        focusDistance_ = focusDistance_ <= 0.0f ? target : focusDistance_ + (target - focusDistance_) * (1.0f - std::exp(-dt * 6.0f));
        r.post().settings.dofFocusDistance = focusDistance_;
        // The player's view narrows to read the scoresheet (S), but eyes do not zoom: the depth of
        // field keeps the blur of the normal view instead of a telephoto's (the same f-number at 24
        // degrees would blur 5.5 times more than at 52).
        float zoom = std::tan(kFov * 0.5f) / std::tan(std::max(cam.fovY, 1.0f * DEG) * 0.5f);
        r.post().settings.dofFStop = firstPerson && !observer ? kEyeFStop * zoom * zoom : kEyeFStop;
    }
    if (cameraCut_) {
        r.post().settings.resetHistory = true;
        cameraCut_ = false;
        for (auto& h : hasPrevGlobals_) h = false;
    }
    // Eyes adapt to the page when the player looks at their scoresheet (it lies in the body's
    // shadow, and the sunlit floor around it would keep the exposure low).
    r.post().settings.exposureCompensation = settings().brightness + 0.6f * smootherstep(glanceBlend_);
    r.fade = hotSeat() ? std::max(fade_, handover_.fade()) : fade_;  // a hot-seat cut goes through black
    if (coach()) r.fade = std::max(r.fade, coachFade_);             // a lesson position set up behind a fade
    r.beginFrame(cam, env, dt);
    if (state_ != State::Loading) {
        // Coach mode: the pieces and squares the coach designates (the director's marks).
        std::vector<PieceHighlight> highlights;
        std::vector<CoachMark> coachMarkList;
        if (coach()) coachMarks(highlights, coachMarkList);
        world_.submitStatic(r);
        world_.submitPieces(r, board_, highlights.empty() ? nullptr : &highlights);
        world_.submitClock(r, clockDisplay());
        // Through the player's eyes, the playing arm fades to a see-through ghost while a piece is
        // in hand, so the squares under it stay readable (the piece itself stays opaque). The arm
        // is the carrying player's (hot-seat: the player to move); it fades back on that arm.
        bool ghostArm = !watching() && state_ == State::Playing && headless == inputSeat() &&
                        (turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing || turn_ == Turn::HumanPromotion);
        if (ghostArm) armSeeThroughSeat_ = inputSeat();
        armSeeThrough_ = clamp(armSeeThrough_ + (ghostArm ? dt : -dt) / 0.2f, 0.0f, 1.0f);
        for (int seat = 0; seat < 2; ++seat) {
            const mat4* g = anim_[seat].globals();
            world_.submitRobot(r, seat, g, hasPrevGlobals_[seat] ? prevGlobals_[seat] : nullptr, seat == headless,
                               !watching() && seat == armSeeThroughSeat_ ? armSeeThrough_ : 0.0f, seats_[seat].playHand);
            for (int b = 0; b < character::BoneCount; ++b) prevGlobals_[seat][b] = g[b];
            hasPrevGlobals_[seat] = true;
        }
        world_.submitMarkers(r, markers());
        if (!coachMarkList.empty()) world_.submitCoachMarks(r, coachMarkList);
        scorekeeper_.submit(r);
    }
    r.endFrame();
    audio::setListener(cam.position, cam.forward(), cam.up());
}

void GameScene::renderOverlay(AppContext&, float) {
    bool inGame = state_ == State::Playing || state_ == State::GameOver;
    if (observerView() && !paused_ && state_ != State::FadeToGame && state_ != State::FadeToMenu) {
        ui::ViewerHud hud;
        hud.visible = hudVisible_ && !(gameOverShown_ && !ui::gameOverFolded());
        for (int i = 0; i < 2; ++i) {
            std::string& name = i == 0 ? hud.white : hud.black;
            name = i18n::trf("viewer.player", {ui::presetName(seats_[i].presetName), std::to_string(seats_[i].elo)});
        }
        if (replaying()) {
            // The record's players: name and rating as written in it.
            auto label = [](const std::string& name, int elo) { return elo > 0 ? name + " \xC2\xB7 " + std::to_string(elo) : name; };
            hud.white = label(seats_[0].name, seats_[0].elo);
            hud.black = label(seats_[1].name, seats_[1].elo);
            hud.replay = true;
        }
        hud.sideToMove = state_ == State::Playing ? seatOf(game_.position().sideToMove()) : -1;
        if (viewpointShown_ >= 0) hud.viewpoint = i18n::tr("viewer.view." + std::to_string(viewpointShown_));
        hud.viewpointAge = viewpointAge_;
        hud.speed = observer_.speed();
        hud.speedAge = speedAge_;
        ui::viewerHud(hud);
        if (replaying() && hud.visible) drawReplayBar();
    }
    if (replaying() && replaySheetOffset() && !game_.sanMoves().empty()) {
        // A record that starts with Black to move: its first move in Black's column, as on the sheets.
        std::vector<std::string> san(1, std::string("..."));
        san.insert(san.end(), game_.sanMoves().begin(), game_.sanMoves().end());
        ui::moveList(san, inGame && showMoveList_);
    } else {
        ui::moveList(game_.sanMoves(), inGame && showMoveList_);
    }
    if (coach() && (inGame || state_ == State::Handshake)) drawCoachSubtitles();
    // The coach's voice model: its download prompt, progress panel and notices (coach_model.h).
    drawModelDownload();
    int fetched = 0;
    if (coachModelInstalled(&fetched)) coachModelDownloaded(fetched);   // heard from the coach's next line on
    ui::drawNotifications();
    // No pointer while the view goes over to the other player (hot-seat): the arrow stays hidden.
#ifdef __ANDROID__
    // The drawn arrow is the only pointer on this platform (see the hideArrow note in update()):
    // it is drawn on the menus, the pause and the cards too, and the kind it shows follows the
    // screen the player is on (the Waiting arrow over the menus, not the aiming one).
    if (!ctx_->screenshotMode && !(hotSeat() && handover_.active()))
        ui::gameCursor(cursorPixels(), gameCursorKind());
#else
    if (osCursorHidden_ && !(hotSeat() && handover_.active()) &&
        (mouseOverride_ || (plat::input().mouseInWindow && !ctx_->screenshotMode)))
        ui::gameCursor(cursorPixels(), gameCursorKind());
#endif
    ui::endFrame();
}

// =============================================================================================
// Viewer mode: the observer camera
// =============================================================================================

bool GameScene::observerView() const {
    // From the setup of a watched game until the menu (a "Watch again" fade keeps it).
    return watching() && observerPlaced_ && state_ != State::Loading && state_ != State::Menu;
}

void GameScene::updateWatchInput() {
    const plat::Input& in = plat::input();
    observerControls_ = ObserverCamera::Controls();
    // Esc: the viewer's pause menu (the game stops, the camera can still land).
    if (state_ == State::Playing && !paused_ && in.keyPressed[plat::KEY_ESCAPE] && !ui::wantsKeyboard()) paused_ = true;
    if (paused_) {
        if (dragging_) {
            dragging_ = false;
            plat::setMouseCaptured(false);
        }
        switch (menuChoice(ui::viewerPauseMenu())) {
        case ui::MenuAction::Resume: paused_ = false; break;
        case ui::MenuAction::BackToMainMenu:
            paused_ = false;
            clock_.stop();
            state_ = State::FadeToMenu;
            stateTime_ = 0.0f;
            break;
        case ui::MenuAction::OptionsChanged: applySettings(true); break;
        default: break;
        }
        return;
    }
    // The game over card has the keyboard until it is folded ("View the board").
    bool cardUp = state_ == State::GameOver && gameOverShown_ && !ui::gameOverFolded();
    bool keys = !cardUp && (state_ == State::GameOver || !ui::wantsKeyboard());
    // Right mouse button held: look around.
    if (in.mouseDown[plat::MOUSE_RIGHT] && !dragging_ && !ui::wantsMouse()) {
        dragging_ = true;
        plat::setMouseCaptured(true);
    }
    if (dragging_ && !in.mouseDown[plat::MOUSE_RIGHT]) {
        dragging_ = false;
        plat::setMouseCaptured(false);
    }
    observerControls_ = ObserverCamera::read(in, keys, dragging_);
    if (ui::wantsMouse() && !dragging_) observerControls_.wheel = 0.0f;
    if (!keys) return;
    if (replaying()) updateReplayInput();
    for (int n = 0; n <= 9; ++n) {
        if (in.keyPressed[plat::KEY_0 + n]) selectViewpoint(n, false);
    }
    if (in.keyPressed['H']) {
        hudVisible_ = !hudVisible_;
        settings().viewerShowControls = hudVisible_;
        settings().save();
    }
    // (Tab is handled by the game over state itself.)
    if (state_ != State::GameOver && in.keyPressed[plat::KEY_TAB]) showMoveList_ = !showMoveList_;
}

void GameScene::updateObserver(float dt) {
    const Settings& s = settings();
    ObserverCamera::Controls c = observerControls_;
    observerControls_ = ObserverCamera::Controls();  // once per frame (warps simulate many steps)
    viewpointAge_ += dt;
    speedAge_ += dt;
    if (c.wheel != 0.0f) speedAge_ = 0.0f;
    if (followEyes_ && c.any()) {
        // The observer leaves the player's eyes and flies on from there.
        followEyes_ = false;
        if (clockFrozen_) setClockFrozen(false);
    }
    if (followEyes_) {
        // Steadied eyes: the robots' saccades and small head motions are smoothed out.
        CameraPose e = eyePose(eyesSeat_);
        float k = 1.0f - std::exp(-dt * 5.0f);
        eyesSmooth_.position = e.position;
        eyesSmooth_.yaw += angleDelta(eyesSmooth_.yaw, e.yaw) * k;
        eyesSmooth_.pitch += (e.pitch - eyesSmooth_.pitch) * k;
        eyesSmooth_.roll += angleDelta(eyesSmooth_.roll, e.roll) * k;
        eyesSmooth_.fovY = e.fovY;
        observer_.retarget(eyesSmooth_);
    }
    bool wasFlying = observer_.flying();
    observer_.update(dt, c, s.mouseSensitivity, s.invertLook);
    // Handover preview: the next player's clock starts when the camera is in its eyes.
    if (wasFlying && !observer_.flying() && clockFrozen_) setClockFrozen(false);
    const CameraPose& p = observer_.pose();
    camera_.position = p.position;
    camera_.orientation = p.orientation();
    camera_.fovY = p.fovY;
    camera_.nearZ = 0.02f;
}

CameraPose GameScene::viewpoint(int n) const {
    // The development viewpoints of the former --view option.
    const vec3 board(0.0f, layout::BOARD_TOP_Y, 0.0f);
    switch (n) {
    case 1: return CameraPose::looking(vec3(1.35f, 1.28f, 0.05f), vec3(0.0f, 0.86f, 0.0f), kFov);  // beside the table
    case 2: return CameraPose::looking(vec3(0.0f, 1.55f, 0.35f), board, kFov);                  // above the board
    case 3: return CameraPose::looking(vec3(4.5f, 2.2f, 8.5f), vec3(-2.0f, 1.8f, 0.0f), kFov);    // the hall
    case 4: {                                                                                    // the clock
        vec3 c = transformPoint(world_.clockTransform(), vec3(0, layout::CLOCK_HEIGHT * 0.5f, 0));
        return CameraPose::looking(c + vec3(c.x > 0 ? -0.28f : 0.28f, 0.14f, 0.22f), c, kFov);
    }
    case 5:
    case 6: {  // three-quarter portrait of White's / Black's head
        int seat = n - 5;
        vec3 eye = anim_[seat].eyeCameraTransform().c[3].xyz();
        float f = seat == 0 ? -1.0f : 1.0f;  // White (seat 0) faces -Z
        return CameraPose::looking(eye + vec3(0.24f * f, 0.03f, 0.52f * f), eye + vec3(0.0f, -0.06f, 0.0f), 34.0f * DEG);
    }
    case 7: return CameraPose::looking(vec3(1.75f, 1.32f, 0.0f), vec3(0.0f, 1.02f, 0.0f), 42.0f * DEG);  // the duel
    case 8: return CameraPose::looking(vec3(5.6f, 2.1f, 3.4f), vec3(-2.0f, 2.2f, -0.4f), 55.0f * DEG);   // windows
    case 9: return CameraPose::looking(vec3(-5.2f, 1.9f, -3.6f), vec3(2.5f, 2.0f, 0.8f), 55.0f * DEG);  // tapestries
    default: return eyePose(eyesSeat_ < 0 ? 0 : eyesSeat_);
    }
}

CameraPose GameScene::eyePose(int seat) const {
    mat4 e = anim_[seat & 1].eyeCameraTransform();
    quat q = fromMat3(mat3(normalize(e.c[0].xyz()), normalize(e.c[1].xyz()), normalize(e.c[2].xyz())));
    return CameraPose::fromOrientation(e.c[3].xyz(), q, kFov);
}

void GameScene::selectViewpoint(int n, bool jump) {
    n = std::clamp(n, 0, 9);
    viewpointShown_ = n;
    viewpointAge_ = 0.0f;
    CameraPose to;
    if (n == 0) {
        // Through the eyes of the player to move, following them from one player to the other.
        followEyes_ = true;
        eyesSeat_ = seatOf(game_.position().sideToMove());
        to = eyePose(eyesSeat_);
        eyesSmooth_ = to;
    } else {
        followEyes_ = false;
        if (clockFrozen_) setClockFrozen(false);
        to = viewpoint(n);
    }
    if (jump) {
        observer_.setPose(to);
        cameraCut_ = true;
    } else {
        observer_.flyTo(to);
    }
}

void GameScene::followEyesAfterMove(int seat) {
    if (!watching() || !followEyes_) return;
    int next = 1 - seat;
    CameraPose to = eyePose(next);
    observer_.flyTo(to, CameraFlight::kHandoverDuration,
                    CameraFlight::handoverShape(observer_.pose(), to, vec3(0, layout::BOARD_TOP_Y, 0)));
    eyesSeat_ = next;
    eyesSmooth_ = to;
    if (handoverPreview_) setClockFrozen(true);
}

int GameScene::headNearCamera(vec3 p) const {
    for (int seat = 0; seat < 2; ++seat) {
        mat4 e = anim_[seat].eyeCameraTransform();
        vec3 centre = e.c[3].xyz() + normalize(e.c[2].xyz()) * 0.06f;  // +Z: behind the eyes
        if (length(p - centre) < 0.16f) return seat;
    }
    return -1;
}

float GameScene::firstPersonFocus(const Ray& ray) const {
    // The nearest of what the table holds along the gaze (the opponent's face, a piece, the clock,
    // the board, a scoresheet, the table), else the floor of the hall. A plane through the board
    // alone focused far beyond the table whenever the player looked aside.
    float best = 1e30f;
    auto consider = [&](float t) {
        if (t > 0.05f && t < best) best = t;
    };
    int me = viewSeat();
    int opponent = 1 - (me < 0 ? humanSeat() : me);
    vec3 face = anim_[opponent].eyeCameraTransform().c[3].xyz() - ray.o;
    float faceDist = length(face);
    // Within 12 degrees: the pointer at the top of the window lifts the view to the face and rests
    // just above the head.
    if (faceDist > 0.2f && dot(face / faceDist, ray.d) > std::cos(12.0f * DEG)) consider(faceDist);
    float tPiece = 1e30f;
    if (pickPiece(ray, &tPiece) >= 0) consider(tPiece);
    float tClock = 1e30f;
    if (world_.rayHitsClock(ray, &tClock)) consider(tClock);
    auto rect = [&](float y, float halfX, float halfZ, vec3 centre, vec3 axisX, vec3 axisZ) {
        float t = rayPlane(ray, vec3(0, y, 0), vec3(0, 1, 0));
        if (t <= 0.0f) return;
        vec3 d = ray.o + ray.d * t - centre;
        if (std::abs(dot(d, axisX)) <= halfX && std::abs(dot(d, axisZ)) <= halfZ) consider(t);
    };
    const vec3 X(1, 0, 0), Z(0, 0, 1);
    rect(layout::BOARD_TOP_Y, 0.5f * layout::BOARD_SIZE, 0.5f * layout::BOARD_SIZE, vec3(0), X, Z);
    for (int seat = 0; seat < 2; ++seat) {
        sheet::PadFrame f = sheet::padFrame(seat, world_.clockOnPositiveX());
        rect(layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS, 0.5f * layout::SCORESHEET_WIDTH, 0.5f * layout::SCORESHEET_LENGTH,
             f.center, f.right, f.down);
    }
    rect(layout::TABLE_TOP_Y, 0.5f * layout::TABLE_WIDTH, 0.5f * layout::TABLE_DEPTH, vec3(0), X, Z);
    if (best < 1e29f) return best;
    float tFloor = rayPlane(ray, vec3(0), vec3(0, 1, 0));
    return tFloor > 0.0f ? std::min(tFloor, 8.0f) : 4.0f;
}

float GameScene::observerFocus(const render::Camera& cam) const {
    // A head in the middle of the view (portraits), else the board under the centre of the view,
    // else the table.
    vec3 fwd = cam.forward();
    for (int seat = 0; seat < 2; ++seat) {
        vec3 d = anim_[seat].eyeCameraTransform().c[3].xyz() - cam.position;
        float dist = length(d);
        if (dist > 0.2f && dist < 2.5f && dot(d / dist, fwd) > std::cos(10.0f * DEG)) return dist;
    }
    Ray centre{cam.position, fwd};
    float t = rayPlane(centre, vec3(0, layout::BOARD_TOP_Y, 0), vec3(0, 1, 0));
    if (t > 0.0f && t < 3.0f) {
        vec3 hit = cam.position + fwd * t;
        if (std::abs(hit.x) < 0.9f && std::abs(hit.z) < 0.9f) return t;
    }
    return std::max(0.3f, length(cam.position - vec3(0, 1.0f, 0)));
}

SCACELITH_SCENE("game", "Scacelith: the chess game", GameScene);

}  // namespace game

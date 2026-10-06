// Tests for the coach's end-of-game appraisal (src/coach/appraisal.h): the lichess game-accuracy
// port on lila's own test vectors, the level suggestion, verdict collection (the human's reviews, the
// coach's moves from the neighbouring evaluations, takebacks, truncation, background requests), the
// statistics, and the appraisal scripts of a won, a lost and a drawn game at every level (keys exist,
// placeholders have arguments, anchors are placeholders, sentence caps, every beat skippable).
#include "test.h"

#include "ai/analysis.h"
#include "chess/chess.h"
#include "coach/appraisal.h"
#include "coach/review.h"
#include "core/embedded.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace chess;
using namespace coach;

namespace {

// ---- The English lines (same rules as the review tests) ----------------------------------------

std::string messageKey(const std::string& key) {
    const size_t dot = key.rfind('.');
    if (dot == std::string::npos || dot + 1 >= key.size()) return key;
    for (size_t i = dot + 1; i < key.size(); ++i)
        if (key[i] < '0' || key[i] > '9') return key;
    return std::atoi(key.c_str() + dot + 1) >= 2 ? key.substr(0, dot) : key;
}

const std::map<std::string, std::vector<std::string>>& lines() {
    static const std::map<std::string, std::vector<std::string>> m = [] {
        std::map<std::string, std::vector<std::string>> out;
        for (const char* file : {"assets/coach/speech/en/review.lang", "assets/coach/speech/en/appraisal.lang"}) {
            std::vector<std::pair<std::string, std::string>> entries;
            std::string err;
            const bool ok = i18n::parse(embedded::text(file), entries, &err);
            if (!ok) std::fprintf(stderr, "  %s: %s\n", file, err.c_str());
            CHECK(ok);
            for (const auto& e : entries) out[messageKey(e.first)].push_back(e.second);
        }
        return out;
    }();
    return m;
}

std::set<std::string> placeholders(const std::string& text) {
    std::set<std::string> out;
    size_t p = 0;
    while ((p = text.find('{', p)) != std::string::npos) {
        const size_t q = text.find('}', p);
        if (q == std::string::npos) break;
        std::string name = text.substr(p + 1, q - p - 1);
        const size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(0, colon);
        out.insert(name);
        p = q + 1;
    }
    return out;
}

bool hasKey(const Script& s, const std::string& key) {
    for (const Beat& b : s)
        if (b.line.key == key) return true;
    return false;
}

bool hasPrefix(const Script& s, const std::string& prefix) {
    for (const Beat& b : s)
        if (b.line.key.rfind(prefix, 0) == 0) return true;
    return false;
}

int capOf(int level) { return level <= 2 ? 5 : level <= 4 ? 6 : 4; }

void checkAppraisal(const Script& s, int level, const char* where) {
    const auto& en = lines();
    CHECK(!s.empty());
    CHECK((int)s.size() <= capOf(level));
    for (const Beat& b : s) {
        CHECK(b.kind == BeatKind::Say);
        CHECK(b.skippable);
        auto it = en.find(b.line.key);
        if (it == en.end()) {
            std::fprintf(stderr, "  %s: key %s missing\n", where, b.line.key.c_str());
            CHECK(false);
            continue;
        }
        for (const std::string& text : it->second) {
            const std::set<std::string> ph = placeholders(text);
            for (const std::string& p : ph)
                if (!b.line.arg(p)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": no argument for {%s}\n", where, b.line.key.c_str(),
                                 text.c_str(), p.c_str());
                    CHECK(false);
                }
            for (const Gesture& g : b.gestures)
                if (!g.anchor.empty() && !ph.count(g.anchor)) {
                    std::fprintf(stderr, "  %s: %s: anchor {%s} not in \"%s\"\n", where, b.line.key.c_str(),
                                 g.anchor.c_str(), text.c_str());
                    CHECK(false);
                }
            for (const Mark& m : b.marks)
                if (!m.anchor.empty() && !ph.count(m.anchor)) CHECK(false);
        }
        // Text arguments name keys of the lines (phases, reasons, themes, levels).
        for (const auto& a : b.line.args)
            if (a.second.kind == Arg::Kind::Text && !en.count(a.second.text)) {
                std::fprintf(stderr, "  %s: %s: text key %s missing\n", where, b.line.key.c_str(), a.second.text.c_str());
                CHECK(false);
            }
    }
}

void dump(const Script& s) {
    for (const Beat& b : s) std::fprintf(stderr, "    %s\n", b.line.key.c_str());
}

// ---- Games -------------------------------------------------------------------------------------------

Game playSans(const char* fen, const std::vector<const char*>& sans) {
    Game g;
    if (fen) CHECK(g.resetFromFEN(fen));
    for (const char* s : sans) {
        const Move m = g.position().parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        g.play(m);
    }
    return g;
}

// K+R vs K: the rook walks a1-f1-f6 while the black king shuffles, then Rf8 mates (11 White moves).
const char* kRookFen = "7k/8/6K1/8/8/8/8/R7 w - - 0 1";
Game rookGame() {
    return playSans(kRookFen, {"Rb1", "Kg8", "Rc1", "Kh8", "Rd1", "Kg8", "Re1", "Kh8", "Rf1", "Kg8", "Rf2", "Kh8",
                               "Rf3", "Kg8", "Rf4", "Kh8", "Rf5", "Kg8", "Rf6", "Kh8", "Rf8#"});
}

// A verdict for the move at 'ply' of 'g', as the review would have stored it.
Review reviewAt(const Game& g, int ply, MoveClass cls, int cpBefore, int cpAfter, ExType type = ExType::None) {
    Review r;
    PlyVerdict& v = r.verdict;
    const Position& p0 = g.positionAt(size_t(ply));
    v.ply = ply;
    v.mover = p0.sideToMove();
    v.human = true;
    v.uci = p0.toUCI(g.moves()[size_t(ply)]);
    v.san = g.sanMoves()[size_t(ply)];
    v.cls = cls;
    const int sign = v.mover == White ? 1 : -1;
    v.wBest = winPercent(sign * cpBefore);
    v.wPlayed = winPercent(sign * cpAfter);
    v.delta = std::max(0.0, v.wBest - v.wPlayed);
    v.accuracy = moveAccuracy(v.wBest, v.wPlayed);
    v.cpWhiteBefore = cpBefore;
    v.hasEvalBefore = true;
    v.cpWhiteAfter = cpAfter;
    v.hasEvalAfter = true;
    v.bestUci = v.uci;
    v.bestSan = v.san;
    v.exType = type;
    v.check = g.positionAt(size_t(ply) + 1).inCheck();
    return r;
}

// The won game, human White: best moves throughout except a mistake at move 5 (ply 8).
Appraisal wonAppraisal(const Game& g, int level) {
    Appraisal a;
    a.reset(level, White);
    for (int ply = 0; ply < int(g.moves().size()); ply += 2) {
        if (ply == 8) a.add(reviewAt(g, ply, MoveClass::Mistake, 900, 300, ExType::Positional));
        else if (ply == 20) a.add(reviewAt(g, ply, MoveClass::Best, 300, 1000));
        else a.add(reviewAt(g, ply, MoveClass::Best, ply < 8 ? 900 : 300, ply < 8 ? 900 : 300));
    }
    return a;
}

}  // namespace

// ---- Accuracy ------------------------------------------------------------------------------------------

TEST(coach_appraisal_accuracy_vectors) {
    // lila AccuracyPercentTest.scala (the loose bounds), and the values of this port (the tight ones).
    SideAccuracy a = gameAccuracy({-900});   // White blunders on the first move
    CHECK(std::fabs(a.white - 10.0) <= 5.0);
    CHECK(std::fabs(a.white - 10.7) < 0.1);
    CHECK(a.black < 0.0);

    a = gameAccuracy({15, 900});   // Black blunders on the first move
    CHECK(std::fabs(a.black - 10.0) <= 5.0);
    CHECK(std::fabs(a.black - 12.3) < 0.1);
    CHECK(std::fabs(a.white - 100.0) < 1e-9);

    std::vector<int> cps(20, 15);   // 20 perfect moves and a White blunder
    cps.push_back(-900);
    a = gameAccuracy(cps);
    CHECK(std::fabs(a.white - 50.0) <= 5.0);
    CHECK(std::fabs(a.white - 46.9) < 0.1);

    cps.clear();   // 5 average moves (65 cpl) on each side
    for (int i = 0; i < 5; ++i) {
        cps.push_back(-50);
        cps.push_back(15);
    }
    a = gameAccuracy(cps);
    CHECK(std::fabs(a.white - 76.0) <= 8.0);
    CHECK(std::fabs(a.black - 76.0) <= 8.0);
    CHECK(std::fabs(a.white - 77.4) < 0.15);

    cps.clear();   // 50 bad moves (150 cpl) on each side
    for (int i = 0; i < 50; ++i) {
        cps.push_back(-135);
        cps.push_back(15);
    }
    a = gameAccuracy(cps);
    CHECK(std::fabs(a.white - 54.0) <= 8.0);
    CHECK(std::fabs(a.black - 54.0) <= 8.0);
    CHECK(std::fabs(a.white - 55.0) < 0.15);

    a = gameAccuracy(std::vector<int>(10, 15));   // nothing lost
    CHECK(std::fabs(a.white - 100.0) < 1e-9 && std::fabs(a.black - 100.0) < 1e-9);
    a = gameAccuracy({});
    CHECK(a.white < 0.0 && a.black < 0.0);
    // Black moving first (a position set up with Black to move).
    a = gameAccuracy({-900}, Black);
    CHECK(a.white < 0.0);
    CHECK(std::fabs(a.black - 100.0) < 1e-9);
}

TEST(coach_appraisal_level_suggestion) {
    CHECK(std::fabs(typicalAccuracy(600) - 65.0) < 1e-9);
    CHECK(std::fabs(typicalAccuracy(1250) - 75.0) < 1e-9);
    CHECK(std::fabs(typicalAccuracy(3000) - 91.0) < 1e-9);
    CHECK_EQ(levelRating(1), 750);
    CHECK_EQ(levelRating(6), 2250);
    // Two wins in the last three at level 2, accurate enough for level 3 (typical at 1350: ~76.6).
    CHECK_EQ(suggestLevel(2, {{2, 1, 80.0}, {2, -1, 60.0}, {2, 1, 82.0}}), 3);
    CHECK_EQ(suggestLevel(2, {{2, 1, 70.0}, {2, 1, 82.0}}), 2);   // one win not accurate enough
    CHECK_EQ(suggestLevel(6, {{6, 1, 99.0}, {6, 1, 99.0}}), 6);
    // Three losses in a row below the level's typical accuracy (level 3: ~76.6).
    CHECK_EQ(suggestLevel(3, {{3, -1, 50.0}, {3, -1, 55.0}, {3, -1, 60.0}}), 2);
    CHECK_EQ(suggestLevel(3, {{3, -1, 50.0}, {3, -1, 90.0}, {3, -1, 60.0}}), 3);
    CHECK_EQ(suggestLevel(3, {{3, -1, 50.0}, {3, 0, 55.0}, {3, -1, 60.0}}), 3);
    CHECK_EQ(suggestLevel(1, {{1, -1, 20.0}, {1, -1, 20.0}, {1, -1, 20.0}}), 1);
    // Games at another level do not count.
    CHECK_EQ(suggestLevel(2, {{1, 1, 99.0}, {1, 1, 99.0}, {2, 1, 90.0}}), 2);
}

// ---- Collection ------------------------------------------------------------------------------------------

TEST(coach_appraisal_collection) {
    Game g = rookGame();
    CHECK(g.isOver());
    CHECK_EQ(g.moves().size(), size_t(21));
    Appraisal a;
    a.reset(3, White);
    // Nothing known yet: every position after a move needs an evaluation, except the final mate.
    std::vector<size_t> missing = a.missingEvals(g);
    CHECK_EQ(missing.size(), size_t(20));
    ai::AnalysisRequest r = a.evalRequest(g, 5);
    CHECK_EQ(r.priority, -1);
    CHECK_EQ(r.multiPV, 1);
    CHECK_EQ(r.depth, 14);
    CHECK_EQ(r.moveTimeMs, 400);
    CHECK_EQ(r.moves.size(), size_t(5));
    CHECK_EQ(r.startFen, std::string(kRookFen));

    // The human's reviews fill both sides of each human move; the coach's moves are judged from them.
    Appraisal w = wonAppraisal(g, 3);
    CHECK(w.missingEvals(g).empty());
    std::vector<PlyVerdict> vs = w.verdicts(g);
    CHECK_EQ(vs.size(), size_t(21));
    CHECK(vs[0].human && !vs[1].human);
    CHECK_EQ(vs[1].cls, MoveClass::Forced);   // coach ply with one legal move (Kg8)
    CHECK_EQ(vs[3].cls, MoveClass::Best);     // from the neighbouring evaluations: 900 -> 900
    CHECK_EQ(vs[8].cls, MoveClass::Mistake);

    // A background evaluation fills a gap; a review's own figures win over it.
    Appraisal b;
    b.reset(3, White);
    ai::Analysis an;
    an.ok = true;
    an.whiteToMove = false;   // after 1 ply, Black to move: its view
    an.lines.resize(1);
    an.lines[0].score.cp = -250;
    an.lines[0].pv = {"h8g8"};
    b.addEval(1, an);
    std::vector<size_t> left = b.missingEvals(g);
    CHECK(std::find(left.begin(), left.end(), size_t(1)) == left.end());

    // Takebacks: a retry replaces the verdict of the move taken back and is counted.
    Appraisal t;
    t.reset(1, White);
    Review first = reviewAt(g, 0, MoveClass::Blunder, 900, 0);
    first.verdict.offered = true;
    t.add(first);
    t.truncate(0);   // Game::undo(1)
    CHECK(t.verdicts(g)[0].cls == MoveClass::Unjudged);
    Review retry = reviewAt(g, 0, MoveClass::Best, 900, 900);
    retry.isRetry = true;
    retry.takeback.fixed = true;
    t.add(retry);
    AppraisalStats ts = t.stats(g);
    CHECK_EQ(ts.offers, 1);
    CHECK_EQ(ts.takebacks, 1);
    CHECK_EQ(ts.fixed, 1);
    CHECK_EQ(t.verdicts(g)[0].cls, MoveClass::Best);
}

TEST(coach_appraisal_stats) {
    Game g = rookGame();
    Appraisal a = wonAppraisal(g, 3);
    AppraisalStats st = a.stats(g);
    CHECK_EQ(st.result, 1);
    CHECK_EQ(st.plies, 21);
    CHECK_EQ(st.humanMoves, 11);
    CHECK(st.numbers);
    CHECK(st.human.accuracy > 0.0 && st.human.accuracy < 100.0);
    CHECK(st.coach.accuracy >= 0.0);
    CHECK_EQ(st.human.count(MoveClass::Best), 10);
    CHECK_EQ(st.human.count(MoveClass::Mistake), 1);
    CHECK_EQ(st.human.topMoves, 10);
    CHECK_EQ(st.criticalPly, 8);
    CHECK_EQ(st.bestStreak, 6);   // plies 10..20 after the mistake
    CHECK(st.bestMoment == BestMoment::Streak);
    CHECK(!st.themeRecurring);
    CHECK(st.theme == ExType::Positional);   // the critical moment's type
    CHECK_EQ(st.human.checks, 1);            // the mate
    CHECK_EQ(st.human.captured, 0);
    // The accuracy is lichess' on the evaluation series.
    std::vector<int> cps;
    for (int ply = 0; ply < 21; ++ply) cps.push_back(ply < 8 ? 900 : ply == 20 ? 1000 : 300);
    const SideAccuracy direct = gameAccuracy(cps, White, 900);
    CHECK(std::fabs(direct.white - st.human.accuracy) < 1e-9);

    // Two faults of the same type: a recurring theme.
    Appraisal r;
    r.reset(2, White);
    for (int ply = 0; ply < 21; ply += 2) {
        if (ply == 4 || ply == 12) r.add(reviewAt(g, ply, MoveClass::Blunder, 900, 200, ExType::Fork));
        else r.add(reviewAt(g, ply, MoveClass::Best, 900, 900));
    }
    AppraisalStats rs = r.stats(g);
    CHECK(rs.themeRecurring);
    CHECK(rs.theme == ExType::Fork);
    CHECK_EQ(rs.criticalPly, 4);   // equal Δ: the earliest

    // Too many unjudged moves: no numbers.
    Appraisal u;
    u.reset(3, White);
    u.add(reviewAt(g, 0, MoveClass::Best, 900, 900));
    CHECK(!u.stats(g).numbers);
}

// ---- Scripts ---------------------------------------------------------------------------------------------

TEST(coach_appraisal_won_game) {
    Game g = rookGame();
    for (int level = 1; level <= 6; ++level) {
        Appraisal a = wonAppraisal(g, level);
        AppraisalContext ctx;
        ctx.opening = "family:italian";
        ctx.outOfBookMove = 4;
        Script s = a.script(g, ctx);
        char where[32];
        std::snprintf(where, sizeof where, "won b%d", level);
        checkAppraisal(s, level, where);
        const std::string lv = ".b" + std::to_string(level);
        REQUIRE(!s.empty());
        CHECK(s.front().line.key == "appraisal.open.win" + lv);
        CHECK(s.back().line.key == "appraisal.end" + lv);
        CHECK(s.front().look == Look::Player);
        CHECK(hasPrefix(s, "appraisal.improve."));
        if (level == 1) CHECK(hasKey(s, "appraisal.improve.clean.b1"));   // level 1: blunders only
        if (level >= 2) CHECK(hasPrefix(s, "appraisal.improve.was_winning") || hasPrefix(s, "appraisal.improve.critical"));
        if (level <= 4) CHECK(hasKey(s, "appraisal.best.streak" + lv));
        if (level == 3) CHECK(hasKey(s, "appraisal.num.acc.b3"));
        if (level <= 2) CHECK(!hasPrefix(s, "appraisal.num.acc"));   // no accuracy at levels 1-2
        if (s.empty()) dump(s);
    }
    // The level suggestion: up after a win, never down after a win.
    Appraisal a = wonAppraisal(g, 2);
    AppraisalContext up;
    up.suggestedLevel = 3;
    Script s = a.script(g, up);
    CHECK(hasKey(s, "appraisal.end.up.b2"));
    checkAppraisal(s, 2, "won up");
    AppraisalContext down;
    down.suggestedLevel = 1;
    CHECK(hasKey(a.script(g, down), "appraisal.end.b2"));
}

TEST(coach_appraisal_lost_game) {
    // The same game from Black's side: the human is mated. A blunder at ply 5 turned it.
    Game g = rookGame();
    for (int level = 1; level <= 6; ++level) {
        Appraisal a;
        a.reset(level, Black);
        for (int ply = 1; ply < 21; ply += 2) {
            if (ply == 5) a.add(reviewAt(g, ply, MoveClass::Blunder, 0, 600, ExType::Hanging));
            else a.add(reviewAt(g, ply, ply < 5 ? MoveClass::Best : MoveClass::Good, ply < 5 ? 0 : 600, ply < 5 ? 0 : 600));
        }
        AppraisalStats st = a.stats(g);
        CHECK_EQ(st.result, -1);
        CHECK_EQ(st.criticalPly, 5);
        CHECK_EQ(st.turningPly, 5);
        AppraisalContext ctx;
        ctx.suggestedLevel = level > 1 ? level - 1 : 0;
        Script s = a.script(g, ctx);
        char where[32];
        std::snprintf(where, sizeof where, "lost b%d", level);
        checkAppraisal(s, level, where);
        const std::string lv = ".b" + std::to_string(level);
        CHECK(!s.empty() && s.front().line.key == "appraisal.open.loss" + lv);
        if (level == 1) CHECK(hasKey(s, "appraisal.improve.theme.b1"));   // the blunder's theme
        if (level >= 2) CHECK(hasKey(s, "appraisal.improve.critical" + lv));
        if (level >= 2) CHECK(hasKey(s, "appraisal.end.down" + lv));
        const Beat* improve = nullptr;
        for (const Beat& b : s)
            if (b.line.key.rfind("appraisal.improve.", 0) == 0) improve = &b;
        CHECK(improve != nullptr);
        if (improve) {
            const Arg* theme = improve->line.arg("theme");
            CHECK(theme && theme->text == "theme.hanging");
        }
    }
    // Resigned: the resignation opener; still playable at level 3 -> "it wasn't over yet".
    Game r = playSans(nullptr, {"e4", "e5", "Nf3", "Nc6"});
    r.resign(White);
    Appraisal a;
    a.reset(3, White);
    a.add(reviewAt(r, 0, MoveClass::Best, 20, 20));
    a.add(reviewAt(r, 2, MoveClass::Best, 20, 20));
    AppraisalContext ctx;
    ctx.humanResigned = true;
    Script s = a.script(r, ctx);
    checkAppraisal(s, 3, "resigned");
    CHECK(!s.empty() && s.front().line.key == "appraisal.open.resigned.b3");
    CHECK(hasKey(s, "appraisal.resign_early.b3"));
}

TEST(coach_appraisal_drawn_game) {
    // Stalemate while winning: the stalemate opener and the stalemate improvement.
    Game g = playSans("7k/5K2/8/6Q1/8/8/8/8 w - - 0 1", {"Qg6"});
    CHECK(g.isOver());
    CHECK(g.endReason() == GameEndReason::Stalemate);
    for (int level = 1; level <= 6; ++level) {
        Appraisal a;
        a.reset(level, White);
        a.add(reviewAt(g, 0, MoveClass::Blunder, 1000, 0, ExType::Stalemate));
        Script s = a.script(g, AppraisalContext{});
        char where[32];
        std::snprintf(where, sizeof where, "stalemate b%d", level);
        checkAppraisal(s, level, where);
        const std::string lv = ".b" + std::to_string(level);
        CHECK(!s.empty() && s.front().line.key == "appraisal.open.stalemate" + lv);
        CHECK(hasKey(s, "appraisal.improve.stalemate" + lv));
        CHECK(!hasPrefix(s, "appraisal.num.acc"));   // a short game: no accuracy
    }
    // A draw by agreement, with the reason as a name.
    Game d = playSans(nullptr, {"e4", "e5"});
    d.agreeDraw();
    Appraisal a;
    a.reset(4, White);
    a.add(reviewAt(d, 0, MoveClass::Best, 20, 20));
    Script s = a.script(d, AppraisalContext{});
    checkAppraisal(s, 4, "agreement");
    CHECK(!s.empty() && s.front().line.key == "appraisal.open.draw.b4");
    if (!s.empty()) {
        const Arg* reason = s.front().line.arg("text");
        CHECK(reason && reason->text == "appraisal.reason.agreement");
    }
}

TEST(coach_appraisal_without_engine) {
    // No review at all (engine unavailable): the opener, the rules-based facts, the encouragement.
    Game g = playSans(nullptr, {"e4", "e5", "Qh5", "Nc6", "Bc4", "Nf6", "Qxf7#"});
    for (int level = 1; level <= 6; ++level) {
        Appraisal a;
        a.reset(level, White);
        Script s = a.script(g, AppraisalContext{});
        checkAppraisal(s, level, "no engine");
        CHECK_EQ(s.size(), size_t(3));
        CHECK(hasKey(s, "appraisal.num.material.b" + std::to_string(level)));
        const Beat* m = nullptr;
        for (const Beat& b : s)
            if (b.line.key.rfind("appraisal.num.material", 0) == 0) m = &b;
        if (m) {
            CHECK_EQ(m->line.arg("n")->number, 1);   // one pawn captured
            CHECK_EQ(m->line.arg("m")->number, 1);   // one check (the mate)
        }
    }
    // An unfinished game (abandoned to the menu): no appraisal.
    Game open = playSans(nullptr, {"e4"});
    Appraisal a;
    a.reset(1, White);
    CHECK(a.script(open, AppraisalContext{}).empty());
}

TEST(coach_appraisal_highlight_points_at_the_piece) {
    // A brilliant move whose piece still stands on its square: pointed at, anchored on {move}.
    Game g = rookGame();
    Appraisal a = wonAppraisal(g, 2);
    Review br = reviewAt(g, 18, MoveClass::Best, 300, 300);   // Rf6
    br.verdict.brilliant = true;
    a.add(br);
    Script s = a.script(g, AppraisalContext{});
    checkAppraisal(s, 2, "brilliant");
    const Beat* hl = nullptr;
    for (const Beat& b : s)
        if (b.line.key == "appraisal.best.brilliant.b2") hl = &b;
    CHECK(hl != nullptr);
    if (hl) {
        // Rf6 moved on to f8 (the mate): the piece is not on f6 any more, so no pointing.
        CHECK(hl->gestures.empty());
        CHECK(hl->look == Look::Board);
    }
    Appraisal m = wonAppraisal(g, 2);
    Review mate = reviewAt(g, 20, MoveClass::Best, 300, 1000);   // Rf8#: the rook is on f8
    mate.verdict.great = true;
    m.add(mate);
    s = m.script(g, AppraisalContext{});
    checkAppraisal(s, 2, "great");
    hl = nullptr;
    for (const Beat& b : s)
        if (b.line.key == "appraisal.best.great.b2") hl = &b;
    CHECK(hl != nullptr);
    if (hl) {
        CHECK_EQ(hl->gestures.size(), size_t(1));
        if (!hl->gestures.empty()) {
            CHECK(hl->gestures[0].kind == GestureKind::PointPiece);
            CHECK_EQ(hl->gestures[0].square, parseSquare("f8"));
            CHECK_EQ(hl->gestures[0].anchor, std::string("move"));
        }
        CHECK(hl->look == Look::Target);
    }
    // A brilliant promotion: pointed at only while the queen stands on its square (an empty square
    // reads as a white Piece{}, which must not count as the human's piece).
    auto promotionHighlight = [](const std::vector<const char*>& sans) {
        Game p = playSans("k7/4P3/2K5/8/8/8/8/8 w - - 0 1", sans);
        p.resign(Black);
        Appraisal ap;
        ap.reset(2, White);
        for (int ply = 0; ply < int(p.moves().size()); ply += 2) {
            Review r = reviewAt(p, ply, MoveClass::Best, 900, 900);
            r.verdict.brilliant = ply == 0;
            ap.add(r);
        }
        const Script ps = ap.script(p, AppraisalContext{});
        checkAppraisal(ps, 2, "promotion");
        for (const Beat& b : ps)
            if (b.line.key == "appraisal.best.brilliant.b2") return b;
        CHECK(false);
        return Beat{};
    };
    const Beat left = promotionHighlight({"e8=Q+", "Ka7", "Qe4", "Ka8", "Qd5"});   // e8 is empty at the end
    CHECK(left.gestures.empty());
    CHECK(left.look == Look::Board);
    const Beat stays = promotionHighlight({"e8=Q+", "Ka7"});
    CHECK_EQ(stays.gestures.size(), size_t(1));
    if (!stays.gestures.empty()) {
        CHECK(stays.gestures[0].kind == GestureKind::PointPiece);
        CHECK_EQ(stays.gestures[0].square, parseSquare("e8"));
    }
    CHECK(stays.look == Look::Target);
}

TEST(coach_appraisal_every_key_exists) {
    const auto& en = lines();
    const char* families[] = {
        "appraisal.open.win", "appraisal.open.draw", "appraisal.open.stalemate", "appraisal.open.loss",
        "appraisal.open.resigned", "appraisal.best.brilliant", "appraisal.best.great", "appraisal.best.only",
        "appraisal.best.won", "appraisal.best.streak", "appraisal.best.capture", "appraisal.best.phase",
        "appraisal.num.material", "appraisal.num.material_nochecks", "appraisal.improve.theme",
        "appraisal.improve.was_winning", "appraisal.improve.stalemate", "appraisal.improve.clean", "appraisal.end",
        "appraisal.end.up",
    };
    for (const char* f : families)
        for (int l = 1; l <= 6; ++l) {
            const std::string k = std::string(f) + ".b" + std::to_string(l);
            if (!en.count(k)) std::fprintf(stderr, "  missing %s\n", k.c_str());
            CHECK(en.count(k) == 1);
        }
    for (int l = 3; l <= 6; ++l)
        for (const char* f : {"appraisal.resign_early", "appraisal.opening", "appraisal.improve.critical"})
            CHECK(en.count(std::string(f) + ".b" + std::to_string(l)) == 1);
    for (int l = 2; l <= 6; ++l) {
        CHECK(en.count("appraisal.end.down.b" + std::to_string(l)) == 1);
        CHECK(en.count("appraisal.improve.critical.b" + std::to_string(l)) == 1);
    }
    for (int l = 1; l <= 3; ++l) {
        CHECK(en.count("appraisal.num.takebacks.b" + std::to_string(l)) == 1);
        CHECK(en.count("appraisal.improve.coach_hung.b" + std::to_string(l)) == 1);
    }
    for (const char* k : {"appraisal.open.loss_close.b3", "appraisal.open.loss_close.b4", "appraisal.num.clean.b2",
                          "appraisal.num.few.b2", "appraisal.num.best.b2", "appraisal.num.acc.b3",
                          "appraisal.num.acc_explain.b3", "appraisal.num.acc.b4", "appraisal.num.acc_phase.b4",
                          "appraisal.num.coach.b4", "appraisal.num.acc.b5", "appraisal.num.book.b5",
                          "appraisal.num.acc.b6", "appraisal.num.acc_solo.b6", "appraisal.phase.opening",
                          "appraisal.phase.middlegame", "appraisal.phase.endgame", "theme.general"})
        CHECK(en.count(k) == 1);
    for (int l = 1; l <= 6; ++l) CHECK(en.count("appraisal.level.l" + std::to_string(l)) == 1);
    // A theme phrase for every explanation type.
    for (int t = int(ExType::MateAllowed); t <= int(ExType::Positional); ++t) {
        const std::string k = std::string("theme.") + exTypeName(ExType(t));
        if (!en.count(k)) std::fprintf(stderr, "  missing %s\n", k.c_str());
        CHECK(en.count(k) == 1);
    }
    // The counted nouns the lines use exist in common.lang.
    const std::set<std::string> nouns = {"move", "point", "pawn", "piece", "mistake", "blunder", "inaccuracy",
                                         "game", "time", "square"};
    for (const auto& kv : en)
        for (const std::string& text : kv.second) {
            size_t p = 0;
            while ((p = text.find('{', p)) != std::string::npos) {
                const size_t q = text.find('}', p);
                const std::string inner = text.substr(p + 1, q - p - 1);
                const size_t colon = inner.find(':');
                if (colon != std::string::npos) {
                    const std::string name = inner.substr(0, colon), form = inner.substr(colon + 1);
                    if (name == "n" || name == "m" || name == "k" || name == "pts") {
                        if (!nouns.count(form)) std::fprintf(stderr, "  %s: unknown counted noun %s\n", kv.first.c_str(), form.c_str());
                        CHECK(nouns.count(form) == 1);
                    }
                }
                p = q + 1;
            }
        }
}

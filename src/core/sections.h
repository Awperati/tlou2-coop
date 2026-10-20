// Section sync (owner 10-21): "your partner is on a different section of the game, go to their section?"
// Each PC keeps a saved list of every story section (chapter world + task) its own player has been in.
// When the partner's section differs from ours for a while, the player is offered to go there:
//   - a section this PC has already been in (or a chapter before the furthest one reached) goes straight away,
//   - one beyond anything explored first asks "This area is beyond anywhere you've explored before, do you still
//     wish to continue?" and needs a second press.
// The jump itself is the game's own task play (task-play-debug!, native rva 0xD61500), done by the bridge.
// Game-free so it can be unit-tested.
#pragma once
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop {

struct Section {
    std::string world;          // chapter world pack, e.g. "world-seattle-arrival"
    std::string task;           // story task, e.g. "sea-hub-..."
    uint64_t findA = 0, findB = 0;   // the two ids task-find / task-play-debug! take (read live on the sender)
    bool valid() const { return !world.empty() && !task.empty(); }
    bool sameAs(const Section& o) const { return world == o.world && task == o.task; }
};

// Section as WorldEvent args (kWorldSection): world, task (u8 length + bytes each), findA, findB (u64 LE).
inline std::vector<uint8_t> SectionArgs(const Section& s) {
    std::vector<uint8_t> a;
    auto str = [&](const std::string& t) { size_t n = t.size() > 255 ? 255 : t.size(); a.push_back(static_cast<uint8_t>(n)); a.insert(a.end(), t.begin(), t.begin() + n); };
    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) a.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    str(s.world); str(s.task); u64(s.findA); u64(s.findB);
    return a;
}
inline bool SectionFromArgs(const std::vector<uint8_t>& a, Section& s) {
    size_t o = 0;
    auto str = [&](std::string& t) { if (o >= a.size()) return false; size_t n = a[o++]; if (o + n > a.size()) return false; t.assign(a.begin() + o, a.begin() + o + n); o += n; return true; };
    auto u64 = [&](uint64_t& v) { if (o + 8 > a.size()) return false; v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(a[o + i]) << (8 * i); o += 8; return true; };
    Section t;
    if (!str(t.world) || !str(t.task) || !u64(t.findA) || !u64(t.findB)) return false;
    s = t;
    return true;
}

// Where the partner is, seen from this PC's own exploration.
enum class SectionWhere { Same, Explored, Unexplored, Unknown };

class ExploredLog {
public:
    // chapterOrder: chapter worlds in story order (roster.ini's [chapter.*] order).
    explicit ExploredLog(std::vector<std::string> chapterOrder = {}) : order_(std::move(chapterOrder)) {}
    void SetChapterOrder(std::vector<std::string> order) { order_ = std::move(order); }

    // Returns true when this is a section not seen before (worth saving).
    bool Visit(const Section& s) {
        if (!s.valid()) return false;
        bool added = seen_.insert({s.world, s.task}).second;
        worlds_.insert(s.world);
        return added;
    }
    bool Visited(const Section& s) const { return seen_.count({s.world, s.task}) != 0; }
    int ChapterIndex(const std::string& world) const {
        for (size_t i = 0; i < order_.size(); ++i) if (order_[i] == world) return static_cast<int>(i);
        return -1;
    }
    int FurthestChapter() const {
        int best = -1;
        for (const auto& w : worlds_) { int i = ChapterIndex(w); if (i > best) best = i; }
        return best;
    }
    // Explored: this exact section was visited, or its chapter comes before the furthest chapter reached
    // (earlier story), or its chapter was visited and is not the furthest one (we moved on from it).
    // Unexplored: anything else, e.g. a later task in the chapter we are in, or a later chapter.
    SectionWhere Classify(const Section& mine, const Section& partner) const {
        if (!partner.valid()) return SectionWhere::Unknown;
        if (mine.valid() && mine.sameAs(partner)) return SectionWhere::Same;
        if (Visited(partner)) return SectionWhere::Explored;
        int pi = ChapterIndex(partner.world), furthest = FurthestChapter();   // ('far' is a Windows macro)
        if (pi >= 0 && furthest >= 0 && pi < furthest) return SectionWhere::Explored;
        return SectionWhere::Unexplored;
    }

    // Saved as one "world|task" line per section.
    bool Load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t bar = line.find('|');
            if (line.empty() || line[0] == ';' || bar == std::string::npos) continue;
            Visit({line.substr(0, bar), line.substr(bar + 1)});
        }
        return true;
    }
    bool Save(const std::string& path) const {
        std::ofstream f(path, std::ios::trunc);
        if (!f) return false;
        f << "; TLOU2 co-op: story sections this PC's player has been in (world|task). Delete to reset.\n";
        for (const auto& [w, t] : seen_) f << w << '|' << t << '\n';
        return static_cast<bool>(f);
    }
    size_t size() const { return seen_.size(); }

private:
    std::vector<std::string> order_;
    std::set<std::pair<std::string, std::string>> seen_;
    std::set<std::string> worlds_;
};

// The offer on screen. Update every loop with the time, both sections and the key edge; read text() and act().
//   Hidden -> (partner elsewhere, steady for settleMs) -> Offer
//   Offer + key: Explored -> Go;  Unexplored -> Confirm
//   Confirm + key (within confirmMs) -> Go;  Confirm timeout -> Offer
//   Offer unanswered for offerMs -> Hidden, and that partner section is not offered again until it changes.
class SectionPrompt {
public:
    enum class State { Hidden, Offer, Confirm };
    unsigned settleMs = 5000, offerMs = 30000, confirmMs = 10000;

    // Returns true once when the player chose to go (the caller starts the jump).
    bool Update(unsigned long long now, const Section& mine, const Section& partner, SectionWhere where, bool keyPressed) {
        bool differs = partner.valid() && mine.valid() && !mine.sameAs(partner);
        if (!differs) { state_ = State::Hidden; differSince_ = 0; declined_ = {}; return false; }
        if (!partner.sameAs(target_)) { target_ = partner; differSince_ = now; state_ = State::Hidden; declined_ = {}; }
        where_ = where;
        switch (state_) {
        case State::Hidden:
            if (!declined_.valid() && now - differSince_ >= settleMs) { state_ = State::Offer; since_ = now; }
            return false;
        case State::Offer:
            if (keyPressed) {
                if (where == SectionWhere::Unexplored) { state_ = State::Confirm; since_ = now; return false; }
                state_ = State::Hidden; declined_ = target_; return true;
            }
            if (now - since_ >= offerMs) { state_ = State::Hidden; declined_ = target_; }
            return false;
        case State::Confirm:
            if (keyPressed) { state_ = State::Hidden; declined_ = target_; return true; }
            if (now - since_ >= confirmMs) { state_ = State::Offer; since_ = now; }
            return false;
        }
        return false;
    }
    State state() const { return state_; }
    const Section& target() const { return target_; }
    // The partner moved on again: offer the new section (also clears a decline).
    void Reset() { state_ = State::Hidden; differSince_ = 0; target_ = {}; declined_ = {}; }

private:
    State state_ = State::Hidden;
    Section target_, declined_;
    SectionWhere where_ = SectionWhere::Unknown;
    unsigned long long differSince_ = 0, since_ = 0;
};

} // namespace coop

#include "core/roster.h"

namespace coop {

namespace {
const char* kNames[] = {"none", "ellie", "abby", "dina", "jesse", "lev", "yara", "tommy", "joel", "manny", "owen", "mel"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(CharacterId::Count));
}

const char* CharacterName(CharacterId id) {
    auto i = static_cast<size_t>(id);
    return i < static_cast<size_t>(CharacterId::Count) ? kNames[i] : "none";
}

CharacterId CharacterFromName(const std::string& name) {
    auto n = Lower(Trim(name));
    for (size_t i = 0; i < static_cast<size_t>(CharacterId::Count); ++i)
        if (n == kNames[i]) return static_cast<CharacterId>(i);
    return CharacterId::None;
}

void Roster::Load(const Ini& ini) {
    chars_.clear();
    chapters_.clear();
    for (const auto& sec : ini.sections()) {
        if (sec.rfind("character.", 0) == 0) {
            CharacterInfo c;
            c.name = sec.substr(10);
            c.id = CharacterFromName(c.name);
            if (c.id == CharacterId::None) continue;
            c.weapons = SplitList(ini.get(sec, "weapons"));
            c.craftables = SplitList(ini.get(sec, "craftables"));
            c.modelHint = ini.get(sec, "model");
            c.look = ini.get(sec, "look");
            if (auto* kv = ini.section(sec))
                for (auto& [k, v] : *kv)
                    if (k.rfind("look.", 0) == 0) c.chapterLooks[Lower(k.substr(5))] = v;
            chars_.push_back(std::move(c));
        } else if (sec.rfind("chapter.", 0) == 0) {
            ChapterRule r;
            r.worldPack = sec.substr(8);
            r.protagonist = CharacterFromName(ini.get(sec, "protagonist"));
            for (auto& n : SplitList(ini.get(sec, "companions"))) {
                auto id = CharacterFromName(n);
                if (id != CharacterId::None) r.companions.push_back(id);
            }
            chapters_.push_back(std::move(r));
        }
    }
}

const CharacterInfo* Roster::character(CharacterId id) const {
    for (auto& c : chars_) if (c.id == id) return &c;
    return nullptr;
}

const ChapterRule* Roster::chapter(const std::string& worldPack) const {
    auto w = Lower(worldPack);
    for (auto& r : chapters_) if (r.worldPack == w) return &r;
    return nullptr;
}

std::vector<CharacterId> Roster::Playable() const {
    std::vector<CharacterId> out;
    for (auto& c : chars_) out.push_back(c.id);
    return out;
}

CharacterId Roster::AssignForSlot(const std::string& worldPack, uint8_t slot, CharacterId preferred, std::mt19937& rng) const {
    const ChapterRule* r = chapter(worldPack);
    if (r && slot >= 1 && slot <= r->companions.size()) return r->companions[slot - 1];

    // No story companion for this slot: honour the player's pick, else random.
    auto pool = Playable();
    std::erase_if(pool, [&](CharacterId c) { return r && c == r->protagonist; });
    if (preferred != CharacterId::None && character(preferred)) return preferred;
    if (pool.empty()) return CharacterId::None;
    std::uniform_int_distribution<size_t> d(0, pool.size() - 1);
    return pool[d(rng)];
}

} // namespace coop

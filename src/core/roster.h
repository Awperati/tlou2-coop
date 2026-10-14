// Who plays whom. Built from config/roster.ini:
//  * [chapter.<world-pack>] says who the story protagonist and companion are.
//  * [character.<name>] gives that character's weapons and craftables.
// Co-op: player 2 takes the chapter's companion (Dina, Lev, ...).
// Solo or no companion in the chapter: the player picks any character or "random".
#pragma once
#include "core/config.h"
#include "core/protocol.h"
#include <random>
#include <string>
#include <vector>

namespace coop {

struct CharacterInfo {
    CharacterId id = CharacterId::None;
    std::string name;
    std::vector<std::string> weapons;     // item tokens, resolved by [items] after RE
    std::vector<std::string> craftables;
    std::string modelHint;                // actor/skin name in game data, filled by RE
    std::string look;                     // player look id (dc1/look2/characters.bin), e.g. dina-player
};

struct ChapterRule {
    std::string worldPack;                // e.g. "world-seattle-arrival"
    CharacterId protagonist = CharacterId::None;
    std::vector<CharacterId> companions;  // in order of slot 1, 2, 3
};

const char* CharacterName(CharacterId id);
CharacterId CharacterFromName(const std::string& name);

class Roster {
public:
    void Load(const Ini& ini);
    const CharacterInfo* character(CharacterId id) const;
    const ChapterRule* chapter(const std::string& worldPack) const;

    // Character for a co-op slot (1..3) in this chapter.
    // `preferred` is honoured only when the chapter has no companion for the slot.
    CharacterId AssignForSlot(const std::string& worldPack, uint8_t slot, CharacterId preferred, std::mt19937& rng) const;

    std::vector<CharacterId> Playable() const;
    size_t chapterCount() const { return chapters_.size(); }

private:
    std::vector<CharacterInfo> chars_;
    std::vector<ChapterRule> chapters_;
};

} // namespace coop

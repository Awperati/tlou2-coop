// Minimal INI reader. Sections, key = value, ; and # comments.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace coop {

class Ini {
public:
    bool LoadFile(const std::string& path);
    void LoadText(const std::string& text);

    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    float getFloat(const std::string& section, const std::string& key, float def) const;
    int getInt(const std::string& section, const std::string& key, int def) const;
    bool getBool(const std::string& section, const std::string& key, bool def) const;
    const std::map<std::string, std::string>* section(const std::string& name) const;
    std::vector<std::string> sections() const;

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

std::vector<std::string> SplitList(const std::string& s, char sep = ',');
std::string Trim(const std::string& s);
std::string Lower(std::string s);

} // namespace coop

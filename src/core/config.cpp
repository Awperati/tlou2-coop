#include "core/config.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace coop {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> SplitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) {
        cur = Trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

bool Ini::LoadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    LoadText(ss.str());
    return true;
}

void Ini::LoadText(const std::string& text) {
    std::istringstream in(text);
    std::string line, sec;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { sec = Lower(Trim(line.substr(1, line.size() - 2))); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = Trim(line.substr(eq + 1));
        auto c = v.find(" ;");
        if (c != std::string::npos) v = Trim(v.substr(0, c));
        data_[sec][Lower(Trim(line.substr(0, eq)))] = v;
    }
}

std::string Ini::get(const std::string& s, const std::string& k, const std::string& def) const {
    auto si = data_.find(Lower(s));
    if (si == data_.end()) return def;
    auto ki = si->second.find(Lower(k));
    return ki == si->second.end() ? def : ki->second;
}

float Ini::getFloat(const std::string& s, const std::string& k, float def) const {
    auto v = get(s, k);
    try { return v.empty() ? def : std::stof(v); } catch (...) { return def; }
}

int Ini::getInt(const std::string& s, const std::string& k, int def) const {
    auto v = get(s, k);
    try { return v.empty() ? def : std::stoi(v, nullptr, 0); } catch (...) { return def; }
}

bool Ini::getBool(const std::string& s, const std::string& k, bool def) const {
    auto v = Lower(get(s, k));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

const std::map<std::string, std::string>* Ini::section(const std::string& name) const {
    auto it = data_.find(Lower(name));
    return it == data_.end() ? nullptr : &it->second;
}

std::vector<std::string> Ini::sections() const {
    std::vector<std::string> out;
    for (auto& [k, v] : data_) out.push_back(k);
    return out;
}

} // namespace coop

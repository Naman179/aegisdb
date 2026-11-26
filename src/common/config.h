#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// config.h – Configuration file parser for AegisDB (Milestone 1 / Common)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegis {

class ConfigFile {
public:
    ConfigFile() = default;

    // Load and parse configuration from a file or string.
    // Supports INI style: [section], key = value, and comments (#, ;)
    bool loadFromFile(const std::string& path);
    bool loadFromString(const std::string& content);

    bool has(const std::string& key) const;

    std::string get(const std::string& key, const std::string& defaultVal = "") const;
    int         getInt(const std::string& key, int defaultVal = 0) const;
    uint64_t    getU64(const std::string& key, uint64_t defaultVal = 0) const;
    double      getDouble(const std::string& key, double defaultVal = 0.0) const;
    bool        getBool(const std::string& key, bool defaultVal = false) const;

    void set(const std::string& key, const std::string& value);

    const std::unordered_map<std::string, std::string>& all() const { return values_; }

private:
    static std::string trim(const std::string& str);

    std::unordered_map<std::string, std::string> values_;
};

}  // namespace aegis

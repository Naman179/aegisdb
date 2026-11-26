#include "common/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace aegis {

std::string ConfigFile::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool ConfigFile::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return loadFromString(buffer.str());
}

bool ConfigFile::loadFromString(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    std::string currentSection;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Section header: [section]
        if (line.front() == '[' && line.back() == ']') {
            currentSection = trim(line.substr(1, line.size() - 2));
            continue;
        }

        // Key-Value delimiter: '=' or ':'
        size_t delimiterPos = line.find('=');
        if (delimiterPos == std::string::npos) {
            delimiterPos = line.find(':');
        }

        if (delimiterPos != std::string::npos) {
            std::string key = trim(line.substr(0, delimiterPos));
            std::string value = trim(line.substr(delimiterPos + 1));

            // Remove potential inline comments starting with '#' or ';'
            size_t commentPos = value.find_first_of("#;");
            if (commentPos != std::string::npos) {
                value = trim(value.substr(0, commentPos));
            }

            if (!currentSection.empty()) {
                values_[currentSection + "." + key] = value;
            }
            // Also store bare key if not yet set
            if (values_.find(key) == values_.end()) {
                values_[key] = value;
            }
        }
    }
    return true;
}

bool ConfigFile::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string ConfigFile::get(const std::string& key, const std::string& defaultVal) const {
    auto it = values_.find(key);
    if (it != values_.end()) {
        return it->second;
    }
    return defaultVal;
}

int ConfigFile::getInt(const std::string& key, int defaultVal) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultVal;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return defaultVal;
    }
}

uint64_t ConfigFile::getU64(const std::string& key, uint64_t defaultVal) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultVal;
    try {
        return std::stoull(it->second);
    } catch (...) {
        return defaultVal;
    }
}

double ConfigFile::getDouble(const std::string& key, double defaultVal) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultVal;
    try {
        return std::stod(it->second);
    } catch (...) {
        return defaultVal;
    }
}

bool ConfigFile::getBool(const std::string& key, bool defaultVal) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultVal;
    std::string lower = it->second;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        return false;
    }
    return defaultVal;
}

void ConfigFile::set(const std::string& key, const std::string& value) {
    values_[key] = value;
}

}  // namespace aegis

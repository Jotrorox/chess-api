#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace dotenv {
namespace detail {

inline std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

inline std::string parse_value(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '\"' && value.back() == '\"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }

    const auto comment = value.find('#');
    return trim(value.substr(0, comment));
}

} // namespace detail

// Loads KEY=VALUE pairs from a dotenv file. Existing process environment
// variables always take precedence over values from the file.
inline bool load(const char* path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = detail::trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        std::string_view entry = trimmed;
        constexpr std::string_view export_prefix = "export ";
        if (entry.substr(0, export_prefix.size()) == export_prefix) {
            entry.remove_prefix(export_prefix.size());
        }

        const auto separator = entry.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }

        const std::string key = detail::trim(entry.substr(0, separator));
        if (key.empty() || std::getenv(key.c_str()) != nullptr) {
            continue;
        }

        const std::string value = detail::parse_value(
            std::string(entry.substr(separator + 1))
        );
#if defined(_WIN32)
        _putenv_s(key.c_str(), value.c_str());
#else
        ::setenv(key.c_str(), value.c_str(), 0);
#endif
    }

    return true;
}

} // namespace dotenv

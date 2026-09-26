#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dawplay {

struct XmlNode {
    std::string name;
    std::unordered_map<std::string, std::string> attributes;
    std::vector<XmlNode> children;

    const XmlNode* child(std::string_view name) const;
    std::string attribute(std::string_view key) const;
    bool flag(std::string_view key, bool fallback) const;
    double number(std::string_view key, double fallback) const;
    bool has(std::string_view key) const;
};

struct XmlDocument {
    XmlNode root;
    std::string error;
};

XmlDocument parseXml(std::string_view text);

}  // namespace dawplay

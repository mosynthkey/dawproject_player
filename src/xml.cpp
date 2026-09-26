#include "dawplay/xml.hpp"

#include <cctype>

namespace dawplay {
namespace {

struct Parser {
    std::string_view text;
    size_t cursor = 0;
    std::string error;

    bool ok() const { return error.empty(); }

    void fail(const std::string& message) {
        if (error.empty()) {
            error = message;
        }
    }

    void skipSpace() {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
    }

    bool consume(std::string_view token) {
        if (text.substr(cursor, token.size()) == token) {
            cursor += token.size();
            return true;
        }
        return false;
    }

    void skipUntil(std::string_view token) {
        const size_t found = text.find(token, cursor);
        if (found == std::string_view::npos) {
            fail("Unclosed XML markup");
            cursor = text.size();
            return;
        }
        cursor = found + token.size();
    }

    void skipProlog() {
        skipSpace();
        while (ok() && cursor < text.size() && text[cursor] == '<') {
            if (consume("<?")) {
                skipUntil("?>");
            } else if (consume("<!--")) {
                skipUntil("-->");
            } else if (consume("<!")) {
                skipUntil(">");
            } else {
                break;
            }
            skipSpace();
        }
    }

    std::string decode(std::string_view raw) const {
        std::string value;
        value.reserve(raw.size());
        for (size_t index = 0; index < raw.size(); ++index) {
            if (raw[index] != '&') {
                value.push_back(raw[index]);
                continue;
            }
            const size_t end = raw.find(';', index);
            if (end == std::string_view::npos) {
                value.push_back('&');
                continue;
            }
            const std::string_view entity = raw.substr(index + 1, end - index - 1);
            if (entity == "amp") {
                value.push_back('&');
            } else if (entity == "lt") {
                value.push_back('<');
            } else if (entity == "gt") {
                value.push_back('>');
            } else if (entity == "quot") {
                value.push_back('"');
            } else if (entity == "apos") {
                value.push_back('\'');
            } else {
                value.push_back('&');
                value.append(entity);
                value.push_back(';');
            }
            index = end;
        }
        return value;
    }

    std::string readName() {
        const size_t start = cursor;
        while (cursor < text.size()) {
            const unsigned char character = static_cast<unsigned char>(text[cursor]);
            if (std::isalnum(character) || character == '_' || character == ':' || character == '-' || character == '.') {
                ++cursor;
                continue;
            }
            break;
        }
        if (cursor == start) {
            fail("Expected an XML name");
        }
        return std::string(text.substr(start, cursor - start));
    }

    std::string readValue() {
        if (cursor >= text.size() || (text[cursor] != '"' && text[cursor] != '\'')) {
            fail("Expected a quoted XML attribute");
            return {};
        }
        const char quote = text[cursor++];
        const size_t start = cursor;
        while (cursor < text.size() && text[cursor] != quote) {
            ++cursor;
        }
        const std::string value = decode(text.substr(start, cursor - start));
        if (cursor < text.size()) {
            ++cursor;
        }
        return value;
    }

    XmlNode parseElement() {
        XmlNode node;
        if (!consume("<")) {
            fail("Expected '<'");
            return node;
        }
        node.name = readName();
        while (ok()) {
            skipSpace();
            if (cursor >= text.size()) {
                fail("Unclosed XML element");
                break;
            }
            if (consume("/>")) {
                return node;
            }
            if (consume(">")) {
                break;
            }
            const std::string key = readName();
            skipSpace();
            if (!consume("=")) {
                fail("Expected '=' in an XML attribute");
                break;
            }
            skipSpace();
            node.attributes.emplace(key, readValue());
        }
        const std::string closing = "</" + node.name;
        while (ok()) {
            if (text.compare(cursor, closing.size(), closing) == 0) {
                cursor += closing.size();
                skipSpace();
                if (!consume(">")) {
                    fail("Expected '>' after a closing tag");
                }
                return node;
            }
            if (cursor < text.size() && text[cursor] == '<') {
                if (consume("<!--")) {
                    skipUntil("-->");
                    continue;
                }
                node.children.push_back(parseElement());
                continue;
            }
            const size_t start = cursor;
            while (cursor < text.size() && text[cursor] != '<') {
                ++cursor;
            }
            if (cursor == start) {
                fail("Malformed XML element");
                break;
            }
        }
        return node;
    }
};

}  // namespace

const XmlNode* XmlNode::child(std::string_view name) const {
    for (const XmlNode& candidate : children) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

std::string XmlNode::attribute(std::string_view key) const {
    const auto found = attributes.find(std::string(key));
    if (found == attributes.end()) {
        return {};
    }
    return found->second;
}

bool XmlNode::has(std::string_view key) const {
    return attributes.find(std::string(key)) != attributes.end();
}

bool XmlNode::flag(std::string_view key, bool fallback) const {
    if (!has(key)) {
        return fallback;
    }
    const std::string value = attribute(key);
    return value == "true" || value == "1" || value == "True";
}

double XmlNode::number(std::string_view key, double fallback) const {
    if (!has(key)) {
        return fallback;
    }
    try {
        return std::stod(attribute(key));
    } catch (...) {
        return fallback;
    }
}

XmlDocument parseXml(std::string_view text) {
    Parser parser;
    parser.text = text;
    parser.skipProlog();
    XmlDocument document;
    if (!parser.ok()) {
        document.error = parser.error;
        return document;
    }
    if (parser.cursor >= parser.text.size()) {
        document.error = "XML document is empty";
        return document;
    }
    document.root = parser.parseElement();
    document.error = parser.error;
    return document;
}

}  // namespace dawplay

/**
 * @file source_text.cpp
 * @brief Implementation of the source-text helpers.
 */
#include "optimizer/source_text.h"

#include <cctype>
#include <regex>

#include "common/register_names.h"

namespace Kites
{
namespace optimizer
{
SourceText splitSource(const std::string &text)
{
    SourceText result;
    result.newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    result.endsWithNewline = !text.empty() && text.back() == '\n';

    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        result.lines.push_back(std::move(line));
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string joinSource(const SourceText &text)
{
    std::string joined;
    for (size_t i = 0; i < text.lines.size(); ++i)
    {
        if (i > 0)
        {
            joined += text.newline;
        }
        joined += text.lines[i];
    }
    if (text.endsWithNewline && !text.lines.empty())
    {
        joined += text.newline;
    }
    return joined;
}

SplitLine splitLine(const std::string &line)
{
    static const std::regex prefixPattern(R"(^\s*(?:[A-Za-z_.$][A-Za-z0-9_.$]*\s*:\s*)*)");
    std::smatch match;
    std::regex_search(line, match, prefixPattern);
    const std::string prefix = match.str(0);
    return {prefix, line.substr(prefix.size())};
}

std::string leadingWhitespace(const std::string &prefix)
{
    return prefix.substr(0, prefix.find_first_not_of(" \t"));
}

bool hasLabel(const std::string &prefix)
{
    return prefix.find_first_not_of(" \t") != std::string::npos;
}

std::string rightTrimmed(std::string text)
{
    const size_t end = text.find_last_not_of(" \t");
    text.erase(end == std::string::npos ? 0 : end + 1);
    return text;
}

namespace
{
// Register index for a name, or -1.
int gprIndexOf(const std::string &name)
{
    for (size_t i = 0; i < 32; ++i)
    {
        if (name == register_names::kGprNames[i] || name == register_names::kGprAliases[i])
        {
            return static_cast<int>(i);
        }
    }
    return name == "fp" ? 8 : -1; // fp is another name for s0
}

bool isIdentifierStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}
} // namespace

std::string renameRegisters(const std::string &body, const std::map<unsigned, unsigned> &renames)
{
    if (renames.empty())
    {
        return body;
    }

    std::string result;
    size_t i = 0;
    while (i < body.size())
    {
        if (body[i] == '#')
        {
            result += body.substr(i); // the comment is left exactly as written
            break;
        }
        if (!isIdentifierStart(body[i]))
        {
            result += body[i++];
            continue;
        }

        size_t end = i;
        while (end < body.size() && isIdentifierChar(body[end]))
        {
            ++end;
        }
        const std::string token = body.substr(i, end - i);
        const int index = gprIndexOf(token);
        const auto rename = index >= 0 ? renames.find(static_cast<unsigned>(index)) : renames.end();
        result += rename != renames.end() ? "x" + std::to_string(rename->second) : token;
        i = end;
    }
    return result;
}

} // namespace optimizer
} // namespace Kites

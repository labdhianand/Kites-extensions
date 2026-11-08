/**
 * @file source_text.cpp
 * @brief Implementation of the source-text helpers.
 */
#include "optimizer/source_text.h"

#include <algorithm>
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
// One step of an edit script: a line kept (' '), removed ('-') or added ('+').
struct DiffStep
{
    char kind;
    size_t beforeIndex; ///< valid for ' ' and '-'
    size_t afterIndex;  ///< valid for ' ' and '+'
};

// The shortest edit script, from a longest common subsequence of lines.
std::vector<DiffStep> diffSteps(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const size_t n = a.size();
    const size_t m = b.size();
    // common[i][j]: length of the longest common subsequence of a[i..] and b[j..].
    std::vector<std::vector<size_t>> common(n + 1, std::vector<size_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
    {
        for (size_t j = m; j-- > 0;)
        {
            common[i][j] = a[i] == b[j] ? common[i + 1][j + 1] + 1
                                        : std::max(common[i + 1][j], common[i][j + 1]);
        }
    }

    std::vector<DiffStep> steps;
    size_t i = 0;
    size_t j = 0;
    while (i < n || j < m)
    {
        if (i < n && j < m && a[i] == b[j])
        {
            steps.push_back({' ', i++, j++});
        }
        else if (i < n && (j == m || common[i + 1][j] >= common[i][j + 1]))
        {
            steps.push_back({'-', i++, j}); // removals before additions, as diff tools show them
        }
        else
        {
            steps.push_back({'+', i, j++});
        }
    }
    return steps;
}
} // namespace

std::string unifiedDiff(const std::string &before, const std::string &after,
                        const std::string &beforeName, const std::string &afterName, int context)
{
    const std::vector<std::string> a = splitSource(before).lines;
    const std::vector<std::string> b = splitSource(after).lines;
    const std::vector<DiffStep> steps = diffSteps(a, b);
    const size_t contextLines = static_cast<size_t>(std::max(context, 0));

    std::string result;
    size_t s = 0;
    while (s < steps.size())
    {
        // Find the next change, then extend the hunk while changes are close together.
        while (s < steps.size() && steps[s].kind == ' ')
        {
            ++s;
        }
        if (s == steps.size())
        {
            break;
        }
        const size_t hunkStart = s >= contextLines ? s - contextLines : 0;
        size_t hunkEnd = s;
        size_t unchangedRun = 0;
        for (size_t t = s; t < steps.size(); ++t)
        {
            if (steps[t].kind == ' ')
            {
                if (++unchangedRun > 2 * contextLines)
                {
                    break;
                }
            }
            else
            {
                unchangedRun = 0;
                hunkEnd = t;
            }
        }
        const size_t hunkStop = std::min(steps.size(), hunkEnd + 1 + contextLines);

        size_t beforeCount = 0;
        size_t afterCount = 0;
        for (size_t t = hunkStart; t < hunkStop; ++t)
        {
            beforeCount += steps[t].kind != '+' ? 1 : 0;
            afterCount += steps[t].kind != '-' ? 1 : 0;
        }
        if (result.empty())
        {
            result += "--- " + beforeName + "\n+++ " + afterName + "\n";
        }
        result += "@@ -" + std::to_string(steps[hunkStart].beforeIndex + 1) + "," +
                  std::to_string(beforeCount) + " +" +
                  std::to_string(steps[hunkStart].afterIndex + 1) + "," +
                  std::to_string(afterCount) + " @@\n";
        for (size_t t = hunkStart; t < hunkStop; ++t)
        {
            const std::string &line =
                steps[t].kind == '+' ? b[steps[t].afterIndex] : a[steps[t].beforeIndex];
            result += std::string(1, steps[t].kind) + line + "\n";
        }
        s = hunkStop;
    }
    return result;
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

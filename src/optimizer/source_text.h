/**
 * @file source_text.h
 * @brief Line-level helpers for rewriting assembly source while keeping its layout.
 */
#pragma once

#include <map>
#include <string>
#include <vector>

namespace Kites
{
namespace optimizer
{
/// The program text as lines, remembering how to put it back together exactly.
struct SourceText
{
    std::vector<std::string> lines;
    std::string newline{"\n"};
    bool endsWithNewline{false};
};

[[nodiscard]] SourceText splitSource(const std::string &text);
[[nodiscard]] std::string joinSource(const SourceText &text);

/// A source line split into what must stay at its position (indentation and any labels) and the
/// instruction with its trailing comment, which is what moves.
struct SplitLine
{
    std::string prefix;
    std::string body;
};

[[nodiscard]] SplitLine splitLine(const std::string &line);

/// The indentation at the start of `prefix`.
[[nodiscard]] std::string leadingWhitespace(const std::string &prefix);

/// True if `prefix` holds a label and not just indentation.
[[nodiscard]] bool hasLabel(const std::string &prefix);

[[nodiscard]] std::string rightTrimmed(std::string text);

/// Replaces whole register names in the instruction part of `body` (not in a trailing comment).
/// Both canonical (x5) and ABI (t0) names are recognised; replacements are written canonically.
[[nodiscard]] std::string renameRegisters(const std::string &body,
                                          const std::map<unsigned, unsigned> &renames);

} // namespace optimizer
} // namespace Kites

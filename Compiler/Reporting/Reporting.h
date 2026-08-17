#pragma once

// Host-independent presentation primitives shared by command reporters and
// reusable compiler reports. Callers resolve color policy before constructing
// a Style; this component never inspects a terminal or the environment.

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Rux::Reporting {
namespace Ansi {
inline constexpr std::string_view green = "\033[32m";
inline constexpr std::string_view red = "\033[31m";
inline constexpr std::string_view yellow = "\033[33m";
inline constexpr std::string_view cyan = "\033[36m";
inline constexpr std::string_view bold = "\033[1m";
inline constexpr std::string_view dim = "\033[2m";
inline constexpr std::string_view reset = "\033[0m";
} // namespace Ansi

enum class StatusKind {
    Progress,
    Success,
    Warning,
    Error,
    Failure,
};

enum class StatusVerb {
    Compiling,
    Checking,
    Downloading,
    Built,
    Checked,
    Installed,
    Removed,
    Failed,
};

struct Style {
    bool enabled = false;

    [[nodiscard]] std::string_view Green() const;
    [[nodiscard]] std::string_view Red() const;
    [[nodiscard]] std::string_view Yellow() const;
    [[nodiscard]] std::string_view Cyan() const;
    [[nodiscard]] std::string_view Bold() const;
    [[nodiscard]] std::string_view Dim() const;
    [[nodiscard]] std::string_view Reset() const;
    [[nodiscard]] std::string_view Color(StatusKind kind) const;
};

/// Two spaces are the canonical indent for supporting report detail. The wider continuation aligns text below the
/// message portion of an `error: ` line.
inline constexpr std::string_view indentation = "  ";
inline constexpr std::string_view errorContinuation = "       ";

[[nodiscard]] std::string Indent(std::string_view text, std::size_t levels = 1);
[[nodiscard]] std::string_view StatusText(StatusVerb status);
[[nodiscard]] StatusKind KindOf(StatusVerb status);
[[nodiscard]] std::string RenderStatus(StatusVerb status, const Style &style);

/// One label and value per report row. Rows are rendered as a block, so the label column is sized to the longest label
/// in that block alone: a section aligns with itself rather than with a width picked for the whole report.
struct TableRow {
    std::string_view label;
    std::string_view value;
};

/// Counts, sizes, and durations read as a column when their right edges line up; names and paths read as a column when
/// their left edges do.
enum class ValueAlign {
    Left,
    Right,
};

[[nodiscard]] std::string RenderRows(std::span<const TableRow> rows, ValueAlign align = ValueAlign::Left);

/// A titled block: the title at column 0, its rows indented below it.
[[nodiscard]] std::string RenderSection(std::string_view title, std::span<const TableRow> rows, const Style &style,
                                        ValueAlign align = ValueAlign::Left);

/// Return just the correctly inflected label, or the count and label together. An omitted plural uses the ordinary
/// English `s` suffix.
[[nodiscard]] std::string Pluralize(std::size_t count, std::string_view singular, std::string_view plural = {});
[[nodiscard]] std::string FormatCount(std::size_t count, std::string_view singular, std::string_view plural = {});

/// Human duration policy: milliseconds below one second, seconds below one minute, then minutes plus seconds. Units are
/// always separated by spaces.
[[nodiscard]] std::string FormatDuration(std::chrono::milliseconds elapsed);
} // namespace Rux::Reporting

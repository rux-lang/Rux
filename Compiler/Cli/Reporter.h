#pragma once

// Stream-scoped semantic reporting for CLI commands. The reporter owns CLI
// suppression and terminal presentation, but not command wording or stream
// selection: callers provide both explicitly.

#include "Cli/Cli.h"
#include "Reporting/Reporting.h"

#include <cstdio>
#include <span>
#include <string_view>

namespace Rux::CliSupport {
enum class MessageVisibility {
    // Diagnostics and their corrective context are never hidden by --quiet.
    Always,
    // Ordinary command progress, outcomes, detail, tables, and summaries.
    Normal,
    // Additional phase and path detail requested with --verbose.
    Verbose,
};

struct ReporterOptions {
    ColorMode color = ColorMode::Auto;
    bool quiet = false;
    bool verbose = false;
};

struct TableRow {
    std::string_view label;
    std::string_view value;
};

class Reporter {
public:
    Reporter(std::FILE *output, ReporterOptions options = {});

    [[nodiscard]] bool Visible(MessageVisibility visibility) const;
    [[nodiscard]] const Reporting::Style &Style() const;

    void Error(std::string_view text) const;
    void Warning(std::string_view text) const;
    void Note(std::string_view text) const;
    void Help(std::string_view text) const;
    void Link(std::string_view text) const;

    // `primary` is the status verb and is the only bold portion of the line.
    void Progress(std::string_view primary, std::string_view detail = {}) const;
    void Success(std::string_view primary, std::string_view detail = {}) const;
    void Detail(std::string_view text, MessageVisibility visibility = MessageVisibility::Normal) const;
    void Verbose(std::string_view text) const;
    void Table(std::span<const TableRow> rows, MessageVisibility visibility = MessageVisibility::Normal) const;
    void Summary(std::string_view title, std::span<const TableRow> rows) const;

private:
    void Labeled(std::string_view label, std::string_view text, std::string_view color, bool indented = false) const;
    void Status(std::string_view primary, std::string_view detail, Reporting::StatusKind kind) const;

    std::FILE *output_;
    ReporterOptions options_;
    Reporting::Style style_;
};
} // namespace Rux::CliSupport

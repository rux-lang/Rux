#pragma once

// Stream-scoped semantic reporting for CLI commands. The reporter owns CLI
// suppression and terminal presentation, but not command wording or stream
// selection: callers provide both explicitly.

#include "Cli/Cli.h"
#include "Reporting/Reporting.h"

#include <cstdio>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Rux::CliSupport {
enum class MessageVisibility {
    /// Diagnostics and their corrective context are never hidden by --quiet.
    Always,
    /// Ordinary command progress, outcomes, detail, tables, and summaries.
    Normal,
    /// Additional phase and path detail requested with --verbose.
    Verbose,
};

struct ReporterOptions {
    ColorMode color = ColorMode::Auto;
    bool quiet = false;
    bool verbose = false;
};

/// Report rows and their alignment policy are shared with reusable compiler reports, which render the same blocks into
/// strings rather than a stream.
using TableRow = Reporting::TableRow;
using ValueAlign = Reporting::ValueAlign;

class Reporter {
public:
    Reporter(std::FILE *inputOutput, ReporterOptions inputOptions = {});
    /// A reporter that collects what it would print into `inputCapture`, styled for `styleOf`, the stream the text is
    /// written to later. Work that runs side by side reports through these so its reports never interleave.
    Reporter(std::string &inputCapture, std::FILE *styleOf, ReporterOptions inputOptions = {});

    [[nodiscard]] bool Visible(MessageVisibility visibility) const;
    [[nodiscard]] const Reporting::Style &Style() const;

    void Error(std::string_view text) const;
    void Warning(std::string_view text) const;
    void Note(std::string_view text) const;
    void Help(std::string_view text) const;
    void Link(std::string_view text) const;

    /// `primary` is the status verb and is the only bold portion of the line.
    void Progress(std::string_view primary, std::string_view detail = {}) const;
    void Success(std::string_view primary, std::string_view detail = {}) const;
    /// Failure outcomes remain visible in quiet mode, just like diagnostics.
    void Failure(std::string_view primary, std::string_view detail = {}) const;
    void Detail(std::string_view text, MessageVisibility visibility = MessageVisibility::Normal) const;
    void Verbose(std::string_view text) const;
    void Write(std::string_view text, MessageVisibility visibility = MessageVisibility::Normal) const;
    /// Finish a report block before switching to another stream or waiting on a worker.
    void Flush() const;
    void Table(std::span<const TableRow> rows, MessageVisibility visibility = MessageVisibility::Normal) const;
    void Summary(std::string_view title, std::span<const TableRow> rows) const;

private:
    template <typename... Arguments>
    void Print(std::format_string<Arguments...> format, Arguments &&...arguments) const {
        if (capture != nullptr)
            capture->append(std::format(format, std::forward<Arguments>(arguments)...));
        else
            std::print(output, format, std::forward<Arguments>(arguments)...);
    }

    void Labeled(std::string_view label, std::string_view text, std::string_view color, bool indented = false) const;
    void Status(std::string_view primary, std::string_view detail, Reporting::StatusKind kind,
                MessageVisibility visibility) const;

    std::FILE *output;
    std::string *capture = nullptr;
    ReporterOptions options;
    Reporting::Style style;
};
} // namespace Rux::CliSupport

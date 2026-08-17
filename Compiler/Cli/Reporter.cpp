#include "Cli/Reporter.h"

#include "Cli/TerminalStyle.h"

#include <print>

namespace Rux::CliSupport {
Reporter::Reporter(std::FILE *inputOutput, const ReporterOptions inputOptions)
    : output(inputOutput)
    , options(inputOptions)
    , style{ColorEnabled(inputOptions.color, inputOutput)} {
}

bool Reporter::Visible(const MessageVisibility visibility) const {
    switch (visibility) {
    case MessageVisibility::Always:
        return true;
    case MessageVisibility::Normal:
        return !options.quiet;
    case MessageVisibility::Verbose:
        return !options.quiet && options.verbose;
    }
    return false;
}

const Reporting::Style &Reporter::Style() const {
    return style;
}

void Reporter::Labeled(const std::string_view label, const std::string_view text, const std::string_view color,
                       const bool indented) const {
    std::print(output, "{}{}{}{}:{} {}\n", indented ? Reporting::indentation : std::string_view{}, color, style.Bold(),
               label, style.Reset(), text);
}

void Reporter::Error(const std::string_view text) const {
    Labeled("error", text, style.Red());
}

void Reporter::Warning(const std::string_view text) const {
    Labeled("warning", text, style.Yellow());
}

void Reporter::Note(const std::string_view text) const {
    Labeled("note", text, style.Dim(), true);
}

void Reporter::Help(const std::string_view text) const {
    Labeled("help", text, {}, true);
}

void Reporter::Link(const std::string_view text) const {
    std::print(output, "{}{}{}docs:{} {}{}{}\n", Reporting::indentation, style.Cyan(), style.Bold(), style.Reset(),
               style.Cyan(), text, style.Reset());
}

void Reporter::Status(const std::string_view primary, const std::string_view detail, const Reporting::StatusKind kind,
                      const MessageVisibility visibility) const {
    if (!Visible(visibility)) {
        return;
    }
    std::print(output, "{}{}{}{}", style.Color(kind), style.Bold(), primary, style.Reset());
    if (!detail.empty()) {
        std::print(output, " {}", detail);
    }
    std::print(output, "\n");
}

void Reporter::Progress(const std::string_view primary, const std::string_view detail) const {
    Status(primary, detail, Reporting::StatusKind::Progress, MessageVisibility::Normal);
}

void Reporter::Success(const std::string_view primary, const std::string_view detail) const {
    Status(primary, detail, Reporting::StatusKind::Success, MessageVisibility::Normal);
}

void Reporter::Failure(const std::string_view primary, const std::string_view detail) const {
    Status(primary, detail, Reporting::StatusKind::Failure, MessageVisibility::Always);
}

void Reporter::Detail(const std::string_view text, const MessageVisibility visibility) const {
    if (Visible(visibility)) {
        std::print(output, "{}{}\n", Reporting::indentation, text);
    }
}

void Reporter::Verbose(const std::string_view text) const {
    if (Visible(MessageVisibility::Verbose)) {
        std::print(output, "{}{}{}{}\n", Reporting::indentation, style.Dim(), text, style.Reset());
    }
}

void Reporter::Write(const std::string_view text, const MessageVisibility visibility) const {
    if (Visible(visibility)) {
        std::print(output, "{}", text);
    }
}

void Reporter::Table(const std::span<const TableRow> rows, const MessageVisibility visibility) const {
    if (!Visible(visibility) || rows.empty()) {
        return;
    }
    std::print(output, "{}", Reporting::RenderRows(rows));
}

void Reporter::Summary(const std::string_view title, const std::span<const TableRow> rows) const {
    if (!Visible(MessageVisibility::Normal)) {
        return;
    }
    std::print(output, "{}", Reporting::RenderSection(title, rows, style));
}
} // namespace Rux::CliSupport

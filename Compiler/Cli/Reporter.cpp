#include "Cli/Reporter.h"

#include "Cli/TerminalStyle.h"

#include <print>

namespace Rux::CliSupport {
Reporter::Reporter(std::FILE *output, const ReporterOptions options)
    : output_(output)
    , options_(options)
    , style_{ColorEnabled(options.color, output)} {
}

bool Reporter::Visible(const MessageVisibility visibility) const {
    switch (visibility) {
    case MessageVisibility::Always:
        return true;
    case MessageVisibility::Normal:
        return !options_.quiet;
    case MessageVisibility::Verbose:
        return !options_.quiet && options_.verbose;
    }
    return false;
}

const Reporting::Style &Reporter::Style() const {
    return style_;
}

void Reporter::Labeled(const std::string_view label, const std::string_view text, const std::string_view color,
                       const bool indented) const {
    std::print(output_, "{}{}{}{}:{} {}\n", indented ? Reporting::indentation : std::string_view{}, color,
               style_.Bold(), label, style_.Reset(), text);
}

void Reporter::Error(const std::string_view text) const {
    Labeled("error", text, style_.Red());
}

void Reporter::Warning(const std::string_view text) const {
    Labeled("warning", text, style_.Yellow());
}

void Reporter::Note(const std::string_view text) const {
    Labeled("note", text, style_.Dim(), true);
}

void Reporter::Help(const std::string_view text) const {
    Labeled("help", text, {}, true);
}

void Reporter::Link(const std::string_view text) const {
    std::print(output_, "{}{}{}docs:{} {}{}{}\n", Reporting::indentation, style_.Cyan(), style_.Bold(), style_.Reset(),
               style_.Cyan(), text, style_.Reset());
}

void Reporter::Status(const std::string_view primary, const std::string_view detail, const Reporting::StatusKind kind,
                      const MessageVisibility visibility) const {
    if (!Visible(visibility)) {
        return;
    }
    std::print(output_, "{}{}{}{}", style_.Color(kind), style_.Bold(), primary, style_.Reset());
    if (!detail.empty()) {
        std::print(output_, " {}", detail);
    }
    std::print(output_, "\n");
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
        std::print(output_, "{}{}\n", Reporting::indentation, text);
    }
}

void Reporter::Verbose(const std::string_view text) const {
    if (Visible(MessageVisibility::Verbose)) {
        std::print(output_, "{}{}{}{}\n", Reporting::indentation, style_.Dim(), text, style_.Reset());
    }
}

void Reporter::Write(const std::string_view text, const MessageVisibility visibility) const {
    if (Visible(visibility)) {
        std::print(output_, "{}", text);
    }
}

void Reporter::Table(const std::span<const TableRow> rows, const MessageVisibility visibility) const {
    if (!Visible(visibility) || rows.empty()) {
        return;
    }
    std::print(output_, "{}", Reporting::RenderRows(rows));
}

void Reporter::Summary(const std::string_view title, const std::span<const TableRow> rows) const {
    if (!Visible(MessageVisibility::Normal)) {
        return;
    }
    std::print(output_, "{}", Reporting::RenderSection(title, rows, style_));
}
} // namespace Rux::CliSupport

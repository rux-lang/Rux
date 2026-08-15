#include "Reporting/Reporting.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace Rux::Reporting {
namespace {
std::string FormatDecimal(const double value, const int decimals) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(decimals) << value;
    std::string text = output.str();
    const auto dot = text.find('.');
    if (dot == std::string::npos) {
        return text;
    }
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}
} // namespace

std::string_view Style::Green() const {
    return enabled ? Ansi::green : std::string_view{};
}

std::string_view Style::Red() const {
    return enabled ? Ansi::red : std::string_view{};
}

std::string_view Style::Yellow() const {
    return enabled ? Ansi::yellow : std::string_view{};
}

std::string_view Style::Cyan() const {
    return enabled ? Ansi::cyan : std::string_view{};
}

std::string_view Style::Bold() const {
    return enabled ? Ansi::bold : std::string_view{};
}

std::string_view Style::Dim() const {
    return enabled ? Ansi::dim : std::string_view{};
}

std::string_view Style::Reset() const {
    return enabled ? Ansi::reset : std::string_view{};
}

std::string_view Style::Color(const StatusKind kind) const {
    switch (kind) {
    case StatusKind::Progress:
        return Cyan();
    case StatusKind::Success:
        return Green();
    case StatusKind::Warning:
        return Yellow();
    case StatusKind::Error:
    case StatusKind::Failure:
        return Red();
    }
    return {};
}

std::string Indent(const std::string_view text, const std::size_t levels) {
    if (text.empty() || levels == 0) {
        return std::string(text);
    }
    const std::string prefix(levels * indentation.size(), ' ');
    std::string result;
    result.reserve(text.size() + prefix.size());
    result += prefix;
    for (std::size_t i = 0; i < text.size(); ++i) {
        result += text[i];
        if (text[i] == '\n' && i + 1 < text.size()) {
            result += prefix;
        }
    }
    return result;
}

std::string_view StatusText(const StatusVerb status) {
    switch (status) {
    case StatusVerb::Compiling:
        return "Compiling";
    case StatusVerb::Checking:
        return "Checking";
    case StatusVerb::Downloading:
        return "Downloading";
    case StatusVerb::Built:
        return "Built";
    case StatusVerb::Checked:
        return "Checked";
    case StatusVerb::Installed:
        return "Installed";
    case StatusVerb::Removed:
        return "Removed";
    case StatusVerb::Failed:
        return "Failed";
    }
    return {};
}

StatusKind KindOf(const StatusVerb status) {
    switch (status) {
    case StatusVerb::Compiling:
    case StatusVerb::Checking:
    case StatusVerb::Downloading:
        return StatusKind::Progress;
    case StatusVerb::Built:
    case StatusVerb::Checked:
    case StatusVerb::Installed:
    case StatusVerb::Removed:
        return StatusKind::Success;
    case StatusVerb::Failed:
        return StatusKind::Failure;
    }
    return StatusKind::Failure;
}

std::string RenderStatus(const StatusVerb status, const Style &style) {
    return std::string(style.Color(KindOf(status))) + std::string(style.Bold()) + std::string(StatusText(status)) +
           std::string(style.Reset());
}

std::string Pluralize(const std::size_t count, const std::string_view singular, const std::string_view plural) {
    if (count == 1) {
        return std::string(singular);
    }
    if (!plural.empty()) {
        return std::string(plural);
    }
    return std::string(singular) + 's';
}

std::string FormatCount(const std::size_t count, const std::string_view singular, const std::string_view plural) {
    return std::to_string(count) + ' ' + Pluralize(count, singular, plural);
}

std::string FormatDuration(const std::chrono::milliseconds elapsed) {
    const auto milliseconds = std::max<std::chrono::milliseconds::rep>(elapsed.count(), 0);
    if (milliseconds < 1'000) {
        return std::to_string(milliseconds) + " ms";
    }
    if (milliseconds < 60'000) {
        return FormatDecimal(static_cast<double>(milliseconds) / 1'000.0, 2) + " s";
    }

    const auto minutes = milliseconds / 60'000;
    const auto remaining = milliseconds % 60'000;
    return std::to_string(minutes) + " min " + FormatDecimal(static_cast<double>(remaining) / 1'000.0, 1) + " s";
}
} // namespace Rux::Reporting

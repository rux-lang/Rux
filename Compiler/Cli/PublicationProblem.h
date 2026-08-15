#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Rux::CliSupport {
/// CLI-ready context for one rejected registry publication. The primary error
/// stays stable while problem-document codes select actionable guidance.
struct PublicationProblem {
    std::string error;
    std::vector<std::string> notes;
    std::string help;
};

[[nodiscard]] PublicationProblem DescribePublicationProblem(unsigned status, std::string_view body,
                                                            std::string_view identity, std::string_view version,
                                                            std::string_view packageNamespace,
                                                            std::string_view registry,
                                                            std::string_view credentialSource);
} // namespace Rux::CliSupport

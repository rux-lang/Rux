#include "Cli/PublicationProblem.h"

#include "System/Json.h"

#include <format>

namespace Rux::CliSupport {
PublicationProblem DescribePublicationProblem(const unsigned status, const std::string_view body,
                                              const std::string_view identity, const std::string_view version,
                                              const std::string_view packageNamespace, const std::string_view registry,
                                              const std::string_view credentialSource) {
    PublicationProblem problem{
        .error = std::format("the registry rejected {} {}", identity, version),
        .notes = {std::format("registry: '{}'", registry), std::format("registry response status: {}", status)},
        .help = {},
    };
    const std::string code = System::JsonLookupString(body, "code");
    if (code == "version_conflict") {
        problem.notes.emplace_back("published versions are immutable");
        problem.help = "increment [Package].Version, then run 'rux publish' again";
    }
    else if (code == "namespace_not_found") {
        problem.notes.emplace_back(std::format("namespace '{}' is not registered at '{}'", packageNamespace, registry));
        problem.help = std::format("claim namespace '{}' at '{}', then retry publication", packageNamespace, registry);
    }
    else if (code == "publication_forbidden") {
        problem.notes.emplace_back(std::format("the credential from '{}' does not own or maintain namespace '{}'",
                                               credentialSource, packageNamespace));
        problem.help =
            std::format("run 'rux login --registry {}' with a namespace owner or maintainer token", registry);
    }
    else if (code == "insufficient_scope") {
        problem.notes.emplace_back(std::format("the credential from '{}' lacks the 'publish' scope", credentialSource));
        problem.help =
            std::format("create a token with the 'publish' scope, then run 'rux login --registry {}'", registry);
    }
    else if (code == "authentication_required") {
        problem.notes.emplace_back(std::format("the credential from '{}' was not accepted", credentialSource));
        problem.help = std::format("run 'rux login --registry {}' with a current token, then retry", registry);
    }
    else if (code == "rate_limited" || status == 429) {
        problem.notes.emplace_back("the registry is rate-limiting publication requests");
        problem.help = std::format("wait briefly, then retry 'rux publish --registry {}'", registry);
    }
    else {
        problem.help = "review the registry details, correct the package or credentials, then retry publication";
    }
    return problem;
}
} // namespace Rux::CliSupport

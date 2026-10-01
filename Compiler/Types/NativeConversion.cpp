#include "Types/NativeConversion.h"

#include <algorithm>
#include <optional>

namespace Rux {
namespace {
using Step = NativeConversionStep;
using Route = std::vector<NativeConversionStep>;

/// Identity, exact injection, or subset widening: the only conversions allowed inside an existing optional's payload
/// or an existing fallible's channel. None of them constructs a wrapper or converts a payload.
std::optional<Route> MemberRoute(const TypeRef &source, const TypeRef &destination) {
    if (source == destination) {
        return Route{Step{Step::Kind::Identity}};
    }
    if (!destination.IsSum()) {
        return std::nullopt;
    }
    if (const auto member = std::ranges::find(destination.inner, source); member != destination.inner.end()) {
        return Route{Step{Step::Kind::Inject, static_cast<std::size_t>(member - destination.inner.begin())}};
    }
    if (source.IsSum() && std::ranges::all_of(source.inner, [&](const TypeRef &member) {
            return std::ranges::find(destination.inner, member) != destination.inner.end();
        })) {
        return Route{Step{Step::Kind::WidenSum}};
    }
    return std::nullopt;
}

Route Prepend(const Step::Kind kind, const Route &rest) {
    Route route{Step{kind}};
    route.insert(route.end(), rest.begin(), rest.end());
    return route;
}

/// Every permitted route from `source` to `destination`, descending through the destination's native levels. A
/// destination level identical to the source ends the descent there, so identity wins at each level.
void CollectRoutes(const TypeRef &source, const TypeRef &destination, std::vector<Route> &routes) {
    if (source == destination) {
        routes.push_back(Route{Step{Step::Kind::Identity}});
        return;
    }
    if (destination.IsSum()) {
        // A sum takes an exact member or a subset; it never searches for conversions into an individual member.
        if (std::optional<Route> route = MemberRoute(source, destination)) {
            routes.push_back(std::move(*route));
        }
        return;
    }
    if (destination.IsOptional()) {
        const TypeRef &payload = destination.inner[0];
        if (source.IsOptional()) {
            // An existing optional keeps its presence or absence and widens its payload, the way a fallible widens
            // its channels.
            if (std::optional<Route> widened = MemberRoute(source.inner[0], payload);
                widened && widened->front().kind != Step::Kind::Identity) {
                routes.push_back(Prepend(Step::Kind::WidenOptional, *widened));
            }
        }
        std::vector<Route> inner;
        CollectRoutes(source, payload, inner);
        for (const Route &route : inner) {
            routes.push_back(Prepend(Step::Kind::Presence, route));
        }
        return;
    }
    if (destination.IsFallible()) {
        const TypeRef &success = destination.inner[0];
        const TypeRef &error = destination.inner[1];
        bool errorCouldForward = false;
        if (source.IsFallible()) {
            const std::optional<Route> successChannel = MemberRoute(source.inner[0], success);
            const std::optional<Route> errorChannel = MemberRoute(source.inner[1], error);
            errorCouldForward = errorChannel.has_value();
            if (successChannel && errorChannel) {
                routes.push_back(Route{Step{Step::Kind::WidenFallible}});
            }
        }
        std::vector<Route> inner;
        CollectRoutes(source, success, inner);
        for (const Route &route : inner) {
            routes.push_back(Prepend(Step::Kind::Success, route));
        }
        // A fallible whose error could reach the destination's error channel is never silently stored as successful
        // data: when channel widening is unavailable, the forwarding reading still competes with every success route.
        if (errorCouldForward && !inner.empty() && std::ranges::none_of(routes, [](const Route &route) {
                return route.front().kind == Step::Kind::WidenFallible;
            })) {
            routes.push_back(Route{Step{Step::Kind::WidenFallible}});
        }
        return;
    }
    // Any other destination takes only the conversions that already exist between ordinary types. A native type
    // nested in an ordinary one, as in `&(A | B)`, converts by identity alone, so there is no conversion to search.
    if (!MentionsNativeType(source) && !MentionsNativeType(destination) && source.IsAssignableTo(destination)) {
        routes.push_back(Route{Step{Step::Kind::Identity}});
    }
}
} // namespace

bool MentionsNativeType(const TypeRef &type) noexcept {
    return type.IsSum() || type.IsOptional() || type.IsFallible() ||
           std::ranges::any_of(type.inner, MentionsNativeType);
}

NativeConversion ClassifyNativeConversion(const TypeRef &source, const TypeRef &destination) {
    if (source.IsUnknown() || destination.IsUnknown()) {
        return {NativeConversion::Outcome::Identity, {}};
    }
    // Contextual `none` follows the expected success levels to an optional and becomes its absence. It never chooses
    // a sum member or a failure channel.
    if (source.IsNoneValue()) {
        Route route;
        const TypeRef *level = &destination;
        while (level->IsFallible()) {
            route.push_back(Step{Step::Kind::Success});
            level = &level->inner[0];
        }
        if (!level->IsOptional() || level->IsNoneValue()) {
            return {NativeConversion::Outcome::Incompatible, {}};
        }
        route.push_back(Step{Step::Kind::Absent});
        return {NativeConversion::Outcome::Converted, std::move(route)};
    }
    if (source == destination) {
        return {NativeConversion::Outcome::Identity, {}};
    }
    std::vector<Route> routes;
    CollectRoutes(source, destination, routes);
    // Routes are compared by the steps they take, so two spellings of one meaning count once.
    std::vector<Route> distinct;
    for (Route &route : routes) {
        if (std::ranges::find(distinct, route) == distinct.end()) {
            distinct.push_back(std::move(route));
        }
    }
    if (distinct.empty()) {
        return {NativeConversion::Outcome::Incompatible, {}};
    }
    if (distinct.size() > 1) {
        return {NativeConversion::Outcome::Ambiguous, {}};
    }
    return {NativeConversion::Outcome::Converted, std::move(distinct.front())};
}
} // namespace Rux

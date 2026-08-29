// SPDX-License-Identifier: MIT
#include "batbot/engine/route_policy.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace batbot {

RouteDecision apply_route_policy(const RouteDecision& proposed, const Config& config) {
    RouteDecision decision = proposed;
    const bool has_fallback = config.has_expert(Subject::Fallback);

    // The delegator answered, but not confidently. Treat that as "no decision"
    // and let the Fallback expert take it rather than committing to a subject on
    // a coin flip. Pinned routes skip this: the user decided, not the model.
    if (decision.source == RouteSource::Model && has_fallback
        && config.routing.min_confidence > 0.0F
        && decision.confidence < config.routing.min_confidence) {
        const std::string wanted(subject_name(decision.subject));
        decision.detail  = "undecided (" + wanted + " at "
                         + std::to_string(static_cast<int>(decision.confidence * 100))
                         + "%); used Fallback";
        decision.subject = Subject::Fallback;
        decision.source  = RouteSource::Fallback;
        return decision;
    }

    if (config.has_expert(decision.subject)) {
        return decision;
    }

    // The routed subject has no model behind it. Fallback is the designated
    // place for that, so prefer it over silently substituting some other seat.
    const std::string wanted(subject_name(decision.subject));
    if (has_fallback && config.routing.use_fallback_expert) {
        decision.subject = Subject::Fallback;
        decision.source  = RouteSource::Fallback;
        decision.detail  = wanted + " has no model; used Fallback";
        return decision;
    }

    // No Fallback expert either. Use any filled seat rather than failing, and
    // say plainly what happened.
    const std::vector<Subject> available = config.configured_experts();
    if (available.empty()) {
        decision.detail = "no experts configured";
        return decision;
    }
    decision.subject = available.front();
    decision.source  = RouteSource::Fallback;
    decision.detail  = wanted + " has no model; used " + std::string(subject_name(decision.subject));
    return decision;
}

}  // namespace batbot

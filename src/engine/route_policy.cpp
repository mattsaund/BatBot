// SPDX-License-Identifier: MIT
#include "crucible/engine/route_policy.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace crucible {

RouteDecision apply_route_policy(const RouteDecision& proposed, const Config& config) {
    RouteDecision decision = proposed;
    const ExpertId fallback_id{kFallbackId};
    const bool has_fallback = config.has_expert(fallback_id);

    // The delegator answered, but not confidently. Treat that as "no decision"
    // and let the Fallback expert take it rather than committing to a subject on
    // a coin flip. Pinned routes skip this: the user decided, not the model.
    if (decision.source == RouteSource::Model && has_fallback
        && config.routing.min_confidence > 0.0F
        && decision.confidence < config.routing.min_confidence) {
        const std::string wanted = expert_label(config.roster, decision.expert);
        decision.detail  = "undecided (" + wanted + " at "
                         + std::to_string(static_cast<int>(decision.confidence * 100))
                         + "%); used Fallback";
        decision.expert  = fallback_id;
        decision.source  = RouteSource::Fallback;
        return decision;
    }

    if (config.has_expert(decision.expert)) {
        return decision;
    }

    // The routed expert has no model behind it. Fallback is the designated
    // place for that, so prefer it over silently substituting some other seat.
    const std::string wanted = expert_label(config.roster, decision.expert);
    if (has_fallback && config.routing.use_fallback_expert) {
        decision.expert  = fallback_id;
        decision.source  = RouteSource::Fallback;
        decision.detail  = wanted + " has no model; used Fallback";
        return decision;
    }

    // No Fallback expert either. Use any filled seat rather than failing, and
    // say plainly what happened.
    const std::vector<ExpertId> available = config.configured_experts();
    if (available.empty()) {
        decision.detail = "no experts configured";
        return decision;
    }
    decision.expert = available.front();
    decision.source = RouteSource::Fallback;
    decision.detail = wanted + " has no model; used "
                    + expert_label(config.roster, decision.expert);
    return decision;
}

}  // namespace crucible

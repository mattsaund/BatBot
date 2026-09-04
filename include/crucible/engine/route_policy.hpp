// SPDX-License-Identifier: MIT
//
// What Crucible does with the delegator's opinion.
//
// The delegator only ever names an expert. Everything else -- whether that
// expert is trustworthy enough to act on, whether it has a model behind it, and
// where the work goes when it does not -- is policy, and lives here.
//
// Kept as a pure function so it can be tested without loading a model, which is
// the only way to check these rules cheaply.
#pragma once

#include "crucible/config/config.hpp"
#include "crucible/routing/router.hpp"

namespace crucible {

/// Adjust `proposed` into the route that will actually be used.
///
/// Four things can happen, and the returned `detail` says which:
///
///   - the route stands, because the expert has a model and the delegator was
///     confident enough;
///   - the delegator answered below `routing.min_confidence`, so it is treated
///     as undecided and the work goes to `routing.default_expert`;
///   - the chosen expert has no model, so the work goes to the default expert;
///   - there is no default expert either, so the work goes to the first filled
///     seat on the roster, and `detail` says so plainly.
///
/// There used to be a tenth seat called Fallback that took the second and third
/// of those. It was never a real expert -- the delegator was forbidden from
/// naming it -- and with a roster the user owns it does not need to be built in:
/// `routing.default_expert` names whichever ordinary seat should play that part,
/// or none, and this reads it.
///
/// A pinned route (`/physics ...`) skips the confidence check: the user decided,
/// not the model.
RouteDecision apply_route_policy(const RouteDecision& proposed, const Config& config);

}  // namespace crucible

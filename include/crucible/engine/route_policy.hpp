// SPDX-License-Identifier: MIT
//
// What Crucible does with the delegator's opinion.
//
// The delegator only ever names a subject. Everything else -- whether that
// subject is trustworthy enough to act on, whether it has a model behind it,
// and where the work goes when it does not -- is policy, and lives here.
//
// Kept as a pure function so it can be tested without loading a model, which
// is the only way to check the fallback rules cheaply.
#pragma once

#include "crucible/config/config.hpp"
#include "crucible/routing/router.hpp"

namespace crucible {

/// Adjust `proposed` into the route that will actually be used.
///
/// Three things can happen, and the returned `detail` says which:
///
///   - the route stands, because the subject has a model and the delegator was
///     confident enough;
///   - the delegator answered below `routing.min_confidence`, so it is treated
///     as undecided and the work goes to Fallback;
///   - the chosen subject has no model, so the work goes to Fallback, or to any
///     filled seat when Fallback is empty too.
///
/// A pinned route (`/physics ...`) skips the confidence check: the user decided,
/// not the model.
RouteDecision apply_route_policy(const RouteDecision& proposed, const Config& config);

}  // namespace crucible

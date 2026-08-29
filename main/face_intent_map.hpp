// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "avatar/expression.hpp"
#include "clawd_motion/face_input.hpp"

namespace stackchan::app {

// Intent → Expression lives in the assembly layer so clawd_motion does not
// duplicate the avatar expression model.
inline avatar::Expression expression_for(clawd_motion::Intent intent) noexcept {
    using avatar::Expression;
    using clawd_motion::Intent;
    switch (intent) {
    case Intent::Tap:
        return Expression::Happy;
    case Intent::Stroke:
        return Expression::Affection;
    case Intent::DoubleTap:
    case Intent::FlickUp:
        return Expression::Surprised;
    case Intent::DizzyStart:
        return Expression::Dizzy;
    case Intent::Hold:
        return Expression::Angry;
    case Intent::FlickDown:
        return Expression::Sleepy;
    default:
        return Expression::Neutral;
    }
}

} // namespace stackchan::app

// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Seam tests for avatar::ExpressionController: from/to + eased blend over a
// fixed duration. The VM has no cross-frame locals, so transition state lives
// here and is written into DrawContext each apply().

#include <cmath>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/expression_controller.hpp"
#include "test_support.hpp"

using stackchan::avatar::DrawContext;
using stackchan::avatar::Expression;
using stackchan::avatar::ExpressionController;

namespace {

bool near(float a, float b)
{
    return std::fabs(a - b) < 1.0e-5f;
}

float smoothstep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

int main()
{
    // Idle: Neutral → Neutral, blend already complete.
    {
        ExpressionController c;
        CHECK(c.from() == Expression::Neutral);
        CHECK(c.to() == Expression::Neutral);
        CHECK(near(c.blend(), 1.0f));

        DrawContext ctx;
        c.apply(ctx, 10'000);
        CHECK(ctx.expression == Expression::Neutral);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 1.0f));
    }

    // set_target is a request; from/to do not change until apply() sees a clock.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        CHECK(c.to() == Expression::Neutral);
        CHECK(near(c.blend(), 1.0f));
    }

    // First apply starts the clock: blend 0 at t0, target Happy, source Neutral.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 1000);
        CHECK(c.from() == Expression::Neutral);
        CHECK(c.to() == Expression::Happy);
        CHECK(near(c.blend(), 0.0f));
        CHECK(ctx.expression == Expression::Happy);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, 0.0f));
    }

    // Midpoint of 300 ms is smoothstep(0.5) = 0.5.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs / 2);
        CHECK(near(c.blend(), smoothstep(0.5f)));
        CHECK(ctx.expression == Expression::Happy);
        CHECK(ctx.expression_from == Expression::Neutral);
        CHECK(near(ctx.expression_blend, smoothstep(0.5f)));
    }

    // At duration the blend completes and from catches up to to.
    {
        ExpressionController c;
        c.set_target(Expression::Sad);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs);
        CHECK(c.from() == Expression::Sad);
        CHECK(c.to() == Expression::Sad);
        CHECK(near(c.blend(), 1.0f));
        CHECK(ctx.expression == Expression::Sad);
        CHECK(ctx.expression_from == Expression::Sad);
        CHECK(near(ctx.expression_blend, 1.0f));
    }

    // Past duration stays complete.
    {
        ExpressionController c;
        c.set_target(Expression::Angry);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs + 50);
        CHECK(c.from() == Expression::Angry);
        CHECK(near(c.blend(), 1.0f));
    }

    // Re-requesting the current target does not restart the ease.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, ExpressionController::kDurationMs);
        c.set_target(Expression::Happy);
        c.apply(ctx, ExpressionController::kDurationMs + 80);
        CHECK(c.from() == Expression::Happy);
        CHECK(c.to() == Expression::Happy);
        CHECK(near(c.blend(), 1.0f));
    }

    // A new target mid-ease takes the previous target as from and restarts.
    {
        ExpressionController c;
        c.set_target(Expression::Happy);
        DrawContext ctx;
        c.apply(ctx, 0);
        c.apply(ctx, 100);
        c.set_target(Expression::Sleepy);
        c.apply(ctx, 140);
        CHECK(c.from() == Expression::Happy);
        CHECK(c.to() == Expression::Sleepy);
        CHECK(near(c.blend(), 0.0f));
        CHECK(ctx.expression == Expression::Sleepy);
        CHECK(ctx.expression_from == Expression::Happy);
    }

    // Duration sits in the 200–400 ms window the ticket asked for.
    {
        CHECK(ExpressionController::kDurationMs >= 200);
        CHECK(ExpressionController::kDurationMs <= 400);
    }

    return avtest::finish("expression_controller");
}

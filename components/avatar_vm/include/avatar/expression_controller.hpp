// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"

namespace stackchan::avatar {

// Cross-frame expression ease. The VM zeros locals every run(), so blend
// progress cannot live in DSL; this controller records from/to and writes
// `expression` / `expression_from` / `expression_blend` into DrawContext.
class ExpressionController {
public:
    static constexpr std::uint32_t kDurationMs = 300;

    void set_target(Expression to) noexcept
    {
        pending_ = to;
    }

    void apply(DrawContext& ctx, std::uint32_t now_ms) noexcept
    {
        if (pending_) {
            if (*pending_ != to_) {
                from_ = to_;
                to_ = *pending_;
                start_ms_ = now_ms;
            }
            pending_.reset();
        }

        if (from_ == to_) {
            blend_ = 1.0f;
        } else {
            const std::uint32_t elapsed = now_ms - start_ms_;
            float t = 1.0f;
            if (elapsed < kDurationMs) {
                t = static_cast<float>(elapsed) / static_cast<float>(kDurationMs);
            }
            blend_ = ease(t);
            if (blend_ >= 1.0f) {
                from_ = to_;
                blend_ = 1.0f;
            }
        }

        ctx.expression = to_;
        ctx.expression_from = from_;
        ctx.expression_blend = blend_;
    }

    Expression from() const noexcept { return from_; }
    Expression to() const noexcept { return to_; }
    float blend() const noexcept { return blend_; }

private:
    static float ease(float t) noexcept
    {
        if (t <= 0.0f) {
            return 0.0f;
        }
        if (t >= 1.0f) {
            return 1.0f;
        }
        return t * t * (3.0f - 2.0f * t);
    }

    Expression from_{Expression::Neutral};
    Expression to_{Expression::Neutral};
    std::optional<Expression> pending_{};
    std::uint32_t start_ms_{0};
    float blend_{1.0f};
};

} // namespace stackchan::avatar

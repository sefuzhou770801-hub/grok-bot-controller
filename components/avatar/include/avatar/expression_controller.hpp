// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"

namespace stackchan::avatar {

// Cross-frame expression ease. The VM zeros locals every run(), so blend
// progress cannot live in DSL; this controller records from/to and two nested
// holds of interrupted mixes, then writes them into DrawContext.
class ExpressionController {
public:
    static constexpr std::uint32_t kDurationMs = 300;

    void set_target(Expression to) noexcept {
        pending_ = to;
    }

    void apply(DrawContext& ctx, std::uint32_t now_ms) noexcept {
        if (pending_) {
            if (*pending_ != to_) {
                retarget(*pending_);
                start_ms_ = now_ms;
            }
            pending_.reset();
        }

        if (from_ == to_ && !hold_active()) {
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
                hold_to_ = to_;
                hold_blend_ = 1.0f;
                hold2_to_ = to_;
                hold2_blend_ = 0.0f;
                blend_ = 1.0f;
            }
        }

        ctx.expression = to_;
        ctx.expression_from = from_;
        ctx.expression_blend = blend_;
        ctx.expression_hold_to = hold_to_;
        ctx.expression_hold_blend = hold_blend_;
        ctx.expression_hold2_to = hold2_to_;
        ctx.expression_hold2_blend = hold2_blend_;
    }

    Expression from() const noexcept {
        return from_;
    }
    Expression to() const noexcept {
        return to_;
    }
    float blend() const noexcept {
        return blend_;
    }
    Expression hold_to() const noexcept {
        return hold_to_;
    }
    float hold_blend() const noexcept {
        return hold_blend_;
    }
    Expression hold2_to() const noexcept {
        return hold2_to_;
    }
    float hold2_blend() const noexcept {
        return hold2_blend_;
    }

private:
    bool hold_active() const noexcept {
        return hold_blend_ < 1.0f || hold_to_ != from_;
    }

    bool hold2_active() const noexcept {
        return hold2_blend_ > 0.0f;
    }

    void retarget(Expression next) noexcept {
        // Freeze the on-screen mix as the new from-pose by shifting the chain
        // one level: the in-flight (to, blend) pair becomes the hold, and a
        // still-active hold moves into hold2. The frozen pose
        //   (1-hb)*[(1-hb2)*from + hb2*hold2_to] + hb*hold_to
        // then equals the pre-interrupt mix exactly for up to two nested
        // interrupts. A third simultaneous interrupt folds the two oldest
        // components into whichever carries more weight; the error is bounded
        // by the lighter, already-decaying weight.
        if (hold_active()) {
            if (hold2_active() && hold2_blend_ >= 0.5f) {
                from_ = hold2_to_;
            }
            hold2_to_ = hold_to_;
            hold2_blend_ = hold_blend_;
        } else {
            hold2_to_ = from_;
            hold2_blend_ = 0.0f;
        }
        hold_to_ = to_;
        hold_blend_ = blend_;
        to_ = next;
    }

    static float ease(float t) noexcept {
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
    Expression hold_to_{Expression::Neutral};
    Expression hold2_to_{Expression::Neutral};
    std::optional<Expression> pending_{};
    std::uint32_t start_ms_{0};
    float blend_{1.0f};
    float hold_blend_{1.0f};
    float hold2_blend_{0.0f};
};

} // namespace stackchan::avatar

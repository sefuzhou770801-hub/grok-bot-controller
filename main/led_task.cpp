// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "led_task.hpp"

#include <array>
#include <cmath>
#include <cstdint>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "led";
// 10 Hz is enough for breathing / rainbow visually, and triples the I2C bus
// + CPU 1 headroom we used to spend at 30 Hz. Dropped from 30 Hz on
// 2026-06-07 after task_wdt on IDLE1 started firing once Phase 2 SSE +
// conv-task TLS + LED + render + speaker/mic all crowded CPU 1 (touch taps
// were being dropped, render dt stretched). See docs/known_issues.md §1.
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(100);

constexpr std::uint8_t kModeOff = 0;
constexpr std::uint8_t kModeSolid = 1;
constexpr std::uint8_t kModeBreath = 2;
constexpr std::uint8_t kModeGradient = 3;

// LipSyncMode (must match config_service::LipSyncMode u8 wire values).
constexpr std::uint8_t kLipBrightness = 0;
constexpr std::uint8_t kLipLevelMeter = 1;

// Nekomimi geometry: 9 LEDs per ear, left = indices 0..8, right = 9..17.
// User-facing 1-indexed LED numbers: 1 (base) … 5 (apex) … 9 (base). In
// 0-indexed C arrays that's apex = 4, base pair = (0, 8). The 5 level-meter
// steps light additional pairs from the base toward the apex:
//   level 1: (0, 8)
//   level 2: + (1, 7)
//   level 3: + (2, 6)
//   level 4: + (3, 5)
//   level 5: + (4)        — apex (single LED, no symmetric partner)
// 0..1 mouth_open is bucketed into 0..5 with thresholds at 0.1, 0.3, 0.5,
// 0.7, 0.9 — slight asymmetric breakpoints so silence-floor noise doesn't
// flicker the first pair on, and a saturated mouth lights all 5 levels.
constexpr float kLevelThresholds[5] = {0.10f, 0.30f, 0.50f, 0.70f, 0.90f};
constexpr std::size_t kLedsPerEar = 9;

// Map a hue in [0, 1) → 24-bit RGB. Standard piecewise sextant HSV with S=V=1.
// Used by the gradient mode.
void hsv_to_rgb(float h, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) noexcept
{
    h -= std::floor(h);
    const float h6 = h * 6.0f;
    const int sector = static_cast<int>(h6);
    const float f = h6 - sector;
    const std::uint8_t v = 255;
    const std::uint8_t p = 0;
    const std::uint8_t q = static_cast<std::uint8_t>(255.0f * (1.0f - f));
    const std::uint8_t t = static_cast<std::uint8_t>(255.0f * f);
    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

// 8-bit channel × 8-bit gain → 8-bit (rounding away from 0 isn't worth the
// cycles here — the strip can't resolve sub-LSB differences anyway).
inline std::uint8_t scale8(std::uint8_t c, std::uint8_t gain) noexcept
{
    return static_cast<std::uint8_t>((static_cast<std::uint16_t>(c) * gain) / 255);
}

void led_task_entry(void* arg)
{
    auto& args = *static_cast<LedTaskArgs*>(arg);
    auto& strip = *args.strip;
    auto& state = *args.state;
    const std::size_t n = strip.size();
    if (n == 0) {
        ESP_LOGW(kTag, "strip size = 0, exiting");
        vTaskDelete(nullptr);
        return;
    }

    // Frame counter — drives breath phase and gradient scroll. Using a wall-
    // clock-derived value (esp_timer) instead of a frame index keeps animations
    // running at the right speed even if the task ever gets paused / preempted.
    auto now_ms = [] { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); };

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // Camera session: the strip refresh reaches the PY32 over In_I2C on
        // some boards, which would re-init the I2C controller under the
        // camera's SCCB driver. Skip the whole frame while quiesced (LEDs
        // just hold their last state for the ~1.5 s session).
        if (state.i2c_quiesce.load(std::memory_order_acquire)) {
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            continue;
        }
        const std::uint8_t mode = state.led.mode.load(std::memory_order_relaxed);
        const std::uint32_t color = state.led.color.load(std::memory_order_relaxed);
        const std::uint8_t base_bright = state.led.brightness.load(std::memory_order_relaxed);
        const std::uint8_t cr = static_cast<std::uint8_t>((color >> 16) & 0xFF);
        const std::uint8_t cg = static_cast<std::uint8_t>((color >>  8) & 0xFF);
        const std::uint8_t cb = static_cast<std::uint8_t>( color        & 0xFF);

        // When the user opts into mouth-driven LED behaviour there are two
        // renderers depending on `lip_sync_mode`:
        //   Brightness (default): scale the base animation's overall
        //     brightness by mouth_open, with a floor so the strip never
        //     fully extinguishes between phrases. The user-set base_bright
        //     becomes the ceiling.
        //   LevelMeter: render the base animation (color from mode = solid /
        //     breath / gradient) normally, then mask off LED pairs above
        //     the current mouth_open level. The base colour pattern is
        //     preserved — only the lit/unlit set changes with the audio
        //     level. mouth_open does NOT additionally scale brightness in
        //     this mode (the meter visualises the level instead).
        // All mouth_open writers (mic lip-sync, jtts babble, conversation
        // playback) feed through the same atomic.
        const bool mouth_sync = state.led.mouth_sync_enabled.load(std::memory_order_relaxed);
        const std::uint8_t lip_mode = state.led.lip_sync_mode.load(std::memory_order_relaxed);
        const bool level_meter_active = mouth_sync && lip_mode == kLipLevelMeter;

        constexpr float kMouthFloor = 0.25f;
        std::uint8_t bright = base_bright;
        float mouth = 0.0f;
        if (mouth_sync) {
            mouth = state.face.mouth_open.load(std::memory_order_relaxed);
            if (mouth < 0.0f) mouth = 0.0f;
            if (mouth > 1.0f) mouth = 1.0f;
            if (lip_mode == kLipBrightness) {
                const float scaled = static_cast<float>(base_bright) *
                                     (kMouthFloor + (1.0f - kMouthFloor) * mouth);
                bright = static_cast<std::uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
            }
        }

        const float t = now_ms() / 1000.0f;

        // 表情联动（默认开）：颜色与呼吸节奏跟随当前表情，覆盖 mode/color。
        // 亮度沿用用户设置（含嘴型亮度缩放）；电平表蒙版在下方继续叠加，
        // 说话时耳朵电平表就是表情色。Dizzy 例外：快转彩虹。
        if (state.led.expr_sync_enabled.load(std::memory_order_relaxed)) {
            const std::uint8_t expr = state.led.resolved_expression.load(std::memory_order_relaxed);
            struct ExprLed {
                std::uint8_t r, g, b;
                float period_s;   // 呼吸周期
                float floor_gain; // 波谷保留亮度
            };
            // 下标 = avatar::Expression。颜色按表情语义：聆听青蓝、思考亮黄、
            // 害羞粉、生气红、睡眠暗紫、无聊暗白、兴奋品红、惊讶白。
            static constexpr ExprLed kExprLed[15] = {
                {255, 210, 160, 4.0f, 0.30f}, // Neutral 暖白
                {255, 200, 0, 2.2f, 0.35f},   // Happy 亮黄
                {40, 80, 255, 5.0f, 0.20f},   // Sad 深蓝
                {255, 30, 0, 1.2f, 0.40f},    // Angry 红（急促）
                {255, 130, 0, 3.5f, 0.30f},   // Doubt 橙
                {90, 0, 140, 6.0f, 0.10f},    // Sleepy 暗紫（慢）
                {0, 190, 255, 1.6f, 0.35f},   // Listening 青蓝（脉冲）
                {255, 255, 90, 2.0f, 0.30f},  // Thinking 亮黄白
                {255, 0, 150, 1.4f, 0.40f},   // Excited 品红
                {0, 255, 170, 2.8f, 0.30f},   // Curious 青绿
                {170, 70, 255, 3.0f, 0.30f},  // Confused 紫
                {255, 255, 255, 1.0f, 0.45f}, // Surprised 白
                {255, 255, 255, 1.0f, 0.30f}, // Dizzy（彩虹快转，色值不用）
                {255, 90, 130, 2.4f, 0.35f},  // Affection 粉
                {110, 110, 110, 5.5f, 0.15f}, // Bored 暗白
            };
            const ExprLed& e = kExprLed[expr < 15 ? expr : 0];
            // 灯在联动模式下是表达器官：用户默认亮度 26（10%）下暖白等低
            // 饱和色几乎不可见（2026-08-30「状态灯没有反应」）。抬到至少
            // 110/255，仍尊重用户调得更亮的情况。
            if (bright < 110) {
                bright = 110;
            }
            if (expr == 12) { // Dizzy: 0.9 秒一圈的彩虹旋转
                const float h0 = t / 0.9f;
                for (std::size_t i = 0; i < n; ++i) {
                    std::uint8_t r, g, b;
                    hsv_to_rgb(h0 + static_cast<float>(i) / static_cast<float>(n), r, g, b);
                    strip.set(i, scale8(r, bright), scale8(g, bright), scale8(b, bright));
                }
            } else {
                const float phase = std::sin(t * 2.0f * 3.14159265f / e.period_s);
                const float gain = (phase * 0.5f + 0.5f) * (1.0f - e.floor_gain) + e.floor_gain;
                const std::uint8_t b2 = static_cast<std::uint8_t>(bright * gain);
                strip.fill(scale8(e.r, b2), scale8(e.g, b2), scale8(e.b, b2));
            }
        } else {
            switch (mode) {
            case kModeSolid: {
            strip.fill(scale8(cr, bright), scale8(cg, bright), scale8(cb, bright));
            break;
        }
        case kModeBreath: {
            // 4 s period sine, biased so dim doesn't fully extinguish (32/255
            // floor keeps the strip visibly "on" at the trough).
            const float phase = std::sin(t * 2.0f * 3.14159265f / 4.0f);
            const float gain = (phase * 0.5f + 0.5f) * 0.85f + 0.15f;
            const std::uint8_t b2 = static_cast<std::uint8_t>(bright * gain);
            strip.fill(scale8(cr, b2), scale8(cg, b2), scale8(cb, b2));
            break;
        }
        case kModeGradient: {
            // Full-strip rainbow that scrolls one full revolution every
            // led_gradient_period_ds × 0.1 s. The colour stored in led_color
            // is ignored in this mode (the hue is generated) — only
            // brightness applies. Clamp the divisor so a runaway 0 doesn't
            // blow up the float division.
            const std::uint8_t period_ds = std::max<std::uint8_t>(
                1, state.led.gradient_period_ds.load(std::memory_order_relaxed));
            const float period_s = static_cast<float>(period_ds) * 0.1f;
            const float h0 = t / period_s;
            for (std::size_t i = 0; i < n; ++i) {
                std::uint8_t r, g, b;
                hsv_to_rgb(h0 + static_cast<float>(i) / static_cast<float>(n), r, g, b);
                strip.set(i, scale8(r, bright), scale8(g, bright), scale8(b, bright));
            }
            break;
        }
            case kModeOff:
            default:
                strip.clear();
                break;
            }
        }

        // Level-meter mask: after the base animation has painted every LED
        // (solid colour / breath / gradient), zero out the LED pairs above
        // the current mouth_open level so only the bottom `level` rows of
        // the ear triangle stay lit. Hue/animation pattern of those lit
        // LEDs comes from the base mode untouched — the meter only changes
        // the lit/unlit set.
        if (level_meter_active) {
            std::size_t level = 0;
            for (std::size_t k = 0; k < 5; ++k) {
                if (mouth >= kLevelThresholds[k]) level = k + 1;
            }
            // Build a 18-bit lit mask. Level k lights the (k-1, 9-k) pair on
            // each ear; level 5 lights only the apex (single LED at index 4
            // / 13). Anything outside the mask gets zeroed.
            std::array<bool, 18> lit{};
            for (std::size_t k = 1; k <= level; ++k) {
                const std::size_t a = k - 1;            // 0..4
                const std::size_t b = kLedsPerEar - k;  // 8..4
                lit[a] = true;
                lit[kLedsPerEar + a] = true;
                if (b != a) {
                    lit[b] = true;
                    lit[kLedsPerEar + b] = true;
                }
            }
            for (std::size_t i = 0; i < n && i < lit.size(); ++i) {
                if (!lit[i]) strip.set(i, 0, 0, 0);
            }
        }

        const bool show_ok = strip.show().has_value();

        // 临时诊断（表情联动落地期）：每 5 秒一行状态，确认链路每环。
        static std::uint32_t last_diag_ms = 0;
        if (now_ms() - last_diag_ms >= 5000) {
            last_diag_ms = now_ms();
            ESP_LOGI(kTag, "diag: expr_sync=%d expr=%u mode=%u bright=%u quiesce=%d show=%d n=%u",
                     static_cast<int>(state.led.expr_sync_enabled.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(state.led.resolved_expression.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(mode), static_cast<unsigned>(bright),
                     static_cast<int>(state.i2c_quiesce.load(std::memory_order_acquire)),
                     static_cast<int>(show_ok), static_cast<unsigned>(n));
        }
        vTaskDelayUntil(&last_wake, kPeriodTicks);
    }
}

} // namespace

void start_led_task(LedTaskArgs& args)
{
    // 4 KiB is comfortable for the sin/HSV math + 64 B local frame buffer.
    // Core 1 keeps the I2C bursts off core 0 where NimBLE + Wi-Fi live.
    xTaskCreatePinnedToCore(led_task_entry, "led", 4096, &args, 2, nullptr, 1);
}

} // namespace stackchan::app

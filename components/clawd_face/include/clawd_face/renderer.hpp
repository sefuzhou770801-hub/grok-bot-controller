// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "avatar/canvas.hpp"
#include "avatar/expression.hpp"

namespace stackchan::clawd_face {

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    bool begin(std::int32_t width, std::int32_t height);
    bool ready() const noexcept;

    void set_expression(avatar::Expression expression) noexcept;
    void set_mouth_open(float ratio) noexcept;
    void set_balloon_text(std::string_view text, std::uint32_t hold_ms = 0);
    void clear_balloon() noexcept;
    bool is_balloon_done() const noexcept;
    void request_full_repaint() noexcept;

    void render(std::uint32_t now_ms, avatar::RichCanvas& canvas);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::clawd_face

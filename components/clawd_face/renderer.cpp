// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// RLE sprite format and animation table are migrated from the old firmware:
// /Users/zhousefu/projects/stackchan/firmware/main/rle_sprite.h
// /Users/zhousefu/projects/stackchan/firmware/main/boards/stackchan/clawd_rle_anim_defs.h
// Old repository is used as read-only reference; this component owns the new
// chassis integration.

#include "clawd_face/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace stackchan::clawd_face {

namespace {

constexpr const char* kTag = "clawd_face";
constexpr const char* kBasePath = "/clawd";
constexpr const char* kStorageLabel = "storage";
constexpr std::int32_t kScale = 1;
constexpr std::uint32_t kDefaultBalloonHoldMs = 3500;
constexpr std::uint32_t kLoaderStackBytes = 4096;
constexpr UBaseType_t kLoaderPriority = tskIDLE_PRIORITY + 1;

struct Animation {
    const char* face;
    const char* asset_name;
    std::uint16_t fallback_frame_ms;
    bool looping;
};

constexpr Animation kAnimations[] = {
    {"idle", "clawd_idle.rle", 125, true},
    {"happy", "clawd_happy.rle", 100, false},
    {"thinking", "clawd_thinking.rle", 125, true},
    {"working_typing", "clawd_typing.rle", 125, true},
    {"typing", "clawd_typing.rle", 125, true},
    {"juggling", "clawd_juggling.rle", 125, true},
    {"sweeping", "clawd_sweeping.rle", 125, false},
    {"building", "clawd_building.rle", 125, true},
    {"debugger", "clawd_debugger.rle", 125, true},
    {"conducting", "clawd_conducting.rle", 125, true},
    {"sleeping", "clawd_sleeping.rle", 125, true},
    {"error", "clawd_error.rle", 100, false},
    {"notification", "clawd_notification.rle", 100, false},
    {"carrying", "clawd_carrying.rle", 125, true},
    {"idle_reading", "clawd_idle_reading.rle", 125, true},
    {"alert", "clawd_error.rle", 100, false},
    {"disconnected", "clawd_disconnected.rle", 125, true},
    {"confused", "clawd_confused.rle", 125, true},
    {"dizzy", "clawd_dizzy.rle", 125, true},
    {"walking", "clawd_walking.rle", 125, true},
    {"going_away", "clawd_going_away.rle", 125, false},
    {"wizard", "clawd_wizard.rle", 125, true},
    {"beacon", "clawd_beacon.rle", 125, true},
    {"mini_crab", "clawd_mini_crab.rle", 250, true},
};

struct AssetView {
    const Animation* animation = nullptr;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t frame_count = 0;
    std::uint16_t frame_ms = 0;
    std::uint16_t transparent_key = 0;
    std::int16_t y_offset = 0;
    std::uint32_t offset_count = 0;
    std::uint32_t rle_word_count = 0;
    const std::uint8_t* frame_offsets = nullptr;
    const std::uint8_t* rle_data = nullptr;
};

struct LoadRequest {
    const Animation* animation = nullptr;
    std::uint32_t sequence = 0;
};

class HeapBuffer {
public:
    HeapBuffer() = default;
    ~HeapBuffer() { reset(); }

    HeapBuffer(const HeapBuffer&) = delete;
    HeapBuffer& operator=(const HeapBuffer&) = delete;

    bool ensure(std::size_t needed, std::uint32_t preferred_caps)
    {
        if (ptr_ != nullptr && capacity_ >= needed) {
            size_ = needed;
            return true;
        }
        reset();
        void* ptr = heap_caps_malloc(needed, preferred_caps | MALLOC_CAP_8BIT);
        if (ptr == nullptr) {
            ptr = heap_caps_malloc(needed, MALLOC_CAP_8BIT);
        }
        if (ptr == nullptr) {
            return false;
        }
        ptr_ = ptr;
        capacity_ = needed;
        size_ = needed;
        return true;
    }

    void reset() noexcept
    {
        if (ptr_ != nullptr) {
            heap_caps_free(ptr_);
            ptr_ = nullptr;
        }
        capacity_ = 0;
        size_ = 0;
    }

    void* data() noexcept { return ptr_; }
    const void* data() const noexcept { return ptr_; }
    std::size_t size() const noexcept { return size_; }

    template <typename T>
    T* as() noexcept
    {
        return static_cast<T*>(ptr_);
    }

    template <typename T>
    const T* as() const noexcept
    {
        return static_cast<const T*>(ptr_);
    }

private:
    void* ptr_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

std::uint16_t read_le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}

std::int16_t read_le_i16(const std::uint8_t* p)
{
    return static_cast<std::int16_t>(read_le16(p));
}

std::uint32_t read_le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t rgb565_from_888(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

std::uint8_t color_r(std::uint32_t color)
{
    return static_cast<std::uint8_t>((color >> 16) & 0xFF);
}

std::uint8_t color_g(std::uint32_t color)
{
    return static_cast<std::uint8_t>((color >> 8) & 0xFF);
}

std::uint8_t color_b(std::uint32_t color)
{
    return static_cast<std::uint8_t>(color & 0xFF);
}

std::uint8_t lerp_byte(std::uint8_t a, std::uint8_t b, std::int32_t pos, std::int32_t max_pos)
{
    if (max_pos <= 0) {
        return b;
    }
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(a) +
                                     ((static_cast<std::int32_t>(b) - static_cast<std::int32_t>(a)) * pos) /
                                         max_pos);
}

std::uint16_t lerp_rgb565(std::uint32_t a, std::uint32_t b, std::int32_t pos, std::int32_t max_pos)
{
    return rgb565_from_888(lerp_byte(color_r(a), color_r(b), pos, max_pos),
                           lerp_byte(color_g(a), color_g(b), pos, max_pos),
                           lerp_byte(color_b(a), color_b(b), pos, max_pos));
}

const char* normalize_face(const char* face)
{
    if (face == nullptr) return nullptr;
    if (std::strcmp(face, "working") == 0 || std::strcmp(face, "working-typing") == 0 ||
        std::strcmp(face, "typing") == 0) {
        return "working_typing";
    }
    if (std::strcmp(face, "working-juggling") == 0) return "juggling";
    if (std::strcmp(face, "working-sweeping") == 0) return "sweeping";
    if (std::strcmp(face, "working-carrying") == 0) return "carrying";
    if (std::strcmp(face, "working-conducting") == 0) return "conducting";
    if (std::strcmp(face, "idle-reading") == 0) return "idle_reading";
    if (std::strcmp(face, "attention") == 0 || std::strcmp(face, "surprised") == 0) return "notification";
    if (std::strcmp(face, "sad") == 0) return "error";
    if (std::strcmp(face, "embarrassed") == 0 || std::strcmp(face, "waking") == 0) return "happy";
    if (std::strcmp(face, "yawning") == 0 || std::strcmp(face, "dozing") == 0 ||
        std::strcmp(face, "collapsing") == 0) {
        return "sleeping";
    }
    return face;
}

const Animation* animation_for_face(const char* face)
{
    const char* mapped = normalize_face(face);
    if (mapped == nullptr) {
        return nullptr;
    }
    for (const auto& animation : kAnimations) {
        if (std::strcmp(mapped, animation.face) == 0) {
            return &animation;
        }
    }
    return nullptr;
}

const char* face_for_expression(avatar::Expression expression)
{
    using avatar::Expression;
    switch (expression) {
        case Expression::Happy:
            return "happy";
        case Expression::Sad:
            return "error";
        case Expression::Angry:
            return "alert";
        case Expression::Doubt:
            return "thinking";
        case Expression::Sleepy:
            return "sleeping";
        case Expression::Neutral:
        default:
            return "idle";
    }
}

bool parse_asset(const std::uint8_t* data, std::size_t size, const Animation& animation, AssetView& out)
{
    if (data == nullptr || size < 28 || data[0] != 'C' || data[1] != 'R' || data[2] != 'L' || data[3] != 'E') {
        ESP_LOGE(kTag, "RLE asset invalid: %s size=%u", animation.asset_name, static_cast<unsigned>(size));
        return false;
    }

    const std::uint16_t version = read_le16(data + 4);
    const std::uint16_t header_size = read_le16(data + 6);
    const std::uint16_t width = read_le16(data + 8);
    const std::uint16_t height = read_le16(data + 10);
    const std::uint16_t frame_count = read_le16(data + 12);
    const std::uint16_t frame_ms = read_le16(data + 14);
    const std::uint16_t transparent_key = read_le16(data + 16);
    const std::int16_t y_offset = read_le_i16(data + 18);
    const std::uint32_t offset_count = read_le32(data + 20);
    const std::uint32_t rle_word_count = read_le32(data + 24);
    const std::size_t offsets_bytes = static_cast<std::size_t>(offset_count) * sizeof(std::uint32_t);
    const std::size_t rle_bytes = static_cast<std::size_t>(rle_word_count) * sizeof(std::uint16_t);

    if (version != 1 || header_size < 28 || header_size > size || width == 0 || height == 0 ||
        frame_count == 0 || offset_count != static_cast<std::uint32_t>(frame_count) + 1 ||
        static_cast<std::size_t>(header_size) + offsets_bytes + rle_bytes > size) {
        ESP_LOGE(kTag, "RLE asset header bad: %s", animation.asset_name);
        return false;
    }

    out = AssetView{.animation = &animation,
                    .data = data,
                    .size = size,
                    .width = width,
                    .height = height,
                    .frame_count = frame_count,
                    .frame_ms = frame_ms == 0 ? animation.fallback_frame_ms : frame_ms,
                    .transparent_key = transparent_key,
                    .y_offset = y_offset,
                    .offset_count = offset_count,
                    .rle_word_count = rle_word_count,
                    .frame_offsets = data + header_size,
                    .rle_data = data + header_size + offsets_bytes};
    return true;
}

} // namespace

class Renderer::Impl {
public:
    ~Impl()
    {
        stop_loader_task();
        if (spiffs_registered_) {
            esp_vfs_spiffs_unregister(kStorageLabel);
        }
    }

    bool begin(std::int32_t width, std::int32_t height)
    {
        width_ = width;
        height_ = height;
        if (width_ <= 0 || height_ <= 0) {
            ESP_LOGE(kTag, "invalid canvas size: %dx%d", static_cast<int>(width_), static_cast<int>(height_));
            return false;
        }
        if (!mount_assets()) {
            return false;
        }
        if (!switch_face_sync("idle", 0, 0)) {
            return false;
        }
        if (!start_loader_task()) {
            return false;
        }
        ready_ = true;
        return true;
    }

    bool ready() const noexcept { return ready_; }

    void set_expression(avatar::Expression expression) noexcept
    {
        desired_face_.store(face_for_expression(expression), std::memory_order_release);
    }

    void set_mouth_open(float ratio) noexcept
    {
        if (ratio < 0.0f) {
            ratio = 0.0f;
        } else if (ratio > 1.0f) {
            ratio = 1.0f;
        }
        mouth_open_ = ratio;
    }

    void set_balloon_text(std::string_view text, std::uint32_t hold_ms)
    {
        balloon_text_.assign(text.data(), text.size());
        balloon_hold_ms_ = hold_ms == 0 ? kDefaultBalloonHoldMs : hold_ms;
        balloon_set_ms_ = 0;
        balloon_done_ = false;
        balloon_visible_ = true;
        frame_dirty_ = true;
    }

    void clear_balloon() noexcept
    {
        balloon_text_.clear();
        balloon_hold_ms_ = 0;
        balloon_set_ms_ = 0;
        balloon_done_ = false;
        balloon_visible_ = false;
        frame_dirty_ = true;
    }

    bool is_balloon_done() const noexcept { return balloon_done_; }

    void request_full_repaint() noexcept { frame_dirty_ = true; }

    void render(std::uint32_t now_ms, avatar::RichCanvas& canvas)
    {
        if (!ready_) {
            return;
        }
        commit_ready_asset(now_ms);

        const char* desired_face = desired_face_.load(std::memory_order_acquire);
        if (std::strcmp(current_face_, desired_face) != 0) {
            request_face_load(desired_face);
        }
        update_frame_index(now_ms);
        update_balloon(now_ms);
        if (!frame_dirty_) {
            return;
        }

        canvas.begin_frame(scene_fill_rgb565_for_y(0));
        draw_scene(canvas);
        draw_current_frame(canvas);
        draw_balloon(canvas);
        frame_dirty_ = false;
    }

private:
    bool mount_assets()
    {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = kBasePath,
            .partition_label = kStorageLabel,
            .max_files = 4,
            .format_if_mount_failed = false,
        };
        const esp_err_t err = esp_vfs_spiffs_register(&conf);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(kTag, "mount SPIFFS '%s' failed: %s", kStorageLabel, esp_err_to_name(err));
            return false;
        }
        spiffs_registered_ = err == ESP_OK;

        std::size_t total = 0;
        std::size_t used = 0;
        const esp_err_t info_err = esp_spiffs_info(kStorageLabel, &total, &used);
        if (info_err == ESP_OK) {
            ESP_LOGI(kTag, "assets mounted: total=%u used=%u", static_cast<unsigned>(total),
                     static_cast<unsigned>(used));
        }
        return true;
    }

    bool read_asset_file(const Animation& animation, HeapBuffer& buffer)
    {
        const std::string path = std::string{kBasePath} + "/" + animation.asset_name;
        FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            ESP_LOGE(kTag, "open asset failed: %s errno=%d", path.c_str(), errno);
            return false;
        }
        if (std::fseek(file, 0, SEEK_END) != 0) {
            ESP_LOGE(kTag, "seek asset end failed: %s", path.c_str());
            std::fclose(file);
            return false;
        }
        const long size = std::ftell(file);
        if (size <= 0) {
            ESP_LOGE(kTag, "asset size invalid: %s size=%ld", path.c_str(), size);
            std::fclose(file);
            return false;
        }
        std::rewind(file);

        if (!buffer.ensure(static_cast<std::size_t>(size), MALLOC_CAP_SPIRAM)) {
            ESP_LOGE(kTag, "asset buffer alloc failed: %s size=%ld", animation.asset_name, size);
            std::fclose(file);
            return false;
        }
        const std::size_t read = std::fread(buffer.data(), 1, static_cast<std::size_t>(size), file);
        std::fclose(file);
        if (read != static_cast<std::size_t>(size)) {
            ESP_LOGE(kTag, "asset read short: %s read=%u size=%ld", animation.asset_name, static_cast<unsigned>(read),
                     size);
            return false;
        }
        return true;
    }

    bool load_asset_into_buffer(const Animation& animation, HeapBuffer& buffer, AssetView& out)
    {
        if (!read_asset_file(animation, buffer)) {
            return false;
        }
        return parse_asset(buffer.as<const std::uint8_t>(), buffer.size(), animation, out);
    }

    bool switch_face_sync(const char* face, std::uint32_t now_ms, std::uint8_t buffer_index)
    {
        const Animation* animation = animation_for_face(face);
        if (animation == nullptr) {
            ESP_LOGW(kTag, "unknown face '%s', fallback to idle", face == nullptr ? "" : face);
            animation = animation_for_face("idle");
            if (animation == nullptr) {
                return false;
            }
        }
        AssetView next{};
        if (!load_asset_into_buffer(*animation, asset_buffers_[buffer_index], next)) {
            return false;
        }
        asset_ = next;
        active_buffer_.store(buffer_index, std::memory_order_release);
        current_face_ = animation->face;
        desired_face_.store(animation->face, std::memory_order_release);
        frame_index_ = 0;
        next_frame_ms_ = now_ms + asset_.frame_ms;
        frame_dirty_ = true;
        ESP_LOGI(kTag, "face=%s asset=%s frames=%u size=%ux%u frame_ms=%u", current_face_,
                 animation->asset_name, static_cast<unsigned>(asset_.frame_count), static_cast<unsigned>(asset_.width),
                 static_cast<unsigned>(asset_.height), static_cast<unsigned>(asset_.frame_ms));
        return true;
    }

    bool start_loader_task()
    {
        load_queue_ = xQueueCreate(1, sizeof(LoadRequest));
        if (load_queue_ == nullptr) {
            ESP_LOGE(kTag, "face loader queue create failed");
            return false;
        }
        stop_loader_.store(false, std::memory_order_release);
        loader_running_.store(true, std::memory_order_release);
        if (xTaskCreate(&Impl::loader_task_entry, "clawd_face_load", kLoaderStackBytes, this, kLoaderPriority,
                        &loader_task_) != pdPASS) {
            ESP_LOGE(kTag, "face loader task create failed");
            loader_running_.store(false, std::memory_order_release);
            vQueueDelete(load_queue_);
            load_queue_ = nullptr;
            return false;
        }
        return true;
    }

    void stop_loader_task()
    {
        if (load_queue_ == nullptr) {
            return;
        }
        stop_loader_.store(true, std::memory_order_release);
        LoadRequest stop{};
        xQueueOverwrite(load_queue_, &stop);
        for (int i = 0; i < 20 && loader_running_.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (loader_running_.load(std::memory_order_acquire) && loader_task_ != nullptr) {
            vTaskDelete(loader_task_);
            loader_running_.store(false, std::memory_order_release);
        }
        loader_task_ = nullptr;
        vQueueDelete(load_queue_);
        load_queue_ = nullptr;
    }

    static void loader_task_entry(void* arg)
    {
        static_cast<Impl*>(arg)->loader_task();
    }

    void loader_task()
    {
        LoadRequest request{};
        while (!stop_loader_.load(std::memory_order_acquire)) {
            if (xQueueReceive(load_queue_, &request, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            LoadRequest latest = request;
            while (xQueueReceive(load_queue_, &latest, 0) == pdTRUE) {
                request = latest;
            }
            if (stop_loader_.load(std::memory_order_acquire) || request.animation == nullptr) {
                break;
            }

            while (!stop_loader_.load(std::memory_order_acquire) &&
                   pending_asset_ready_.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (stop_loader_.load(std::memory_order_acquire)) {
                break;
            }

            const std::uint8_t buffer_index =
                active_buffer_.load(std::memory_order_acquire) == 0 ? static_cast<std::uint8_t>(1)
                                                                    : static_cast<std::uint8_t>(0);
            AssetView loaded{};
            if (!load_asset_into_buffer(*request.animation, asset_buffers_[buffer_index], loaded)) {
                clear_requested_face(request);
                continue;
            }
            if (requested_sequence_.load(std::memory_order_acquire) != request.sequence) {
                continue;
            }

            pending_asset_ = loaded;
            pending_buffer_ = buffer_index;
            pending_sequence_ = request.sequence;
            pending_asset_ready_.store(true, std::memory_order_release);
        }
        loader_running_.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    void clear_requested_face(const LoadRequest& request)
    {
        if (requested_sequence_.load(std::memory_order_acquire) != request.sequence) {
            return;
        }
        requested_animation_.store(nullptr, std::memory_order_release);
        const char* desired_face = desired_face_.load(std::memory_order_acquire);
        if (request.animation != nullptr && std::strcmp(desired_face, request.animation->face) == 0) {
            desired_face_.store("idle", std::memory_order_release);
        }
    }

    void clear_requested_face_if_current(std::uint32_t sequence)
    {
        if (requested_sequence_.load(std::memory_order_acquire) == sequence) {
            requested_animation_.store(nullptr, std::memory_order_release);
        }
    }

    void commit_ready_asset(std::uint32_t now_ms)
    {
        if (!pending_asset_ready_.load(std::memory_order_acquire)) {
            return;
        }

        const AssetView next = pending_asset_;
        const std::uint8_t buffer_index = pending_buffer_;
        const std::uint32_t sequence = pending_sequence_;

        if (next.animation == nullptr ||
            std::strcmp(next.animation->face, desired_face_.load(std::memory_order_acquire)) != 0) {
            pending_asset_ready_.store(false, std::memory_order_release);
            clear_requested_face_if_current(sequence);
            return;
        }

        asset_ = next;
        active_buffer_.store(buffer_index, std::memory_order_release);
        current_face_ = next.animation->face;
        frame_index_ = 0;
        next_frame_ms_ = now_ms + asset_.frame_ms;
        frame_dirty_ = true;
        clear_requested_face_if_current(sequence);
        pending_asset_ready_.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "face=%s asset=%s frames=%u size=%ux%u frame_ms=%u", current_face_,
                 next.animation->asset_name, static_cast<unsigned>(asset_.frame_count),
                 static_cast<unsigned>(asset_.width), static_cast<unsigned>(asset_.height),
                 static_cast<unsigned>(asset_.frame_ms));
    }

    bool request_face_load(const char* face)
    {
        const Animation* animation = animation_for_face(face);
        if (animation == nullptr) {
            ESP_LOGW(kTag, "unknown face '%s', fallback to idle", face == nullptr ? "" : face);
            animation = animation_for_face("idle");
            if (animation == nullptr) {
                return false;
            }
            desired_face_.store(animation->face, std::memory_order_release);
        }
        if (std::strcmp(current_face_, animation->face) == 0) {
            desired_face_.store(animation->face, std::memory_order_release);
            return true;
        }
        if (requested_animation_.load(std::memory_order_acquire) == animation) {
            return true;
        }
        if (load_queue_ == nullptr) {
            return false;
        }

        const std::uint32_t sequence = requested_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
        requested_animation_.store(animation, std::memory_order_release);
        LoadRequest request{.animation = animation, .sequence = sequence};
        if (xQueueOverwrite(load_queue_, &request) != pdPASS) {
            requested_animation_.store(nullptr, std::memory_order_release);
            ESP_LOGE(kTag, "face load request failed: %s", animation->face);
            return false;
        }
        return true;
    }

    void update_frame_index(std::uint32_t now_ms)
    {
        if (asset_.frame_count == 0 || asset_.animation == nullptr || now_ms < next_frame_ms_) {
            return;
        }
        std::uint16_t next = static_cast<std::uint16_t>(frame_index_ + 1);
        if (next >= asset_.frame_count) {
            next = asset_.animation->looping ? 0 : static_cast<std::uint16_t>(asset_.frame_count - 1);
        }
        if (next != frame_index_ || asset_.animation->looping) {
            frame_index_ = next;
            frame_dirty_ = true;
        }
        next_frame_ms_ = now_ms + asset_.frame_ms;
    }

    void update_balloon(std::uint32_t now_ms)
    {
        if (!balloon_visible_ || balloon_done_) {
            return;
        }
        if (balloon_set_ms_ == 0) {
            balloon_set_ms_ = now_ms == 0 ? 1 : now_ms;
        }
        if (now_ms - balloon_set_ms_ >= balloon_hold_ms_) {
            balloon_done_ = true;
        }
    }

    std::uint16_t scene_fill_rgb565_for_y(std::int32_t y) const
    {
        const std::int32_t grass_h = std::max<std::int32_t>(10, height_ / 12);
        if (y >= height_ - grass_h) {
            const std::int32_t pos = y - (height_ - grass_h);
            return lerp_rgb565(0x2d4a2d, 0x1a331a, pos, grass_h - 1);
        }
        return lerp_rgb565(0x0a0e1a, 0x1a1a2e, y, height_ - grass_h - 1);
    }

    void draw_scene(avatar::RichCanvas& canvas) const
    {
        for (std::int32_t y = 0; y < height_; ++y) {
            canvas.fillRect(0, y, width_, 1, scene_fill_rgb565_for_y(y));
        }
        draw_stars(canvas);
        draw_grass_tufts(canvas);
    }

    void draw_stars(avatar::RichCanvas& canvas) const
    {
        struct Star {
            std::int32_t x;
            std::int32_t y;
            std::int32_t size;
            std::uint32_t color;
        };
        constexpr Star stars[] = {
            {10, 8, 2, 0xffff88},  {45, 15, 3, 0x88ccff},  {80, 22, 2, 0xffaa88},
            {120, 5, 4, 0xaaccff}, {150, 18, 2, 0xffdd88}, {160, 30, 3, 0x88ffcc},
        };
        for (const auto& star : stars) {
            const std::int32_t x = star.x * width_ / 160;
            const std::int32_t y = star.y * height_ / 240;
            const std::int32_t size = std::max<std::int32_t>(1, star.size * width_ / 320);
            canvas.fillCircle(x, y, size,
                              rgb565_from_888(color_r(star.color), color_g(star.color), color_b(star.color)));
        }
    }

    void draw_grass_tufts(avatar::RichCanvas& canvas) const
    {
        struct Tuft {
            std::int32_t x;
            std::int32_t w;
        };
        constexpr Tuft tufts[] = {{8, 3}, {25, 2}, {50, 4}, {78, 2}, {100, 3}, {130, 2}, {155, 3}};
        const std::int32_t grass_h = std::max<std::int32_t>(10, height_ / 12);
        const std::int32_t y = height_ - grass_h;
        const std::uint16_t color = rgb565_from_888(0x3d, 0x6a, 0x3d);
        for (const auto& tuft : tufts) {
            canvas.fillRect(tuft.x * width_ / 160, y, std::max<std::int32_t>(1, tuft.w * width_ / 160), 3, color);
        }
    }

    std::uint32_t frame_offset(std::uint16_t index) const
    {
        return read_le32(asset_.frame_offsets + static_cast<std::size_t>(index) * sizeof(std::uint32_t));
    }

    void draw_scaled_run(avatar::RichCanvas& canvas, std::uint16_t value, std::uint32_t src_index,
                         std::uint32_t count) const
    {
        const std::int32_t scaled_w = static_cast<std::int32_t>(asset_.width) * kScale;
        const std::int32_t scaled_h = static_cast<std::int32_t>(asset_.height) * kScale;
        const std::int32_t left = (width_ - scaled_w) / 2;
        const std::int32_t top = height_ + static_cast<std::int32_t>(asset_.y_offset) - scaled_h;

        std::uint32_t remaining = count;
        std::uint32_t index = src_index;
        while (remaining > 0) {
            const std::uint32_t src_y = index / asset_.width;
            const std::uint32_t src_x = index - src_y * asset_.width;
            const std::uint32_t chunk = std::min<std::uint32_t>(remaining, asset_.width - src_x);

            const std::int32_t x0 = left + static_cast<std::int32_t>(src_x) * kScale;
            const std::int32_t y0 = top + static_cast<std::int32_t>(src_y) * kScale;
            const std::int32_t x1 = x0 + static_cast<std::int32_t>(chunk) * kScale;
            const std::int32_t y1 = y0 + kScale;
            const std::int32_t clipped_x0 = std::max<std::int32_t>(0, x0);
            const std::int32_t clipped_y0 = std::max<std::int32_t>(0, y0);
            const std::int32_t clipped_x1 = std::min<std::int32_t>(width_, x1);
            const std::int32_t clipped_y1 = std::min<std::int32_t>(height_, y1);
            if (clipped_x1 > clipped_x0 && clipped_y1 > clipped_y0) {
                canvas.fillRect(clipped_x0, clipped_y0, clipped_x1 - clipped_x0, clipped_y1 - clipped_y0, value);
            }

            index += chunk;
            remaining -= chunk;
        }
    }

    void draw_direct_run(avatar::RichCanvas& canvas, std::uint16_t value, std::uint32_t src_index,
                         std::uint32_t count) const
    {
        const std::int32_t left = (width_ - static_cast<std::int32_t>(asset_.width)) / 2;
        const std::int32_t top = height_ + static_cast<std::int32_t>(asset_.y_offset) -
                                 static_cast<std::int32_t>(asset_.height);

        std::uint32_t remaining = count;
        std::uint32_t index = src_index;
        while (remaining > 0) {
            const std::uint32_t src_y = index / asset_.width;
            const std::uint32_t src_x = index - src_y * asset_.width;
            const std::uint32_t chunk = std::min<std::uint32_t>(remaining, asset_.width - src_x);

            const std::int32_t x0 = left + static_cast<std::int32_t>(src_x);
            const std::int32_t y0 = top + static_cast<std::int32_t>(src_y);
            const std::int32_t x1 = x0 + static_cast<std::int32_t>(chunk);
            const std::int32_t y1 = y0 + 1;
            const std::int32_t clipped_x0 = std::max<std::int32_t>(0, x0);
            const std::int32_t clipped_y0 = std::max<std::int32_t>(0, y0);
            const std::int32_t clipped_x1 = std::min<std::int32_t>(width_, x1);
            const std::int32_t clipped_y1 = std::min<std::int32_t>(height_, y1);
            if (clipped_x1 > clipped_x0 && clipped_y1 > clipped_y0) {
                canvas.fillRect(clipped_x0, clipped_y0, clipped_x1 - clipped_x0, clipped_y1 - clipped_y0, value);
            }

            index += chunk;
            remaining -= chunk;
        }
    }

    void draw_run(avatar::RichCanvas& canvas, std::uint16_t value, std::uint32_t src_index,
                  std::uint32_t count) const
    {
        if constexpr (kScale == 1) {
            draw_direct_run(canvas, value, src_index, count);
        } else {
            draw_scaled_run(canvas, value, src_index, count);
        }
    }

    void draw_current_frame(avatar::RichCanvas& canvas) const
    {
        if (asset_.frame_count == 0 || frame_index_ >= asset_.frame_count) {
            return;
        }
        const std::uint32_t start_word = frame_offset(frame_index_);
        const std::uint32_t end_word = frame_offset(static_cast<std::uint16_t>(frame_index_ + 1));
        if (start_word >= asset_.rle_word_count || end_word > asset_.rle_word_count || start_word >= end_word) {
            ESP_LOGE(kTag, "frame offset bad: %s frame=%u", asset_.animation->asset_name,
                     static_cast<unsigned>(frame_index_));
            return;
        }

        const std::uint8_t* rle = asset_.rle_data + static_cast<std::size_t>(start_word) * sizeof(std::uint16_t);
        const std::uint8_t* rle_end = asset_.rle_data + static_cast<std::size_t>(end_word) * sizeof(std::uint16_t);
        const std::uint32_t pixel_count = static_cast<std::uint32_t>(asset_.width) * asset_.height;
        std::uint32_t src_index = 0;
        while (src_index < pixel_count && rle + 3 < rle_end) {
            const std::uint16_t value = read_le16(rle);
            const std::uint16_t raw_count = read_le16(rle + sizeof(std::uint16_t));
            rle += sizeof(std::uint16_t) * 2;
            const std::uint32_t count = std::min<std::uint32_t>(raw_count, pixel_count - src_index);
            if (value != asset_.transparent_key && count > 0) {
                draw_run(canvas, value, src_index, count);
            }
            src_index += count;
        }
    }

    void draw_balloon(avatar::RichCanvas& canvas)
    {
        if (!balloon_visible_ || balloon_text_.empty()) {
            return;
        }
        const std::int32_t margin = 8;
        const std::int32_t box_h = std::min<std::int32_t>(54, height_ / 3);
        const std::int32_t y = height_ - box_h - margin;
        const std::uint16_t black = canvas.color565(0, 0, 0);
        const std::uint16_t white = canvas.color565(235, 235, 235);
        canvas.begin_group(margin, y, width_ - margin * 2, box_h);
        canvas.fillRoundRect(margin, y, width_ - margin * 2, box_h, 8, black);
        canvas.drawRoundRect(margin, y, width_ - margin * 2, box_h, 8, white);
        canvas.setClipRect(margin + 8, y + 4, width_ - margin * 2 - 16, box_h - 8);
        canvas.setTextDatum(lgfx::textdatum_t::middle_left);
        canvas.setTextColor(white, black);
        canvas.setFont(&fonts::efontCN_16);
        canvas.setTextSize(1);
        canvas.drawString(balloon_text_.c_str(), margin + 10, y + box_h / 2);
        canvas.clearClipRect();
        canvas.end_group();
    }

    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    bool ready_ = false;
    bool spiffs_registered_ = false;
    HeapBuffer asset_buffers_[2];
    AssetView asset_{};
    std::atomic<std::uint8_t> active_buffer_{0};
    AssetView pending_asset_{};
    std::uint8_t pending_buffer_ = 0;
    std::uint32_t pending_sequence_ = 0;
    std::atomic<bool> pending_asset_ready_{false};
    const char* current_face_ = "idle";
    std::atomic<const char*> desired_face_{"idle"};
    QueueHandle_t load_queue_ = nullptr;
    TaskHandle_t loader_task_ = nullptr;
    std::atomic<bool> stop_loader_{false};
    std::atomic<bool> loader_running_{false};
    std::atomic<const Animation*> requested_animation_{nullptr};
    std::atomic<std::uint32_t> requested_sequence_{0};
    std::uint16_t frame_index_ = 0;
    std::uint32_t next_frame_ms_ = 0;
    bool frame_dirty_ = true;
    float mouth_open_ = 0.0f;

    std::string balloon_text_;
    std::uint32_t balloon_hold_ms_ = 0;
    std::uint32_t balloon_set_ms_ = 0;
    bool balloon_visible_ = false;
    bool balloon_done_ = false;
};

Renderer::Renderer() : impl_{std::make_unique<Impl>()} {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

bool Renderer::begin(std::int32_t width, std::int32_t height)
{
    return impl_->begin(width, height);
}

bool Renderer::ready() const noexcept
{
    return impl_->ready();
}

void Renderer::set_expression(avatar::Expression expression) noexcept
{
    impl_->set_expression(expression);
}

void Renderer::set_mouth_open(float ratio) noexcept
{
    impl_->set_mouth_open(ratio);
}

void Renderer::set_balloon_text(std::string_view text, std::uint32_t hold_ms)
{
    impl_->set_balloon_text(text, hold_ms);
}

void Renderer::clear_balloon() noexcept
{
    impl_->clear_balloon();
}

bool Renderer::is_balloon_done() const noexcept
{
    return impl_->is_balloon_done();
}

void Renderer::request_full_repaint() noexcept
{
    impl_->request_full_repaint();
}

void Renderer::render(std::uint32_t now_ms, avatar::RichCanvas& canvas)
{
    impl_->render(now_ms, canvas);
}

} // namespace stackchan::clawd_face

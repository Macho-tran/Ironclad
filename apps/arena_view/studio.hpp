// SPDX-License-Identifier: MIT
//
// Replay Studio — time-travel debugger for ironclad .iclr recordings.
//
// Owns:
//   * The parsed `ReplayModel` (unowned reference; caller keeps the
//     model alive for the lifetime of the Studio).
//   * A `Replayer` that re-simulates frames on demand.
//   * Playback state: current frame, play/pause, lane viewport.
//
// The Studio renders four panels into an SDL window:
//   1. Arena view (neon player circles + projectiles for the current
//      scrubbed frame).
//   2. Top-bar diegetic stats (frame, hash, rollback, desync flag).
//   3. Rollback spike + event timeline (full-window scrubber,
//      coloured markers).
//   4. Per-player input lanes (one row per player, ~120 frames
//      visible, centred on the playhead).
//   5. Entity inspector column (player HP / score / position /
//      cooldowns; live projectile count).
//
// All glyphs come from the line-segment renderer in `render.hpp` —
// no font dependency, no asset pipeline.
#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <vector>

#include <ironclad/replay.hpp>
#include <ironclad/session.hpp>

#include "render.hpp"

namespace arena_view {

class Studio {
public:
    Studio(const ironclad::ReplayModel& model,
           ironclad::Session::WorldInit init,
           ironclad::Session::SimStep   step);

    /// One pass of input handling + rendering. Caller is responsible
    /// for the SDL_Renderer lifetime + Present.
    void frame(SDL_Renderer* renderer, double delta_seconds);

    /// True if the user requested exit (Esc / window close).
    [[nodiscard]] bool wants_quit() const noexcept { return quit_; }

    /// Notify the Studio of a single SDL event. Call from the host
    /// SDL_PollEvent loop before `frame()`.
    void handle_event(const SDL_Event& ev);

    /// Position the playhead at `frame`, clamped.
    void seek(std::uint32_t frame) noexcept;

    /// Render a single frame to a freshly-cleared renderer and
    /// return — used by `--screenshot` mode.
    void render_to(SDL_Renderer* renderer);

    [[nodiscard]] std::uint32_t playhead() const noexcept { return playhead_; }

private:
    void handle_keys();
    void render_top_bar(SDL_Renderer*);
    void render_arena(SDL_Renderer*);
    void render_inspector(SDL_Renderer*);
    void render_timeline(SDL_Renderer*);
    void render_input_lanes(SDL_Renderer*);
    void render_help(SDL_Renderer*);

    const ironclad::ReplayModel& model_;
    ironclad::Replayer           replayer_;

    std::uint32_t  playhead_  = 0;
    bool           playing_   = false;
    bool           quit_      = false;
    double         play_accum_ = 0.0;        // seconds; advances playhead at tick_hz

    // For step-by-key smoothing.
    Uint64 last_key_ms_ = 0;

    // Layout — the full studio window is wider than the live arena
    // view since we have lanes + inspector.
    Layout layout_;
    int    arena_w_         = 0;
    int    arena_h_         = 0;
    int    inspector_x_     = 0;
    int    inspector_w_     = 0;
    int    timeline_y_      = 0;
    int    timeline_h_      = 56;
    int    lane_y_          = 0;
    int    lane_h_per_player = 28;
    int    lane_visible_frames_ = 121;   // odd so playhead is centred
};

}  // namespace arena_view

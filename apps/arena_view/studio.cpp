// SPDX-License-Identifier: MIT
#include "studio.hpp"

#include <algorithm>
#include <cstdio>

#include <ironclad/components.hpp>

namespace arena_view {

namespace {

// Bucket a rollback distance into a pixel-height multiplier that
// keeps the timeline readable: tiny rollbacks should be visible but
// not dominate, big spikes should clearly stand out.
int rollback_bar_height(std::uint8_t distance, int max_h) {
    if (distance == 0) return 0;
    // sqrt-style mapping so 1 frame is visible and 32+ saturate.
    const int saturate = 16;
    int d = distance > saturate ? saturate : distance;
    return std::max(2, (d * max_h) / saturate);
}

}  // namespace

Studio::Studio(const ironclad::ReplayModel& model,
               ironclad::Session::WorldInit init,
               ironclad::Session::SimStep   step)
    : model_(model),
      replayer_(model, std::move(init), std::move(step), /*ckpt=*/30) {
    // Studio window dimensions. We need:
    //   * enough horizontal space for: arena (square), gap, inspector
    //   * enough vertical space for: title bar, arena, timeline, lanes
    layout_.window_w = 1280;
    layout_.window_h = 800;
    layout_.arena_x  = 24;
    layout_.arena_y  = 80;          // leave 80 px for top bar
    layout_.arena_px = 480;         // a hair smaller so inspector fits
    arena_w_         = layout_.arena_px;
    arena_h_         = layout_.arena_px;
    inspector_x_     = layout_.arena_x + arena_w_ + 32;
    inspector_w_     = layout_.window_w - inspector_x_ - 24;

    // Timeline lives below the arena. Input lanes below that.
    timeline_y_      = layout_.arena_y + arena_h_ + 28;
    lane_y_          = timeline_y_ + timeline_h_ + 24;

    playhead_ = 0;
    // Pre-warm the world for frame 0 so first render is correct.
    (void)replayer_.world_at(0);
}

void Studio::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_QUIT) { quit_ = true; return; }
    if (ev.type == SDL_KEYDOWN) {
        switch (ev.key.keysym.sym) {
            case SDLK_ESCAPE: quit_ = true; break;
            case SDLK_SPACE:  playing_ = !playing_; break;
            case SDLK_HOME:   seek(0); break;
            case SDLK_END:    seek(model_.record_count()); break;
            case SDLK_LEFTBRACKET: {
                // Jump to previous rollback event.
                if (playhead_ > 0) {
                    auto i = model_.prev_event_index(playhead_ - 1, true);
                    if (i < model_.events().size()) {
                        seek(model_.events()[i].frame);
                    }
                }
                break;
            }
            case SDLK_RIGHTBRACKET: {
                auto i = model_.next_event_index(playhead_ + 1, true);
                if (i < model_.events().size()) {
                    seek(model_.events()[i].frame);
                }
                break;
            }
            default: break;
        }
    }
    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        // Click on the timeline to scrub.
        int mx = ev.button.x;
        int my = ev.button.y;
        if (my >= timeline_y_ && my < timeline_y_ + timeline_h_ &&
            mx >= layout_.arena_x &&
            mx <  layout_.arena_x + layout_.window_w - 2 * layout_.arena_x) {
            const int tw = layout_.window_w - 2 * layout_.arena_x;
            const std::uint32_t fc = model_.record_count();
            if (fc > 0) {
                std::uint32_t f = static_cast<std::uint32_t>(
                    static_cast<double>(mx - layout_.arena_x) /
                    static_cast<double>(tw) * fc);
                seek(f);
            }
        }
    }
}

void Studio::handle_keys() {
    const Uint64 now = SDL_GetTicks64();
    if (now - last_key_ms_ < 70) return;          // ~14 Hz repeat
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    bool stepped = false;
    int delta = 1;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) delta = 60;
    if (keys[SDL_SCANCODE_LEFT])  { seek(playhead_ > static_cast<std::uint32_t>(delta)
                                         ? playhead_ - static_cast<std::uint32_t>(delta) : 0);
                                    stepped = true; }
    if (keys[SDL_SCANCODE_RIGHT]) { seek(playhead_ + static_cast<std::uint32_t>(delta));
                                    stepped = true; }
    if (stepped) last_key_ms_ = now;
}

void Studio::seek(std::uint32_t f) noexcept {
    if (f > model_.record_count()) f = model_.record_count();
    playhead_ = f;
    play_accum_ = 0.0;
}

void Studio::frame(SDL_Renderer* renderer, double delta_seconds) {
    handle_keys();
    if (playing_) {
        play_accum_ += delta_seconds;
        const double tick = 1.0 / std::max<std::uint16_t>(model_.header().tick_hz, 1);
        while (play_accum_ >= tick && playhead_ < model_.record_count()) {
            ++playhead_;
            play_accum_ -= tick;
        }
        if (playhead_ >= model_.record_count()) playing_ = false;
    }
    render_to(renderer);
}

void Studio::render_to(SDL_Renderer* renderer) {
    set_color(renderer, kBg);
    SDL_RenderClear(renderer);

    render_top_bar(renderer);
    render_arena(renderer);
    render_inspector(renderer);
    render_timeline(renderer);
    render_input_lanes(renderer);
    render_help(renderer);
}

void Studio::render_top_bar(SDL_Renderer* r) {
    // Title at scale=2 keeps it ~176 px wide and avoids cramping the
    // 1280-wide window. The inspector column on the right hosts the
    // frame counter / hash / event banner.
    set_color(r, kCyan);
    draw_text(r, layout_.arena_x, 18, "IRONCLAD REPLAY STUDIO", 2);

    const std::size_t idx = static_cast<std::size_t>(
        std::min<std::uint32_t>(playhead_, model_.record_count() ? model_.record_count() - 1 : 0));
    if (model_.record_count() == 0 || idx >= model_.records().size()) return;
    const auto& rec = model_.records()[idx];
    char buf[128];

    // Frame counter centre.
    set_color(r, kCyan);
    std::snprintf(buf, sizeof(buf), "FRAME %u / %u",
                  rec.frame, model_.record_count());
    draw_text(r, layout_.arena_x + 280, 18, buf, 2);

    // Hash, right-side of the title bar.
    std::snprintf(buf, sizeof(buf), "HASH %016llX",
                  static_cast<unsigned long long>(rec.hash));
    draw_text(r, layout_.arena_x + 540, 18, buf, 2);

    // Optional event banner below the title.
    int row2_x = layout_.arena_x + 280;
    if (rec.rollback > 0) {
        set_color(r, kAmber);
        std::snprintf(buf, sizeof(buf), "ROLLBACK %u F", rec.rollback);
        draw_text(r, row2_x, 42, buf, 2);
        row2_x += 200;
    }
    if (rec.flags & ironclad::ReplayRecord::kFlagDesync) {
        set_color(r, kRed);
        draw_text(r, row2_x, 42, "DESYNC", 2);
    }
}

void Studio::render_arena(SDL_Renderer* r) {
    // Arena outline.
    set_color(r, kCyan);
    stroke_rect(r, layout_.arena_x - 1, layout_.arena_y - 1,
                arena_w_ + 2, arena_h_ + 2);

    // Update layout in renderer for sim_to_px helpers.
    Layout L = layout_;

    // Re-simulate (or use cached) world for the playhead.
    const std::uint32_t want = std::min<std::uint32_t>(playhead_, model_.record_count());
    const ironclad::World& w = replayer_.world_at(want);

    // Players.
    w.each<ironclad::Player>([&](ironclad::Entity e, const ironclad::Player& pl) {
        auto* tr = w.get<ironclad::Transform>(e);
        if (!tr) return;
        const int cx = L.sim_to_px_x(tr->pos.x.to_double());
        const int cy = L.sim_to_px_y(tr->pos.y.to_double());
        const auto& col = player_color(pl.id);
        const int radius = pl.alive ? 14 : 8;
        draw_glow_circle(r, cx, cy, radius, col.r, col.g, col.b);
    });

    // Projectiles.
    w.each<ironclad::Projectile>([&](ironclad::Entity e, const ironclad::Projectile&) {
        auto* tr = w.get<ironclad::Transform>(e);
        if (!tr) return;
        const int cx = L.sim_to_px_x(tr->pos.x.to_double());
        const int cy = L.sim_to_px_y(tr->pos.y.to_double());
        draw_glow_circle(r, cx, cy, 3, 255, 255, 255);
    });
}

void Studio::render_inspector(SDL_Renderer* r) {
    // Heading.
    set_color(r, kCyan);
    draw_text(r, inspector_x_, layout_.arena_y - 14, "ENTITY INSPECTOR", 2);
    stroke_rect(r, inspector_x_ - 8, layout_.arena_y - 4,
                inspector_w_ + 8, arena_h_ - 4);

    const std::uint32_t want = std::min<std::uint32_t>(playhead_, model_.record_count());
    const ironclad::World& w = replayer_.world_at(want);

    int row_y = layout_.arena_y + 8;
    char buf[128];

    // Players.
    int proj_count = 0;
    w.each<ironclad::Projectile>([&](ironclad::Entity, const ironclad::Projectile&) {
        ++proj_count;
    });

    w.each<ironclad::Player>([&](ironclad::Entity e, const ironclad::Player& pl) {
        const auto* tr  = w.get<ironclad::Transform>(e);
        const auto* vel = w.get<ironclad::Velocity>(e);
        const auto& col = player_color(pl.id);
        set_color(r, col);
        std::snprintf(buf, sizeof(buf), "P%u   HP %d   SCORE %u",
                      static_cast<unsigned>(pl.id),
                      pl.hp.to_int(),
                      pl.score);
        draw_text(r, inspector_x_, row_y, buf, 2);
        row_y += 16;
        if (tr) {
            set_color(r, kWhite);
            std::snprintf(buf, sizeof(buf), "  POS %.2f , %.2f",
                          tr->pos.x.to_double(), tr->pos.y.to_double());
            draw_text(r, inspector_x_, row_y, buf, 2);
            row_y += 16;
        }
        if (vel) {
            // Use a darker but still-legible grey for the velocity /
            // cooldown sub-line.
            Color sub{120, 120, 130, 255};
            set_color(r, sub);
            std::snprintf(buf, sizeof(buf), "  VEL %.2f , %.2f   CD %u / %u",
                          vel->v.x.to_double(), vel->v.y.to_double(),
                          static_cast<unsigned>(pl.dash_cd),
                          static_cast<unsigned>(pl.hit_cd));
            draw_text(r, inspector_x_, row_y, buf, 2);
            row_y += 14;
        }
        if (!pl.alive) {
            set_color(r, kRed);
            draw_text(r, inspector_x_, row_y, "  KO", 2);
            row_y += 14;
        }
        row_y += 8;
    });

    set_color(r, kCyan);
    std::snprintf(buf, sizeof(buf), "PROJECTILES %d", proj_count);
    draw_text(r, inspector_x_, row_y + 8, buf, 2);

    // Summary stats from the model.
    const auto& s = model_.stats();
    int stats_y = layout_.arena_y + arena_h_ - 110;
    set_color(r, kCyan);
    draw_text(r, inspector_x_, stats_y, "REPLAY SUMMARY", 2);
    set_color(r, kWhite);
    std::snprintf(buf, sizeof(buf), "FRAMES %u",   s.frame_count);
    draw_text(r, inspector_x_, stats_y + 18, buf, 2);
    std::snprintf(buf, sizeof(buf), "ROLLBACKS %u", s.rollback_event_count);
    draw_text(r, inspector_x_, stats_y + 34, buf, 2);
    std::snprintf(buf, sizeof(buf), "MAX %u F  AVG %.2f",
                  static_cast<unsigned>(s.max_rollback_frames),
                  s.avg_rollback_frames);
    draw_text(r, inspector_x_, stats_y + 50, buf, 2);
    std::snprintf(buf, sizeof(buf), "DESYNCS %u", s.desync_event_count);
    set_color(r, s.desync_event_count > 0 ? kRed : kWhite);
    draw_text(r, inspector_x_, stats_y + 66, buf, 2);
}

void Studio::render_timeline(SDL_Renderer* r) {
    const int x0 = layout_.arena_x;
    const int w  = layout_.window_w - 2 * layout_.arena_x;
    const int y0 = timeline_y_;
    const int h  = timeline_h_;

    // Background.
    set_color(r, kDimGrey);
    fill_rect(r, x0, y0, w, h);
    set_color(r, kCyan);
    stroke_rect(r, x0, y0, w, h);
    draw_text(r, x0, y0 - 16, "ROLLBACK SPIKES", 2);

    if (model_.record_count() == 0) return;

    const double pxf = static_cast<double>(w) /
                       static_cast<double>(model_.record_count());

    // Per-frame rollback bars + desync flags. Walking every record
    // keeps this O(n); for typical 1-10k frame replays that's
    // negligible per draw.
    for (std::size_t i = 0; i < model_.records().size(); ++i) {
        const auto& rec = model_.records()[i];
        if (rec.rollback == 0 && rec.flags == 0) continue;
        const int xi = x0 + static_cast<int>(static_cast<double>(i) * pxf);
        if (rec.flags & ironclad::ReplayRecord::kFlagDesync) {
            set_color(r, kRed);
            fill_rect(r, xi, y0, std::max(1, static_cast<int>(pxf) + 1), h);
        } else {
            const int bar_h = rollback_bar_height(rec.rollback, h - 4);
            set_color(r, kAmber);
            fill_rect(r, xi, y0 + h - bar_h - 2,
                      std::max(1, static_cast<int>(pxf) + 1), bar_h);
        }
    }

    // Playhead.
    const int px_play = x0 + static_cast<int>(
        static_cast<double>(playhead_) * pxf);
    set_color(r, kHotPink);
    SDL_RenderDrawLine(r, px_play, y0 - 4, px_play, y0 + h + 4);

    // Frame counter under timeline.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u / %u",
                  playhead_, model_.record_count());
    set_color(r, kCyan);
    draw_text(r, x0, y0 + h + 6, buf, 2);
}

void Studio::render_input_lanes(SDL_Renderer* r) {
    if (model_.record_count() == 0) return;
    const int x0 = layout_.arena_x;
    // Reserve a small gutter on the left for per-row labels.
    const int label_gutter = 32;
    const int w  = layout_.window_w - 2 * layout_.arena_x - label_gutter;
    const int half = lane_visible_frames_ / 2;

    // Centre window on the playhead. Cells are int-sized.
    const int cell_w = std::max(2, w / lane_visible_frames_);
    const int total_w = cell_w * lane_visible_frames_;
    const int origin_x = x0 + label_gutter + (w - total_w) / 2;

    const std::uint8_t np = model_.header().num_players;

    set_color(r, kCyan);
    draw_text(r, x0, lane_y_ - 18, "PLAYER INPUT LANES", 2);

    for (std::uint8_t p = 0; p < np; ++p) {
        const int row_y = lane_y_ + p * lane_h_per_player;
        const auto& col = player_color(p);
        set_color(r, col);
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "P%u", static_cast<unsigned>(p));
        draw_text(r, x0, row_y + 4, lbl, 2);

        // Background lane.
        set_color(r, kDimGrey);
        fill_rect(r, origin_x, row_y, total_w, lane_h_per_player - 4);

        for (int j = -half; j <= half; ++j) {
            const int rel = j + half;     // 0..lane_visible_frames_-1
            const std::int64_t f = static_cast<std::int64_t>(playhead_) + j;
            if (f < 0 || f >= static_cast<std::int64_t>(model_.record_count())) continue;
            const auto& rec = model_.records()[static_cast<std::size_t>(f)];
            const auto& in  = rec.inputs[p];
            const int cx = origin_x + rel * cell_w;
            const int cy = row_y;
            const int cw = cell_w;
            const int ch = lane_h_per_player - 4;

            // Movement-direction tint.
            Color tint = kDimGrey;
            if (in.move_x != 0 || in.move_y != 0) tint = col;
            set_color(r, tint);
            fill_rect(r, cx + 1, cy + 1, cw - 1, ch - 2);

            // Buttons: small accents.
            if (in.attack()) {
                set_color(r, kHotPink);
                fill_rect(r, cx + 1, cy + 1, cw - 1, 3);
            }
            if (in.dash()) {
                set_color(r, kAmber);
                fill_rect(r, cx + 1, cy + ch - 4, cw - 1, 3);
            }

            // Predicted-vs-canonical mismatch flag for this player on
            // this frame: small red corner ticks (pred_diff bit P).
            if (rec.pred_diff & (1u << p)) {
                set_color(r, kRed);
                fill_rect(r, cx + cw - 3, cy + 1, 2, 2);
            }
        }

        // Lane border.
        set_color(r, kCyan);
        stroke_rect(r, origin_x, row_y, total_w, lane_h_per_player - 4);
    }

    // Centre playhead marker.
    const int center_x = origin_x + (lane_visible_frames_ / 2) * cell_w + cell_w / 2;
    set_color(r, kHotPink);
    SDL_RenderDrawLine(r, center_x,
                       lane_y_ - 4,
                       center_x,
                       lane_y_ + np * lane_h_per_player);
}

void Studio::render_help(SDL_Renderer* r) {
    set_color(r, kDimGrey);
    int y = layout_.window_h - 22;
    draw_text(r, layout_.arena_x, y,
              "SPACE PLAY/PAUSE  < > STEP  SHIFT+< > JUMP 60  "
              "[ ] PREV/NEXT EVENT  HOME/END  ESC QUIT", 2);
}

}  // namespace arena_view

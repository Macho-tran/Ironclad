// SPDX-License-Identifier: MIT
//
// arena_view: SDL2-based visual front-end for the ironclad arena demo.
//
// This is the *stretch* deliverable: an interactive client where one
// of the four sessions is "ours" (player 0). The other three players
// are AI-driven exactly like in the headless arena_demo, so we share
// the same step / init / ai functions.
//
// Visual style ("arcade-tech"):
//   * Black background, neon vector strokes for arena boundary,
//     players (circles), and projectiles (small circles).
//   * "Glass shard" rollback effect: when the local session
//     reports `last_rollback_frames > 0`, we spawn purely-cosmetic
//     shard particles (a *view-layer* concept; the simulation
//     doesn't know they exist).
//   * Diegetic stats overlay drawn with line-segment digits so we
//     don't need a TTF font dependency.
//
// This file is only compiled when `IRONCLAD_BUILD_SDL_DEMO=ON`.
// Without a display (e.g. CI), it's skipped entirely.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include <ironclad/components.hpp>
#include <ironclad/loopback_transport.hpp>
#include <ironclad/session.hpp>

#include "../arena_demo/arena.hpp"

namespace {

constexpr int kWindowW = 960;
constexpr int kWindowH = 720;
constexpr int kArenaPx = 600;          // square arena inside the window
constexpr int kArenaX  = (kWindowW - kArenaPx) / 2;
constexpr int kArenaY  = (kWindowH - kArenaPx) / 2;
// Sim units are roughly [-12, +12]; map that to the arena rectangle.
constexpr double kSimRange = 24.0;

struct GlassShard {
    double x, y;       // pixel coords
    double vx, vy;
    double life;       // 0..1 fade
};

struct BorrowedTransport : ironclad::ITransport {
    ironclad::ITransport* inner;
    explicit BorrowedTransport(ironclad::ITransport* t) : inner(t) {}
    void send(std::uint8_t to, std::span<const std::uint8_t> b) override {
        inner->send(to, b);
    }
    std::optional<ironclad::RecvPacket> recv() override { return inner->recv(); }
    void poll() override { inner->poll(); }
};

void draw_circle(SDL_Renderer* r, int cx, int cy, int radius) {
    // Midpoint circle algorithm — good enough for the neon look.
    int x = radius;
    int y = 0;
    int err = 1 - x;
    while (x >= y) {
        SDL_RenderDrawPoint(r, cx + x, cy + y);
        SDL_RenderDrawPoint(r, cx + y, cy + x);
        SDL_RenderDrawPoint(r, cx - y, cy + x);
        SDL_RenderDrawPoint(r, cx - x, cy + y);
        SDL_RenderDrawPoint(r, cx - x, cy - y);
        SDL_RenderDrawPoint(r, cx - y, cy - x);
        SDL_RenderDrawPoint(r, cx + y, cy - x);
        SDL_RenderDrawPoint(r, cx + x, cy - y);
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x + 1); }
    }
}

void draw_glow_circle(SDL_Renderer* r, int cx, int cy, int radius,
                      Uint8 R, Uint8 G, Uint8 B) {
    // 3-pass glow: outer fainter rings, inner solid.
    SDL_SetRenderDrawColor(r, R, G, B, 64);
    draw_circle(r, cx, cy, radius + 3);
    SDL_SetRenderDrawColor(r, R, G, B, 128);
    draw_circle(r, cx, cy, radius + 1);
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    draw_circle(r, cx, cy, radius);
}

inline int sim_to_px_x(double v) {
    return kArenaX + static_cast<int>((v + kSimRange / 2.0) /
                                      kSimRange * kArenaPx);
}
inline int sim_to_px_y(double v) {
    return kArenaY + kArenaPx -
           static_cast<int>((v + kSimRange / 2.0) / kSimRange * kArenaPx);
}

// --- Tiny line-segment digit renderer for the diegetic overlay ----------
// Each character is drawn from a 3x5 grid of line segments.
void draw_text(SDL_Renderer* r, int x, int y, const char* text,
               int scale = 2) {
    auto draw_seg = [&](int x0, int y0, int x1, int y1) {
        SDL_RenderDrawLine(r, x + x0 * scale, y + y0 * scale,
                              x + x1 * scale, y + y1 * scale);
    };
    for (int i = 0; text[i]; ++i) {
        char c = text[i];
        int ox = i * 4 * scale;
        auto seg = [&](int x0, int y0, int x1, int y1) {
            draw_seg(ox + x0, y0, ox + x1, y1);
        };
        // Implementation note: this is intentionally minimal;
        // a few glyphs cover what the overlay actually prints
        // (digits, a few letters, a colon and a space).
        switch (c) {
            case '0': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,4); seg(0,4,2,4); break;
            case '1': seg(2,0,2,4); break;
            case '2': seg(0,0,2,0); seg(2,0,2,2); seg(0,2,2,2); seg(0,2,0,4); seg(0,4,2,4); break;
            case '3': seg(0,0,2,0); seg(2,0,2,4); seg(0,2,2,2); seg(0,4,2,4); break;
            case '4': seg(0,0,0,2); seg(2,0,2,4); seg(0,2,2,2); break;
            case '5': seg(0,0,2,0); seg(0,0,0,2); seg(0,2,2,2); seg(2,2,2,4); seg(0,4,2,4); break;
            case '6': seg(0,0,2,0); seg(0,0,0,4); seg(0,2,2,2); seg(2,2,2,4); seg(0,4,2,4); break;
            case '7': seg(0,0,2,0); seg(2,0,2,4); break;
            case '8': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,4); seg(0,2,2,2); seg(0,4,2,4); break;
            case '9': seg(0,0,2,0); seg(0,0,0,2); seg(2,0,2,4); seg(0,2,2,2); seg(0,4,2,4); break;
            case 'A': seg(0,4,1,0); seg(1,0,2,4); seg(0,3,2,3); break;
            case 'B': seg(0,0,0,4); seg(0,0,2,1); seg(2,1,0,2); seg(0,2,2,3); seg(2,3,0,4); break;
            case 'D': seg(0,0,0,4); seg(0,0,2,1); seg(2,1,2,3); seg(2,3,0,4); break;
            case 'E': seg(0,0,2,0); seg(0,0,0,4); seg(0,2,2,2); seg(0,4,2,4); break;
            case 'F': seg(0,0,2,0); seg(0,0,0,4); seg(0,2,2,2); break;
            case 'I': seg(0,0,2,0); seg(1,0,1,4); seg(0,4,2,4); break;
            case 'L': seg(0,0,0,4); seg(0,4,2,4); break;
            case 'M': seg(0,0,0,4); seg(0,0,1,2); seg(1,2,2,0); seg(2,0,2,4); break;
            case 'N': seg(0,0,0,4); seg(0,0,2,4); seg(2,0,2,4); break;
            case 'O': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,4); seg(0,4,2,4); break;
            case 'P': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,2); seg(0,2,2,2); break;
            case 'R': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,2); seg(0,2,2,2); seg(0,2,2,4); break;
            case 'S': seg(0,0,2,0); seg(0,0,0,2); seg(0,2,2,2); seg(2,2,2,4); seg(0,4,2,4); break;
            case 'T': seg(0,0,2,0); seg(1,0,1,4); break;
            case 'U': seg(0,0,0,4); seg(2,0,2,4); seg(0,4,2,4); break;
            case 'Y': seg(0,0,1,2); seg(2,0,1,2); seg(1,2,1,4); break;
            case ':': SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 1*scale);
                       SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 3*scale); break;
            case '.': SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 4*scale); break;
            case '%': seg(0,0,0,1); seg(2,3,2,4); seg(0,4,2,0); break;
            case '/': seg(0,4,2,0); break;
            case '=': seg(0,1,2,1); seg(0,3,2,3); break;
            case ' ': default: break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    arena_demo::Options opts;
    opts.frames      = 60u * 60u * 5u;   // 5 minutes at 60 Hz
    opts.num_players = 4;
    opts.rtt_ms      = 100;
    opts.jitter_ms   = 20;
    opts.loss_pct    = 3;
    opts.reorder_pct = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--rtt-ms" && i + 1 < argc) {
            opts.rtt_ms = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (std::string(argv[i]) == "--loss-pct" && i + 1 < argc) {
            opts.loss_pct = static_cast<std::uint8_t>(std::atoi(argv[++i]));
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("IRONCLAD arena_view",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWindowW, kWindowH, SDL_WINDOW_SHOWN);
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                                   SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // Build sessions exactly like the headless demo.
    constexpr std::uint16_t kHz = ironclad::kDefaultTickHz;
    ironclad::NetSimConfig nc;
    nc.latency_ticks = static_cast<std::uint16_t>((opts.rtt_ms * kHz + 1999u) / 2000u);
    nc.jitter_ticks  = static_cast<std::uint16_t>((opts.jitter_ms * kHz + 1999u) / 2000u);
    nc.loss_pct      = opts.loss_pct;
    nc.reorder_pct   = opts.reorder_pct;
    nc.seed          = opts.seed ^ 0xA5A5A5A5A5A5A5A5ULL;

    ironclad::LoopbackHub hub(opts.num_players, nc);
    std::vector<std::unique_ptr<ironclad::Session>> sessions(opts.num_players);
    for (std::uint8_t p = 0; p < opts.num_players; ++p) {
        ironclad::SessionConfig sc;
        sc.num_players  = opts.num_players;
        sc.local_player = p;
        sc.tick_hz      = kHz;
        sc.seed         = opts.seed;
        sessions[p] = std::make_unique<ironclad::Session>(sc,
            std::make_unique<BorrowedTransport>(hub.transport(p)),
            arena_demo::init_arena, arena_demo::step_arena);
    }

    std::vector<GlassShard> shards;
    std::mt19937 vfx_rng{0xC0FFEEu};      // VIEW-only, not the sim RNG

    bool quit = false;
    Uint64 next_tick_ms = SDL_GetTicks64();
    constexpr Uint64 kTickPeriodMs = 1000 / kHz;
    std::uint32_t frame = 0;

    while (!quit && frame < opts.frames) {
        SDL_Event ev;
        ironclad::PlayerInput local{};
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) quit = true;
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) local.move_x = -127;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) local.move_x =  127;
        if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) local.move_y =  127;
        if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) local.move_y = -127;
        if (keys[SDL_SCANCODE_SPACE]) local.buttons |= ironclad::PlayerInput::kAttack;
        if (keys[SDL_SCANCODE_LSHIFT]) local.buttons |= ironclad::PlayerInput::kDash;

        // Tick all sessions; player 0 is "us", others are AI.
        const Uint64 now = SDL_GetTicks64();
        if (now >= next_tick_ms) {
            for (std::uint8_t p = 0; p < opts.num_players; ++p) {
                if (p == 0) sessions[p]->tick(local);
                else        sessions[p]->tick(arena_demo::ai_input(frame, p));
            }
            hub.advance_tick();
            ++frame;
            next_tick_ms += kTickPeriodMs;

            // Spawn glass shards proportional to rollback distance.
            const auto& s = sessions[0]->stats();
            int n = std::min<int>(60, s.last_rollback_frames * 5);
            for (int i = 0; i < n; ++i) {
                GlassShard sh;
                sh.x = sim_to_px_x(0.0);
                sh.y = sim_to_px_y(0.0);
                sh.vx = static_cast<double>(vfx_rng() % 800) / 100.0 - 4.0;
                sh.vy = static_cast<double>(vfx_rng() % 800) / 100.0 - 4.0;
                sh.life = 1.0;
                shards.push_back(sh);
            }
        }

        // ---- Render --------------------------------------------------
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        // Arena outline (cyan).
        SDL_SetRenderDrawColor(ren, 0, 220, 255, 255);
        SDL_Rect arena{kArenaX, kArenaY, kArenaPx, kArenaPx};
        SDL_RenderDrawRect(ren, &arena);

        // Players + projectiles, drawn from player 0's session view.
        const ironclad::World& w = sessions[0]->world();
        w.each<ironclad::Player>([&](ironclad::Entity e, const ironclad::Player& pl) {
            (void)pl;
            auto* tr = w.get<ironclad::Transform>(e);
            if (!tr) return;
            const int cx = sim_to_px_x(tr->pos.x.to_double());
            const int cy = sim_to_px_y(tr->pos.y.to_double());
            // Distinct neon palette per player.
            static const Uint8 palette[4][3] = {
                {255,  64, 128},   // pink
                { 64, 220, 255},   // cyan
                {255, 220,  64},   // yellow
                {128, 255, 128},   // green
            };
            const auto* col = palette[pl.id % 4];
            draw_glow_circle(ren, cx, cy, 14, col[0], col[1], col[2]);
        });
        w.each<ironclad::Projectile>([&](ironclad::Entity e, const ironclad::Projectile&) {
            auto* tr = w.get<ironclad::Transform>(e);
            if (!tr) return;
            const int cx = sim_to_px_x(tr->pos.x.to_double());
            const int cy = sim_to_px_y(tr->pos.y.to_double());
            draw_glow_circle(ren, cx, cy, 3, 255, 255, 255);
        });

        // Glass shards (rollback VFX).
        for (auto& s : shards) {
            s.x += s.vx; s.y += s.vy; s.life -= 0.02;
        }
        shards.erase(std::remove_if(shards.begin(), shards.end(),
            [](const GlassShard& s){ return s.life <= 0.0; }), shards.end());
        for (const auto& s : shards) {
            Uint8 a = static_cast<Uint8>(std::clamp(s.life * 255.0, 0.0, 255.0));
            SDL_SetRenderDrawColor(ren, 255, 255, 255, a);
            SDL_RenderDrawPoint(ren, static_cast<int>(s.x), static_cast<int>(s.y));
            SDL_RenderDrawPoint(ren, static_cast<int>(s.x + 1), static_cast<int>(s.y));
            SDL_RenderDrawPoint(ren, static_cast<int>(s.x), static_cast<int>(s.y + 1));
        }

        // Diegetic overlay: top-left.
        const auto& st = sessions[0]->stats();
        char buf[128];
        SDL_SetRenderDrawColor(ren, 0, 220, 255, 255);
        std::snprintf(buf, sizeof(buf), "FRAME %u",         st.current_frame);
        draw_text(ren, kArenaX, kArenaY - 32, buf);
        std::snprintf(buf, sizeof(buf), "RTT %ums",         opts.rtt_ms);
        draw_text(ren, kArenaX + 200, kArenaY - 32, buf);
        std::snprintf(buf, sizeof(buf), "ROLLBACK %uF",
                      static_cast<unsigned>(st.last_rollback_frames));
        draw_text(ren, kArenaX + 360, kArenaY - 32, buf);
        std::snprintf(buf, sizeof(buf), "DESYNC %s", st.desync_detected ? "YES" : "NO");
        draw_text(ren, kArenaX, kArenaY + kArenaPx + 10, buf);

        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

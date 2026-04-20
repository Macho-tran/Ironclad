// SPDX-License-Identifier: MIT
//
// arena_view: SDL2 visual front-end for the ironclad arena demo,
// with two operating modes:
//
//   * Live mode (default): runs four sessions over a LoopbackHub
//     and renders player 0's view. Glass-shard VFX on rollback.
//
//   * Replay Studio mode (`--replay PATH`): loads an .iclr file,
//     opens the time-travel debugger UI (timeline + input lanes +
//     entity inspector). See apps/arena_view/studio.{hpp,cpp}.
//
// Optional screenshot mode (`--screenshot OUT.bmp --frame N`)
// renders a single replay frame and writes it to disk via
// SDL_SaveBMP. Useful for headless / CI documentation.
//
// This binary is only compiled when `IRONCLAD_BUILD_SDL_DEMO=ON`.
#include <SDL2/SDL.h>

#if defined(IRONCLAD_HAS_IMGUI)
#  include <imgui.h>
#  include <imgui_impl_sdl2.h>
#  include <imgui_impl_sdlrenderer2.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <ironclad/components.hpp>
#include <ironclad/loopback_transport.hpp>
#include <ironclad/replay.hpp>
#include <ironclad/session.hpp>

#include "../arena_demo/arena.hpp"
#include "render.hpp"
#include "studio.hpp"

namespace {

constexpr int kLiveWindowW = 960;
constexpr int kLiveWindowH = 720;

struct GlassShard {
    double x, y;
    double vx, vy;
    double life;
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

int run_live(const arena_demo::Options& opts) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("IRONCLAD arena_view (live)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kLiveWindowW, kLiveWindowH, SDL_WINDOW_SHOWN);
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                                   SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

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

    arena_view::Layout L;
    L.window_w = kLiveWindowW;
    L.window_h = kLiveWindowH;
    L.arena_x  = (kLiveWindowW - 600) / 2;
    L.arena_y  = (kLiveWindowH - 600) / 2;
    L.arena_px = 600;

    std::vector<GlassShard> shards;
    std::mt19937 vfx_rng{0xC0FFEEu};

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

        const Uint64 now = SDL_GetTicks64();
        if (now >= next_tick_ms) {
            for (std::uint8_t p = 0; p < opts.num_players; ++p) {
                if (p == 0) sessions[p]->tick(local);
                else        sessions[p]->tick(arena_demo::ai_input(frame, p));
            }
            hub.advance_tick();
            ++frame;
            next_tick_ms += kTickPeriodMs;

            const auto& s = sessions[0]->stats();
            int n = std::min<int>(60, s.last_rollback_frames * 5);
            for (int i = 0; i < n; ++i) {
                GlassShard sh;
                sh.x = L.sim_to_px_x(0.0);
                sh.y = L.sim_to_px_y(0.0);
                sh.vx = static_cast<double>(vfx_rng() % 800) / 100.0 - 4.0;
                sh.vy = static_cast<double>(vfx_rng() % 800) / 100.0 - 4.0;
                sh.life = 1.0;
                shards.push_back(sh);
            }
        }

        arena_view::set_color(ren, arena_view::kBg);
        SDL_RenderClear(ren);

        arena_view::set_color(ren, arena_view::kCyan);
        arena_view::stroke_rect(ren, L.arena_x, L.arena_y, L.arena_px, L.arena_px);

        const ironclad::World& w = sessions[0]->world();
        w.each<ironclad::Player>([&](ironclad::Entity e, const ironclad::Player& pl) {
            auto* tr = w.get<ironclad::Transform>(e);
            if (!tr) return;
            const int cx = L.sim_to_px_x(tr->pos.x.to_double());
            const int cy = L.sim_to_px_y(tr->pos.y.to_double());
            const auto& col = arena_view::player_color(pl.id);
            arena_view::draw_glow_circle(ren, cx, cy, 14, col.r, col.g, col.b);
        });
        w.each<ironclad::Projectile>([&](ironclad::Entity e, const ironclad::Projectile&) {
            auto* tr = w.get<ironclad::Transform>(e);
            if (!tr) return;
            const int cx = L.sim_to_px_x(tr->pos.x.to_double());
            const int cy = L.sim_to_px_y(tr->pos.y.to_double());
            arena_view::draw_glow_circle(ren, cx, cy, 3, 255, 255, 255);
        });

        for (auto& s : shards) { s.x += s.vx; s.y += s.vy; s.life -= 0.02; }
        shards.erase(std::remove_if(shards.begin(), shards.end(),
            [](const GlassShard& s){ return s.life <= 0.0; }), shards.end());
        for (const auto& s : shards) {
            Uint8 a = static_cast<Uint8>(std::clamp(s.life * 255.0, 0.0, 255.0));
            SDL_SetRenderDrawColor(ren, 255, 255, 255, a);
            SDL_RenderDrawPoint(ren, static_cast<int>(s.x), static_cast<int>(s.y));
        }

        const auto& st = sessions[0]->stats();
        char buf[128];
        arena_view::set_color(ren, arena_view::kCyan);
        std::snprintf(buf, sizeof(buf), "FRAME %u", st.current_frame);
        arena_view::draw_text(ren, L.arena_x, L.arena_y - 32, buf);
        std::snprintf(buf, sizeof(buf), "RTT %ums", opts.rtt_ms);
        arena_view::draw_text(ren, L.arena_x + 200, L.arena_y - 32, buf);
        std::snprintf(buf, sizeof(buf), "ROLLBACK %uF",
                      static_cast<unsigned>(st.last_rollback_frames));
        arena_view::draw_text(ren, L.arena_x + 360, L.arena_y - 32, buf);
        arena_view::set_color(ren, st.desync_detected ? arena_view::kRed : arena_view::kCyan);
        std::snprintf(buf, sizeof(buf), "DESYNC %s", st.desync_detected ? "YES" : "NO");
        arena_view::draw_text(ren, L.arena_x, L.arena_y + L.arena_px + 10, buf);

        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int run_studio(const std::string& path,
               const std::string& screenshot_path,
               std::int32_t       screenshot_frame) {
    auto m = ironclad::ReplayModel::load_file(path);
    if (!m) {
        std::fprintf(stderr, "failed to load replay: %s\n", path.c_str());
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("IRONCLAD Replay Studio",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800, SDL_WINDOW_SHOWN);
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

#if defined(IRONCLAD_HAS_IMGUI)
    // ImGui setup. We use a minimal context: ImGui draws the
    // control panel (stats, scrub bar, buttons), the line-segment
    // renderer keeps drawing the timeline, lanes, arena, inspector.
    bool imgui_ok = false;
    if (screenshot_path.empty()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (ImGui_ImplSDL2_InitForSDLRenderer(win, ren) &&
            ImGui_ImplSDLRenderer2_Init(ren)) {
            imgui_ok = true;
        }
    }
#endif

    arena_view::Studio studio(*m, arena_demo::init_arena, arena_demo::step_arena);

    if (!screenshot_path.empty()) {
        // Headless one-shot: render the requested frame to a BMP.
        const std::uint32_t target = screenshot_frame < 0
            ? m->record_count() / 2
            : static_cast<std::uint32_t>(screenshot_frame);
        studio.seek(target);
        studio.render_to(ren);
        SDL_RenderPresent(ren);

        int rw = 0, rh = 0;
        SDL_GetRendererOutputSize(ren, &rw, &rh);
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, rw, rh, 32,
                                                           SDL_PIXELFORMAT_ARGB8888);
        if (!surf) {
            std::fprintf(stderr, "CreateRGBSurface: %s\n", SDL_GetError());
        } else {
            SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                 surf->pixels, surf->pitch);
            if (SDL_SaveBMP(surf, screenshot_path.c_str()) != 0) {
                std::fprintf(stderr, "SDL_SaveBMP failed: %s\n", SDL_GetError());
            } else {
                std::printf("wrote %s (frame %u)\n", screenshot_path.c_str(), target);
            }
            SDL_FreeSurface(surf);
        }
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }

    Uint64 last_ms = SDL_GetTicks64();
    while (!studio.wants_quit()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
#if defined(IRONCLAD_HAS_IMGUI)
            if (imgui_ok) ImGui_ImplSDL2_ProcessEvent(&ev);
#endif
            studio.handle_event(ev);
        }
        const Uint64 now = SDL_GetTicks64();
        const double dt  = static_cast<double>(now - last_ms) / 1000.0;
        last_ms = now;
        studio.frame(ren, dt);
#if defined(IRONCLAD_HAS_IMGUI)
        if (imgui_ok) {
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            // Floating control panel.
            ImGui::SetNextWindowSize(ImVec2(360, 220), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(900, 12),  ImGuiCond_FirstUseEver);
            ImGui::Begin("Replay Studio");
            std::uint32_t pf = studio.playhead();
            const auto& stats = m->stats();
            ImGui::Text("FRAME %u / %u", pf, m->record_count());
            ImGui::Text("ROLLBACKS %u (max %u, avg %.2f)",
                        stats.rollback_event_count,
                        static_cast<unsigned>(stats.max_rollback_frames),
                        stats.avg_rollback_frames);
            ImGui::Text("LAG-COMP EVENTS %zu", m->lag_events().size());
            ImGui::Text("DESYNCS %u", stats.desync_event_count);
            ImGui::Separator();
            int frame_i = static_cast<int>(pf);
            if (ImGui::SliderInt("scrub", &frame_i, 0,
                                 static_cast<int>(m->record_count()))) {
                studio.seek(static_cast<std::uint32_t>(frame_i));
            }
            if (ImGui::Button("|<")) studio.seek(0);
            ImGui::SameLine();
            if (ImGui::Button("<")) studio.seek(pf > 0 ? pf - 1 : 0);
            ImGui::SameLine();
            if (ImGui::Button(">")) studio.seek(pf + 1);
            ImGui::SameLine();
            if (ImGui::Button(">|")) studio.seek(m->record_count());
            ImGui::SameLine();
            if (ImGui::Button("Rollback >")) {
                auto i = m->next_event_index(pf + 1, true);
                if (i < m->events().size())
                    studio.seek(m->events()[i].frame);
            }
            ImGui::SameLine();
            if (ImGui::Button("Lag >")) {
                const auto& levs = m->lag_events();
                if (!levs.empty()) {
                    std::uint32_t target = levs.front().frame;
                    for (const auto& lev : levs) {
                        if (lev.frame > pf) { target = lev.frame; break; }
                    }
                    studio.seek(target);
                }
            }
            ImGui::End();
            ImGui::Render();
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        }
#endif
        SDL_RenderPresent(ren);
    }

#if defined(IRONCLAD_HAS_IMGUI)
    if (imgui_ok) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
#endif
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "arena_view - ironclad SDL frontend\n"
        "Live mode:\n"
        "  arena_view [--rtt-ms M] [--loss-pct L]\n"
        "Replay Studio mode:\n"
        "  arena_view --replay PATH                (interactive)\n"
        "  arena_view --replay PATH --screenshot OUT.bmp [--frame N]\n");
}

}  // namespace

int main(int argc, char** argv) {
    arena_demo::Options opts;
    opts.frames      = 60u * 60u * 5u;
    opts.num_players = 4;
    opts.rtt_ms      = 100;
    opts.jitter_ms   = 20;
    opts.loss_pct    = 3;
    opts.reorder_pct = 1;

    std::string replay_path;
    std::string screenshot_path;
    std::int32_t screenshot_frame = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (++i >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                std::exit(2);
            }
            return argv[i];
        };
        if      (a == "--rtt-ms")     opts.rtt_ms = static_cast<std::uint16_t>(std::atoi(need("--rtt-ms")));
        else if (a == "--loss-pct")   opts.loss_pct = static_cast<std::uint8_t>(std::atoi(need("--loss-pct")));
        else if (a == "--replay")     replay_path = need("--replay");
        else if (a == "--screenshot") screenshot_path = need("--screenshot");
        else if (a == "--frame")      screenshot_frame = std::atoi(need("--frame"));
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }

    if (!replay_path.empty()) {
        return run_studio(replay_path, screenshot_path, screenshot_frame);
    }
    return run_live(opts);
}

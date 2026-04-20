// SPDX-License-Identifier: MIT
//
// Shared SDL primitives for the arena_view binary: neon glow circles,
// the line-segment text renderer, color palette, and arena ↔ pixel
// coordinate mapping. Used by both the live arena view and the
// Replay Studio.
//
// Everything here is intentionally tiny and dependency-free beyond
// SDL2; no font files, no atlases, no third-party glyph libraries.
#pragma once

#include <SDL2/SDL.h>

#include <cstdint>

namespace arena_view {

// --- Window + arena geometry ---------------------------------------------

struct Layout {
    int window_w = 1024;
    int window_h = 720;
    int arena_x  = 32;
    int arena_y  = 32;
    int arena_px = 480;          // square arena
    double sim_range = 24.0;     // sim units mapped to arena_px

    constexpr int sim_to_px_x(double v) const noexcept {
        return arena_x + static_cast<int>((v + sim_range / 2.0) /
                                          sim_range * arena_px);
    }
    constexpr int sim_to_px_y(double v) const noexcept {
        return arena_y + arena_px -
               static_cast<int>((v + sim_range / 2.0) / sim_range * arena_px);
    }
};

extern const Layout kDefaultLayout;

// --- Drawing primitives --------------------------------------------------

void draw_circle(SDL_Renderer* r, int cx, int cy, int radius);

/// Three-pass neon glow: faint outer rings + solid centre.
void draw_glow_circle(SDL_Renderer* r, int cx, int cy, int radius,
                      Uint8 R, Uint8 G, Uint8 B);

/// Line-segment text renderer. Supported glyphs: 0-9, A-Z (subset),
/// space, ':', '.', '%', '/', '=', '-', ',' '#', plus ASCII arrows
/// '<', '>', '^', 'v' and brackets '[', ']'. Unknown chars render
/// as blank. Each glyph is drawn in a 3x5 cell with `scale`-pixel
/// per cell unit, so the character cell is `4*scale` wide.
void draw_text(SDL_Renderer* r, int x, int y, const char* text, int scale = 2);

/// Filled rectangle (SDL_RenderFillRect wrapper that clamps to a
/// non-negative size).
void fill_rect(SDL_Renderer* r, int x, int y, int w, int h);

/// Outlined rectangle.
void stroke_rect(SDL_Renderer* r, int x, int y, int w, int h);

// --- Palette -------------------------------------------------------------

struct Color { Uint8 r, g, b, a = 255; };

extern const Color kBg;          // black
extern const Color kCyan;        // arena outline / labels
extern const Color kHotPink;     // accent
extern const Color kAmber;       // warnings (rollback)
extern const Color kRed;         // alarms (desync)
extern const Color kDimGrey;     // unselected lane backgrounds
extern const Color kWhite;

/// Distinct neon palette for player rendering.
const Color& player_color(std::uint8_t player_id);

inline void set_color(SDL_Renderer* r, const Color& c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

}  // namespace arena_view

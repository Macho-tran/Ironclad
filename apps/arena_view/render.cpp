// SPDX-License-Identifier: MIT
#include "render.hpp"

#include <algorithm>

namespace arena_view {

const Layout kDefaultLayout{};
const Color  kBg      {  0,   0,   0, 255};
const Color  kCyan    {  0, 220, 255, 255};
const Color  kHotPink {255,  64, 128, 255};
const Color  kAmber   {255, 180,  40, 255};
const Color  kRed     {255,  60,  60, 255};
const Color  kDimGrey { 32,  32,  40, 255};
const Color  kWhite   {255, 255, 255, 255};

const Color& player_color(std::uint8_t player_id) {
    static const Color palette[4] = {
        kHotPink,
        kCyan,
        Color{255, 220,  64},
        Color{128, 255, 128},
    };
    return palette[player_id % 4];
}

void draw_circle(SDL_Renderer* r, int cx, int cy, int radius) {
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
    SDL_SetRenderDrawColor(r, R, G, B, 64);
    draw_circle(r, cx, cy, radius + 3);
    SDL_SetRenderDrawColor(r, R, G, B, 128);
    draw_circle(r, cx, cy, radius + 1);
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    draw_circle(r, cx, cy, radius);
}

void fill_rect(SDL_Renderer* r, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

void stroke_rect(SDL_Renderer* r, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    SDL_Rect rect{x, y, w, h};
    SDL_RenderDrawRect(r, &rect);
}

void draw_text(SDL_Renderer* r, int x, int y, const char* text, int scale) {
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
        switch (c) {
            // Digits
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
            // Letters (subset — extend as needed)
            case 'A': seg(0,4,1,0); seg(1,0,2,4); seg(0,3,2,3); break;
            case 'B': seg(0,0,0,4); seg(0,0,2,1); seg(2,1,0,2); seg(0,2,2,3); seg(2,3,0,4); break;
            case 'C': seg(0,0,2,0); seg(0,0,0,4); seg(0,4,2,4); break;
            case 'D': seg(0,0,0,4); seg(0,0,2,1); seg(2,1,2,3); seg(2,3,0,4); break;
            case 'E': seg(0,0,2,0); seg(0,0,0,4); seg(0,2,2,2); seg(0,4,2,4); break;
            case 'F': seg(0,0,2,0); seg(0,0,0,4); seg(0,2,2,2); break;
            case 'G': seg(0,0,2,0); seg(0,0,0,4); seg(0,4,2,4); seg(2,4,2,2); seg(1,2,2,2); break;
            case 'H': seg(0,0,0,4); seg(2,0,2,4); seg(0,2,2,2); break;
            case 'I': seg(0,0,2,0); seg(1,0,1,4); seg(0,4,2,4); break;
            case 'J': seg(0,0,2,0); seg(1,0,1,4); seg(1,4,0,3); break;
            case 'K': seg(0,0,0,4); seg(0,2,2,0); seg(0,2,2,4); break;
            case 'L': seg(0,0,0,4); seg(0,4,2,4); break;
            case 'M': seg(0,0,0,4); seg(0,0,1,2); seg(1,2,2,0); seg(2,0,2,4); break;
            case 'N': seg(0,0,0,4); seg(0,0,2,4); seg(2,0,2,4); break;
            case 'O': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,4); seg(0,4,2,4); break;
            case 'P': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,2); seg(0,2,2,2); break;
            case 'Q': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,4); seg(0,4,2,4); seg(1,3,2,4); break;
            case 'R': seg(0,0,2,0); seg(0,0,0,4); seg(2,0,2,2); seg(0,2,2,2); seg(0,2,2,4); break;
            case 'S': seg(0,0,2,0); seg(0,0,0,2); seg(0,2,2,2); seg(2,2,2,4); seg(0,4,2,4); break;
            case 'T': seg(0,0,2,0); seg(1,0,1,4); break;
            case 'U': seg(0,0,0,4); seg(2,0,2,4); seg(0,4,2,4); break;
            case 'V': seg(0,0,1,4); seg(2,0,1,4); break;
            case 'W': seg(0,0,0,4); seg(0,4,1,2); seg(1,2,2,4); seg(2,4,2,0); break;
            case 'X': seg(0,0,2,4); seg(0,4,2,0); break;
            case 'Y': seg(0,0,1,2); seg(2,0,1,2); seg(1,2,1,4); break;
            case 'Z': seg(0,0,2,0); seg(0,4,2,0); seg(0,4,2,4); break;
            // Punctuation
            case ':':
                SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 1*scale);
                SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 3*scale);
                break;
            case '.':
                SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 4*scale);
                break;
            case ',':
                SDL_RenderDrawPoint(r, x + ox + 1*scale, y + 4*scale);
                seg(1,4,0,5);
                break;
            case '%': seg(0,0,0,1); seg(2,3,2,4); seg(0,4,2,0); break;
            case '/': seg(0,4,2,0); break;
            case '\\': seg(0,0,2,4); break;
            case '=': seg(0,1,2,1); seg(0,3,2,3); break;
            case '-': seg(0,2,2,2); break;
            case '#':
                seg(0,1,2,1); seg(0,3,2,3);
                seg(1,0,1,4);
                break;
            // ASCII arrows
            case '<': seg(2,0,0,2); seg(0,2,2,4); break;
            case '>': seg(0,0,2,2); seg(2,2,0,4); break;
            case '^': seg(0,2,1,0); seg(1,0,2,2); break;
            case 'v': seg(0,2,1,4); seg(1,4,2,2); break;
            // Brackets
            case '[': seg(2,0,0,0); seg(0,0,0,4); seg(0,4,2,4); break;
            case ']': seg(0,0,2,0); seg(2,0,2,4); seg(2,4,0,4); break;
            // Underscore
            case '_': seg(0,4,2,4); break;
            case ' ': default: break;
        }
    }
}

}  // namespace arena_view

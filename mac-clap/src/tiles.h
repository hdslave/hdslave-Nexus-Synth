#pragma once

#include "hdslave_tiles.h"
#include "screen.h"

namespace nexus
{

struct TilesAccess
{
    static constexpr int kTileCount = Screen::pageCount; // 7 tiles in 4x2 grid

    static uint32_t CLAP_ABI count (const clap_plugin_t*, int* out_cols, int* out_rows)
    {
        if (out_cols) *out_cols = 4;
        if (out_rows) *out_rows = 2;
        return kTileCount;
    }

    static const char* CLAP_ABI title (const clap_plugin_t*, uint32_t tile)
    {
        static constexpr const char* kTitles[kTileCount] = {
            "01 // PLAITS MACRO OSC",
            "02 // VOICE & FILTER",
            "03 // DUAL ADSR ENV",
            "04 // DUAL LFO",
            "05 // CYCLING ENV & RANDOM",
            "06 // FIXED MOD MATRIX",
            "07 // ASSIGNABLE MATRIX"
        };
        return tile < kTileCount ? kTitles[tile] : "";
    }

    static bool CLAP_ABI paint (const clap_plugin_t* plugin, uint32_t tile, const melodee_screen_frame_t* frame, clap_id knobs_out[3])
    {
        if (tile >= kTileCount || !frame || !frame->pixels) return false;
        auto& p = of (plugin);
        const int savedPage = p.screen.page;
        p.screen.page = (int) tile;
        p.screen.shownPage = -1;
        melodee_screen_frame_t c = *frame;
        c.redraw_all = 1;
        const hdui::Host h = hostOf (&c);
        const bool drew = p.screen.paint (h, c.pixels, (int) c.stride, true);
        if (knobs_out)
            screenControls (plugin, knobs_out);
        p.screen.page = savedPage;
        return drew;
    }

    static bool CLAP_ABI hit_test (const clap_plugin_t* plugin, uint32_t tile, int x, int y, hdslave_tile_hotspot_t* out_spot)
    {
        if (!out_spot || tile >= kTileCount) return false;
        *out_spot = {};
        out_spot->param_id = CLAP_INVALID_ID;
        out_spot->param_id_y = CLAP_INVALID_ID;

        // 1. Cells from Screen::layout(tile) (offset vertically by Screen::headerH = 41)
        const Screen::Layout l = Screen::layout ((int) tile);
        for (int i = 0; i < l.count; ++i)
        {
            const auto& c = l.cells[i];
            const int cx = c.x, cy = Screen::headerH + c.y, cw = c.w, ch = c.h;
            if (x >= cx && x < cx + cw && y >= cy && y < cy + ch)
            {
                // For matrix cells, clicking the top-right 14x12 corner toggles route enable (c.id + 1)
                if (c.style == Screen::matrix && x >= cx + cw - 16 && y < cy + 13)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = c.id + 1;
                    out_spot->x = (int16_t) (cx + cw - 16);
                    out_spot->y = (int16_t) cy;
                    out_spot->w = 16;
                    out_spot->h = 13;
                    return true;
                }
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = c.id;
                out_spot->x = (int16_t) cx;
                out_spot->y = (int16_t) cy;
                out_spot->w = (int16_t) cw;
                out_spot->h = (int16_t) ch;
                return true;
            }
        }

        // 3. Visual interactive widgets in the body (y: 41..182)
        const int by = y - Screen::headerH;
        if (by >= 0 && by < 142)
        {
            if (tile == Screen::plaitsPage)
            {
                // Scope region (x: 102..318, by: 2..51): click & hold auditions Middle C
                if (x >= Screen::scopeX && x < Screen::scopeX + Screen::scopeW && by >= Screen::scopeY && by < Screen::scopeY + Screen::scopeH)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_NOTE_PAD;
                    out_spot->custom_id = 60;
                    out_spot->x = (int16_t) Screen::scopeX;
                    out_spot->y = (int16_t) (Screen::headerH + Screen::scopeY);
                    out_spot->w = (int16_t) Screen::scopeW;
                    out_spot->h = (int16_t) Screen::scopeH;
                    return true;
                }
            }
            else if (tile == Screen::voicePage)
            {
                // Drag the filter response plot to set Cutoff and Resonance.
                if (x >= Screen::filterPlotX && x < Screen::filterPlotX + Screen::filterPlotW
                    && by >= Screen::row2 && by < Screen::row2 + Screen::filterPlotH)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::cutoff;
                    out_spot->param_id_y = ParamId::resonance;
                    out_spot->x = Screen::filterPlotX;
                    out_spot->y = Screen::headerH + Screen::row2;
                    out_spot->w = Screen::filterPlotW;
                    out_spot->h = Screen::filterPlotH;
                    out_spot->invert_y = true;
                    return true;
                }
            }
            else if (tile == Screen::envPage)
            {
                // Bottom envelope plots (by: 94..140)
                if (by >= 94 && by < 140)
                {
                    if (x < 160)
                    {
                        out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                        out_spot->param_id = x < 55 ? ParamId::env1Attack : (x < 110 ? ParamId::env1Decay : ParamId::env1Release);
                        out_spot->param_id_y = ParamId::env1Sustain;
                        out_spot->x = 2;
                        out_spot->y = (int16_t) (Screen::headerH + 94);
                        out_spot->w = 156;
                        out_spot->h = 46;
                        out_spot->invert_y = true;
                        return true;
                    }
                    else
                    {
                        out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                        out_spot->param_id = x < 215 ? ParamId::env2Attack : (x < 270 ? ParamId::env2Decay : ParamId::env2Release);
                        out_spot->param_id_y = ParamId::env2Sustain;
                        out_spot->x = 162;
                        out_spot->y = (int16_t) (Screen::headerH + 94);
                        out_spot->w = 156;
                        out_spot->h = 46;
                        out_spot->invert_y = true;
                        return true;
                    }
                }
            }
            else if (tile == Screen::lfoPage && by >= 94 && by < 140)
            {
                if (x >= 54 && x < 160)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::lfo1Rate;
                    out_spot->param_id_y = ParamId::lfo1Shape;
                    out_spot->x = 54;
                    out_spot->y = (int16_t) (Screen::headerH + 94);
                    out_spot->w = 104;
                    out_spot->h = 46;
                    out_spot->invert_y = true;
                    return true;
                }
                if (x >= 214 && x < 318)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::lfo2Rate;
                    out_spot->param_id_y = ParamId::lfo2Shape;
                    out_spot->x = 214;
                    out_spot->y = (int16_t) (Screen::headerH + 94);
                    out_spot->w = 104;
                    out_spot->h = 46;
                    out_spot->invert_y = true;
                    return true;
                }
            }
        }
        return false;
    }

    static void CLAP_ABI focus (const clap_plugin_t* plugin, uint32_t tile, int x, int y, bool)
    {
        if (tile >= kTileCount) return;
        auto& p = of (plugin);
        p.screen.page = (int) tile;
        const Screen::Layout l = Screen::layout ((int) tile);
        for (int i = 0; i < l.count; ++i)
        {
            const auto& c = l.cells[i];
            const int cx = c.x, cy = Screen::headerH + c.y;
            if (x >= cx && x < cx + c.w && y >= cy && y < cy + c.h)
            {
                p.screen.focus[tile] = i;
                p.screen.touched = true;
                break;
            }
        }
    }

    static void CLAP_ABI action (const clap_plugin_t*, uint32_t, int32_t, int32_t, bool) {}
};

const clap_plugin_hdslave_tiles_t tilesExtension {
    TilesAccess::count,
    TilesAccess::title,
    TilesAccess::paint,
    TilesAccess::hit_test,
    TilesAccess::focus,
    TilesAccess::action,
    183,
};

} // namespace nexus

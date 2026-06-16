#pragma once

#include "Common.hpp"

namespace lmc
{
    // Shared state + tuning constants for the floating eviction prompt. The prompt logic is split by
    // responsibility into GlyphWarmup / GlyphWidget / GlyphPositioner / GlyphColorizer / GlyphIcons,
    // and GlyphOverlay orchestrates them. All the mutable state they pass between each other lives
    // here in one place (resolved UFunction/UClass/texture caches, the created widget + its images +
    // TextBlock, the per-widget flags, the resolved text color). Each component additionally keeps its
    // own private timers / one-shot log latches. Plain data struct - no logic.

    // Layout / sizing (on-screen pixels) and the head anchor offset.
    inline constexpr double kAboveHeadPx = 55.0;        // px above the head anchor (clears the name plate)
    inline constexpr double kHeadExtraZ = 24.0;          // cm above the head bone
    inline constexpr double kIconSize = 30.0;            // key glyph px
    inline constexpr double kStationOutlineSize = 30.0;  // (1) earthy ring, largest
    inline constexpr double kStationFillSize = 26.0;     // (2) black fill, ~2px inside the ring
    inline constexpr double kStationSymbolSize = 18.0;   // (3) earthy symbol, on top
    inline constexpr double kKeyCenterX = -34.0;         // key glyph centre (left of the station icon)
    inline constexpr double kStationCenterX = 0.0;       // station icon centre = head anchor
    inline constexpr double kTextLeftX = 23.0;           // name text left edge (right of the icon)

    // Tints (R,G,B,A). The native prompt's color is baked into its TEXTURES (its brush TintColors all
    // read white), so we tint the ring + symbol the SAME earthy color as the prompt TEXT - the
    // colorizer reads our TextBlock's ColorAndOpacity at runtime. This earthy tan is the FALLBACK used
    // until/unless that read yields a usable color. BLACK FILL is always kTintFill.
    inline constexpr float kTintEarthy[4] = {0.52f, 0.41f, 0.24f, 1.0f}; // earthy tan (ring + symbol fallback)
    inline constexpr float kTintFill[4] = {0.0f, 0.0f, 0.0f, 1.0f};       // pure opaque black (never recolored)

    // The exact circle texture the game's own interaction prompt draws under its icons (drawn twice -
    // earthy ring + black fill), and the generic station icon used when a per-station path is missing.
    inline const TCHAR* const kCircleTexture = STR("/Game/UI/Textures/Common/T_BaseCircle.T_BaseCircle");
    inline const TCHAR* const kFallbackStationTex = STR("/Game/UI/Textures/Common/Icons/T_Interaction_Execute.T_Interaction_Execute");

    // NOTE: kWarmupRetryInterval (500ms) is already defined in Common.hpp and reused here.
    // The TextBlock exists as soon as our content is built, so the text-color read resolves within a
    // frame or two; it stops permanently on success or after the cap.
    inline const Clock::duration kTextColorRetryInterval = std::chrono::milliseconds(500);
    inline constexpr int kTextColorMaxAttempts = 60;
    // A color whose R,G,B are ALL above this is treated as pure white => the real color lives in the
    // font/material, not ColorAndOpacity => reject it and keep the earthy fallback.
    inline constexpr float kNearWhiteThreshold = 0.96f;

    struct GlyphContext
    {
        // Resolved UFunction caches (permanent /Script objects; survive map loads).
        FWeakObjectPtr fn_create{};
        FWeakObjectPtr fn_add{};
        FWeakObjectPtr fn_set_pos{};
        FWeakObjectPtr fn_set_align{};
        FWeakObjectPtr fn_set_vis{};
        FWeakObjectPtr fn_project{};
        FWeakObjectPtr fn_socket{};
        FWeakObjectPtr fn_set_brush{};
        FWeakObjectPtr fn_add_to_canvas{};
        FWeakObjectPtr fn_slot_pos{};
        FWeakObjectPtr fn_slot_size{};
        FWeakObjectPtr fn_slot_align{};
        FWeakObjectPtr fn_slot_anchors{};
        FWeakObjectPtr fn_tint{};           // SetColorAndOpacity (legacy; diagnostic only)
        FWeakObjectPtr fn_set_brush_tint{}; // SetBrushTintColor (fallback tint mechanism B)
        FWeakObjectPtr fn_set_text{};
        FWeakObjectPtr widget_class{};
        FWeakObjectPtr image_class{};

        // Textures (world assets).
        FWeakObjectPtr tex_e{};
        FWeakObjectPtr tex_y{};
        FWeakObjectPtr station_tex{};
        FWeakObjectPtr circle_tex{};

        // Created widget + its children.
        FWeakObjectPtr widget{};
        FWeakObjectPtr image_key{};
        FWeakObjectPtr image_station{};         // (3) earthy symbol, on top
        FWeakObjectPtr image_station_bg{};      // (2) black fill
        FWeakObjectPtr image_station_outline{}; // (1) earthy ring, drawn first (bottom)
        FWeakObjectPtr text_block{};

        // Per-widget flags.
        bool warmed{};
        bool content_built{};
        bool added{};
        bool visible{};
        int key_device{-1};

        // Station name shown on the reused TextBlock (set once per change to avoid FText refcount churn).
        FText name_text{};
        StringType name_built{};
        bool name_dirty{};

        // Last station-icon path applied (so set_station_icon early-returns on no change).
        StringType station_path_built{};

        // Text color read from the TextBlock; the ring + symbol are tinted with it (else kTintEarthy).
        float text_color[4]{};
        bool text_color_valid{};
        bool text_color_upgrade_done{}; // one-shot post-build re-tint ran for this widget
    };
}

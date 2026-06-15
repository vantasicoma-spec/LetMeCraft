#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"

namespace lmc
{
    // v1.2.0 floating eviction prompt over the evictable cook NPC.
    //
    // Discovery established (logs):
    //  * a custom UMG widget CAN be created + added to the viewport + positioned via reflection;
    //  * W_InputHint_C CRASHES when created standalone -> we use the proven-safe W_DisplayName_
    //    GothicFont_C, whose tree is CanvasPanel(root) + a TextBlock (no Image of its own);
    //  * real key textures exist: T_Icon_PC_E (keyboard) / T_Icon_Xbox_Y (gamepad), and the
    //    station action icons T_Interaction_Cooking / _Alchemy / _Use.
    //
    // So we host the whole prompt ourselves on that safe widget: two UImages added to the root
    // CanvasPanel (left = station action icon, right = the real key glyph) and the widget's own
    // TextBlock reused to show the station's localized name underneath. The NPC's own name widget
    // is never touched. Positioned every frame over the NPC head (head bone, which follows the
    // animation so it stays put when the cook sits). Shown only while the player can evict that NPC.
    //
    // Pure UI: widget creation + texture brushes + a text + a screen position. No NPC/spot/save
    // writes. Functions and assets resolve on a quiet tick and weak-cache; per-frame positioning is
    // wrapped in its OWN SEH guard so a transient fault skips one icon frame instead of taking the
    // tick (and the evictions) down with it.
    class GlyphOverlay
    {
    public:
        explicit GlyphOverlay(GameObjects& objects) : m_objects(objects) {}

        // active: the player can evict this NPC right now. avatar: the NPC, ALREADY validated live by
        // the caller via weak_get this frame (so no stale-pointer deref here). station_name: the
        // localized station name to print (e.g. "Сковорода"). station_icon_path: full object path of
        // the action icon texture. Both are computed by the caller at scan time off the fresh
        // candidate and only re-applied to the widget when they actually change.
        auto update(bool active, UObject* avatar, bool gamepad,
                    const StringType& station_name, const TCHAR* station_icon_path) -> void
        {
            warm_up();
            if (!active || !m_warmed || !is_usable(avatar)) { hide(); return; }

            auto* controller = m_objects.find_player_controller_cached();
            if (!is_usable(controller)) { hide(); return; }

            auto* widget = ensure_widget(controller);
            if (!is_usable(widget)) { hide(); return; }

            resolve_key_textures_lazy();
            build_content_once(widget);
            set_key_icon(gamepad);
            set_station_icon(station_icon_path);
            rebuild_name(station_name);

            // Read the prompt TEXT color (now that build_content_once has found the TextBlock); if the
            // content was tinted with the earthy FALLBACK first, re-tint the ring + symbol ONCE to match it.
            resolve_text_color();
            apply_text_color_upgrade();

            // Nothing to show until the REAL key icon is actually on the image (no placeholder).
            if (m_key_device < 0) { hide(); return; }

            // Project the head to screen, position the widget, (re)assert the station name. Wrapped in
            // its OWN SEH guard: during a station-take/camera transition the projection occasionally
            // faults on a half-torn-down avatar; with this guard that just skips a single icon frame
            // instead of aborting all evictions and clearing the widget (which made the icon vanish).
            SehCrashInfo crash{};
            if (!invoke_with_seh([&] { position_and_label(controller, widget, avatar); }, crash))
            {
                if (m_pos_fault_logged < 3)
                {
                    ++m_pos_fault_logged;
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] glyph position skipped one frame (SEH code=0x{:X} ip=0x{:X}); eviction unaffected.\n"),
                        crash.code,
                        reinterpret_cast<uintptr_t>(crash.instruction_address));
                }
            }
        }

        auto clear() -> void
        {
            // Re-warm after a map load: the BP widget class may unload with the old world. The
            // /Script function caches are permanent and re-resolve instantly on the next quiet tick.
            m_warmed = false;
            m_next_warmup_at = {};
            m_widget = static_cast<UObject*>(nullptr);
            m_image_key = static_cast<UObject*>(nullptr);
            m_image_station = static_cast<UObject*>(nullptr);
            m_image_station_bg = static_cast<UObject*>(nullptr);
            m_image_station_outline = static_cast<UObject*>(nullptr);
            m_text_block = static_cast<UObject*>(nullptr);
            m_added = false;
            m_visible = false;
            m_content_built = false;
            m_key_device = -1;
            m_name_built.clear();
            m_name_dirty = false;
            m_station_path_built.clear();
            // A new world reloads the prompt BP: drop the text-color cache so it re-reads, and reset the
            // per-value tint-log latches so the next world's diagnostics aren't silent.
            m_text_color_valid = false;
            m_text_color_ready = false;
            m_text_color_upgrade_done = false;
            m_text_color_attempts = 0;
            m_next_text_color_at = {};
            m_logged_text_color_values = false;
            m_logged_tint_outline = false;
            m_logged_tint_fill = false;
            m_logged_tint_symbol = false;
        }

    private:
        static constexpr double kAboveHeadPx = 55.0;     // pixels above the head anchor (clears the game's name plate)
        static constexpr double kHeadExtraZ = 24.0;       // cm above the head bone
        static constexpr double kIconSize = 30.0;         // on-screen icon px (key glyph)
        // The station icon is three concentric T_BaseCircle/symbol layers (like the native [F] prompt):
        // an earthy OUTLINE ring (largest), a BLACK FILL just inside it (so a thin ring shows), and the
        // earthy station SYMBOL on top. Added in this order so each draws over the previous.
        static constexpr double kStationOutlineSize = 30.0; // (1) earthy ring, largest
        static constexpr double kStationFillSize    = 26.0; // (2) black fill, ~2px inside the ring
        static constexpr double kStationSymbolSize  = 18.0; // (3) earthy symbol, on top
        // Tints (R,G,B,A). The native prompt's color is baked into its TEXTURES (its brush TintColors all
        // read white), so instead we tint the ring + symbol the SAME earthy color as the prompt TEXT -
        // resolve_text_color() reads our TextBlock's ColorAndOpacity at runtime. This earthy tan is the
        // FALLBACK used until/unless that read yields a usable color. BLACK FILL is always kTintFill.
        static constexpr float kTintEarthy[4] = {0.52f, 0.41f, 0.24f, 1.0f}; // earthy tan (ring + symbol fallback)
        static constexpr float kTintFill[4]   = {0.0f,  0.0f,  0.0f,  1.0f};  // pure opaque black (never recolored)
        // Single horizontal row, read left->right: KEY -> STATION ICON -> NAME, all vertically centred
        // (y=0). The station icon is the head anchor (x=0); the key sits to its left, the name to its right.
        static constexpr double kKeyCenterX = -34.0;      // key glyph centre (left of the station icon, ~4px gap)
        static constexpr double kStationCenterX = 0.0;    // station icon centre = head anchor
        static constexpr double kTextLeftX = 23.0;        // name text left edge (just right of the station icon)
        // The EXACT circle texture the game's own interaction prompt (W_ObjectInteraction_C,
        // Image_Background/Image_Outline) draws under its icons - confirmed by the glyph-native runtime
        // dump. We draw it twice (earthy ring + black fill) tinted via apply_brush_tint().
        static inline const TCHAR* kCircleTexture = STR("/Game/UI/Textures/Common/T_BaseCircle.T_BaseCircle");
        static constexpr int32_t kFVector2DSize = 16;     // UE5.4 LWC: two doubles live (NOT sizeof=24)
        static inline const Clock::duration kWarmupRetryInterval = std::chrono::milliseconds(500);
        static constexpr uint8_t kVisCollapsed = 1;
        static constexpr uint8_t kVisSelfHitTestInvisible = 4;
        // resolve_text_color() throttle + give-up. The TextBlock exists as soon as our content is built,
        // so this resolves within a frame or two; it stops permanently on success or after the cap.
        static inline const Clock::duration kTextColorRetryInterval = std::chrono::milliseconds(500);
        static constexpr int kTextColorMaxAttempts = 60;
        // A color whose R,G,B are ALL above this is treated as pure white => the real color lives in the
        // font/material, not ColorAndOpacity => reject it and keep the earthy fallback.
        static constexpr float kNearWhiteThreshold = 0.96f;

        auto warm_up() -> void
        {
            if (m_warmed) { return; }
            const auto now = Clock::now();
            if (now < m_next_warmup_at) { return; }
            m_next_warmup_at = now + kWarmupRetryInterval;
            ++m_warmup_attempts;

            run_guarded(STR("glyph warm-up"), [&] {
                const auto create = resolve_fn(m_fn_create, STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
                const auto add = resolve_fn(m_fn_add, STR("/Script/UMG.UserWidget:AddToViewport"));
                const auto setpos = resolve_fn(m_fn_set_pos, STR("/Script/UMG.UserWidget:SetPositionInViewport"));
                const auto setalign = resolve_fn(m_fn_set_align, STR("/Script/UMG.UserWidget:SetAlignmentInViewport"));
                const auto setvis = resolve_fn(m_fn_set_vis, STR("/Script/UMG.Widget:SetVisibility"));
                const auto project = resolve_fn(m_fn_project, STR("/Script/Engine.PlayerController:ProjectWorldLocationToScreen"));
                const auto socket = resolve_fn(m_fn_socket, STR("/Script/Engine.SceneComponent:GetSocketLocation"));
                const auto brush = resolve_fn(m_fn_set_brush, STR("/Script/UMG.Image:SetBrushFromTexture"));
                const auto addcanvas = resolve_fn(m_fn_add_to_canvas, STR("/Script/UMG.CanvasPanel:AddChildToCanvas"));
                const auto slotpos = resolve_fn(m_fn_slot_pos, STR("/Script/UMG.CanvasPanelSlot:SetPosition"));
                const auto slotsize = resolve_fn(m_fn_slot_size, STR("/Script/UMG.CanvasPanelSlot:SetSize"));
                const auto slotalign = resolve_fn(m_fn_slot_align, STR("/Script/UMG.CanvasPanelSlot:SetAlignment"));
                resolve_fn(m_fn_slot_anchors, STR("/Script/UMG.CanvasPanelSlot:SetAnchors"));
                // Tint mechanisms (non-gating). PRIMARY is a direct Brush.TintColor memory write (needs no
                // UFunction at all - see apply_brush_tint); SetBrushTintColor is a fallback. SetColorAndOpacity
                // was the old path and never visibly tinted - kept only resolved for the diagnostic log.
                const auto tint_bt = resolve_fn(m_fn_set_brush_tint, STR("/Script/UMG.Image:SetBrushTintColor"));
                const auto tint_co = resolve_fn(m_fn_tint, STR("/Script/UMG.Image:SetColorAndOpacity"));
                const auto settext = resolve_fn(m_fn_set_text, STR("/Script/UMG.TextBlock:SetText"));
                const auto wcls = resolve_class_by_path(m_widget_class, STR("/Game/UI/Common/W_DisplayName_GothicFont.W_DisplayName_GothicFont_C"), STR("W_DisplayName_GothicFont_C"));
                const auto icls = resolve_class_by_path(m_image_class, STR("/Script/UMG.Image"), STR("Image"));
                m_warmed = create && add && setpos && setvis && project && brush && addcanvas &&
                           slotpos && slotsize && slotalign && settext && wcls && icls;

                if (m_warmed && !m_logged_warm)
                {
                    m_logged_warm = true;
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] glyph warm-up OK (align={} socket={} settext={} | tint: direct=proven setbrushtint={} colorandopacity={}).\n"),
                        setalign ? STR("ok") : STR("-"),
                        socket ? STR("ok") : STR("-"),
                        settext ? STR("ok") : STR("-"),
                        tint_bt ? STR("ok") : STR("-"),
                        tint_co ? STR("ok") : STR("-"));
                }
            });

            if (!m_warmed && (m_warmup_attempts == 1 || m_warmup_attempts % 20 == 0))
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] glyph warm-up attempt {} unresolved (create={} addcanvas={} settext={} wclass={} iclass={}).\n"),
                    m_warmup_attempts,
                    weak_get(m_fn_create) ? STR("ok") : STR("-"), weak_get(m_fn_add_to_canvas) ? STR("ok") : STR("-"),
                    weak_get(m_fn_set_text) ? STR("ok") : STR("-"), weak_get(m_widget_class) ? STR("ok") : STR("-"),
                    weak_get(m_image_class) ? STR("ok") : STR("-"));
            }
        }

        auto resolve_fn(FWeakObjectPtr& cache, const TCHAR* path) -> UFunction*
        {
            if (auto* cached = weak_get(cache)) { return static_cast<UFunction*>(cached); }
            UFunction* fn = nullptr;
            try { fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); } catch (...) {}
            if (fn) { cache = static_cast<UObject*>(fn); }
            return fn;
        }

        auto resolve_texture(FWeakObjectPtr& cache, const TCHAR* path) -> UObject*
        {
            if (auto* cached = weak_get(cache)) { return cached; }
            UObject* tex = nullptr;
            try { tex = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); } catch (...) {}
            if (tex) { cache = tex; }
            return tex;
        }

        auto resolve_key_textures_lazy() -> void
        {
            if (weak_get(m_tex_e) && weak_get(m_tex_y)) { return; }
            const auto now = Clock::now();
            if (now < m_next_tex_retry) { return; }
            m_next_tex_retry = now + std::chrono::milliseconds(1000);
            resolve_texture(m_tex_e, STR("/Game/UI/Textures/Common/HotkeysIcons/T_Icon_PC_E.T_Icon_PC_E"));
            // NOTE the asset is spelled "XboX" (capital X at BOTH ends) - confirmed by the glyph-tex
            // sweep. "T_Icon_Xbox_Y" does not resolve, which is why the gamepad prompt fell back to E.
            resolve_texture(m_tex_y, STR("/Game/UI/Textures/Common/HotkeysIcons/T_Icon_XboX_Y.T_Icon_XboX_Y"));
            if (weak_get(m_tex_e) && !m_logged_tex)
            {
                m_logged_tex = true;
                Output::send<LogLevel::Verbose>(STR("[LetMeCraft] glyph key textures resolved.\n"));
            }
        }

        // glyphicon15: the native interaction prompt's color is baked into its TEXTURES (every native
        // brush TintColor reads 1,1,1,1 - useless). The user wants the icon the SAME earthy color as the
        // prompt TEXT, so we read our OWN TextBlock's ColorAndOpacity (FSlateColor) and tint the ring +
        // symbol with it. Reading our own held widget child -> run_guarded suffices (no foreign-object
        // scan / SEH needed). Throttled until a usable color resolves or the attempt cap; then a one-shot
        // upgrade re-tints the ring + symbol. Until then the earthy fallback stands.
        auto resolve_text_color() -> void
        {
            if (m_text_color_ready || m_text_color_attempts >= kTextColorMaxAttempts) { return; }
            auto* tb = weak_get(m_text_block);
            if (!is_usable(tb)) { return; } // TextBlock not built yet; retry next frame (no attempt spent)
            const auto now = Clock::now();
            if (now < m_next_text_color_at) { return; }
            m_next_text_color_at = now + kTextColorRetryInterval;
            ++m_text_color_attempts;

            float rgba[4]{}; uint8_t rule = 0xFF; bool read = false;
            run_guarded(STR("glyph text-color"), [&] { read = read_slate_color(tb, STR("ColorAndOpacity"), rgba, rule); });

            if (read && !m_logged_text_color_values)
            {
                m_logged_text_color_values = true;
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] glyph text color: ({:.3f},{:.3f},{:.3f},{:.3f}) rule={}.\n"),
                    rgba[0], rgba[1], rgba[2], rgba[3], static_cast<int>(rule));
            }
            // Accept only a specified (rule==0), non-pure-white color. Pure white => the real color lives
            // in the font/material, not ColorAndOpacity -> keep the earthy fallback.
            if (read && rule == 0 &&
                !(rgba[0] > kNearWhiteThreshold && rgba[1] > kNearWhiteThreshold && rgba[2] > kNearWhiteThreshold))
            {
                std::memcpy(m_text_color, rgba, sizeof(m_text_color));
                m_text_color_valid = true;
                m_text_color_ready = true; // got a usable color -> stop polling
            }
            else if (m_text_color_attempts >= kTextColorMaxAttempts)
            {
                m_text_color_ready = true; // give up; the earthy fallback stands
            }
        }

        // Read an FSlateColor property (e.g. UTextBlock.ColorAndOpacity) -> its SpecifiedColor (FLinearColor,
        // 16 bytes) + ColorUseRule byte. Same traversal shape as apply_brush_tint, but the top property IS
        // the FSlateColor (not a brush). out_rule defaults to 0xFF so a missing rule reads as "not specified".
        auto read_slate_color(UObject* obj, const TCHAR* prop_name, float (&out_rgba)[4], uint8_t& out_rule) -> bool
        {
            out_rule = 0xFF;
            if (!is_usable(obj)) { return false; }
            auto* col_prop = CastField<FStructProperty>(obj->GetPropertyByNameInChain(prop_name));
            if (!col_prop) { return false; }
            void* col_ptr = col_prop->ContainerPtrToValuePtr<void>(obj);
            if (!col_ptr) { return false; }
            auto col_struct = col_prop->GetStruct(); // TObjectPtr<UScriptStruct> (FSlateColor)
            if (!col_struct) { return false; }
            auto* spec_prop = CastField<FStructProperty>(col_struct->FindProperty(FName(STR("SpecifiedColor"), FNAME_Find)));
            if (!spec_prop) { return false; }
            const int32_t spec_off = spec_prop->GetOffset_Internal();
            if (spec_off < 0 || spec_prop->GetSize() < 16) { return false; }
            std::memcpy(&out_rgba[0], static_cast<uint8_t*>(col_ptr) + spec_off, 16);
            if (auto* rule_prop = col_struct->FindProperty(FName(STR("ColorUseRule"), FNAME_Find)))
            {
                const int32_t ro = rule_prop->GetOffset_Internal();
                if (ro >= 0 && rule_prop->GetSize() >= 1) { std::memcpy(&out_rule, static_cast<uint8_t*>(col_ptr) + ro, 1); }
            }
            return true;
        }

        // Copy the best color for a layer into out[4]: the resolved text color if valid, else the fallback.
        // Returns true if the primary (text) color was used.
        auto pick_color(const float* primary, bool valid, const float* fallback, float (&out)[4]) -> bool
        {
            const float* s = valid ? primary : fallback;
            out[0] = s[0]; out[1] = s[1]; out[2] = s[2]; out[3] = s[3];
            return valid;
        }

        // One-shot: once the text color resolves AFTER the content was built with the earthy fallback,
        // re-tint the ring + symbol with it (set_station_icon early-returns once the icon is set, so it
        // won't re-tint on its own). The BLACK FILL is never touched. Latched only after a successful
        // outline re-tint, so a momentarily unusable widget retries next frame instead of latching a no-op.
        auto apply_text_color_upgrade() -> void
        {
            if (m_text_color_upgrade_done || !m_text_color_valid || !m_content_built) { return; }
            auto* outline = weak_get(m_image_station_outline);
            if (!is_usable(outline)) { return; }

            auto* circle = resolve_texture(m_circle_tex, kCircleTexture); // re-resolve so the repaint fires
            float c[4]; pick_color(m_text_color, m_text_color_valid, kTintEarthy, c);
            apply_brush_tint(outline, c, 0, circle, STR("glyph tint-out"));
            if (auto* sym = weak_get(m_image_station))
            {
                if (auto* tex = weak_get(m_station_tex)) { apply_brush_tint(sym, c, 2, tex, STR("glyph tint-sym")); }
            }
            m_text_color_upgrade_done = true;
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] glyph text-color upgrade applied to ring+symbol: ({:.3f},{:.3f},{:.3f}).\n"),
                c[0], c[1], c[2]);
        }

        // Resolve a UClass by full object path (works for /Script classes and loaded /Game BP classes);
        // falls back to a global name scan if the path is not directly findable.
        auto resolve_class_by_path(FWeakObjectPtr& cache, const TCHAR* path, const TCHAR* short_name) -> UClass*
        {
            if (auto* cached = weak_get(cache)) { return static_cast<UClass*>(cached); }
            UClass* found = nullptr;
            try { found = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path); } catch (...) {}
            if (!found)
            {
                const FName target{short_name, FNAME_Find};
                UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                    if (auto* cls = Cast<UClass>(obj))
                    {
                        if (cls->GetNamePrivate() == target) { found = cls; return LoopAction::Break; }
                    }
                    return LoopAction::Continue;
                });
            }
            if (found) { cache = static_cast<UObject*>(found); }
            return found;
        }

        auto ensure_widget(UObject* controller) -> UObject*
        {
            if (auto* existing = weak_get(m_widget)) { return existing; }

            auto* create_fn = static_cast<UFunction*>(weak_get(m_fn_create));
            auto* widget_class = static_cast<UClass*>(weak_get(m_widget_class));
            if (!create_fn || !widget_class) { return nullptr; }

            auto call = begin_call_with_function(controller, create_fn, STR("glyph create"));
            set_param(call, STR("WorldContextObject"), &controller, 8, STR("glyph create"));
            set_param(call, STR("WidgetType"), &widget_class, 8, STR("glyph create"));
            set_param(call, STR("OwningPlayer"), &controller, 8, STR("glyph create"));
            if (!invoke_call(call, STR("glyph create"))) { return nullptr; }

            auto* widget = get_object_param(call, STR("ReturnValue"), STR("glyph create"));
            if (!is_usable(widget)) { return nullptr; }

            m_widget = widget;
            m_image_key = static_cast<UObject*>(nullptr);
            m_image_station = static_cast<UObject*>(nullptr);
            m_image_station_bg = static_cast<UObject*>(nullptr);
            m_image_station_outline = static_cast<UObject*>(nullptr);
            m_text_block = static_cast<UObject*>(nullptr);
            m_content_built = false;
            m_added = false;
            m_visible = false;
            m_key_device = -1;
            m_name_built.clear();          // force rebuild_name to re-push the name onto the new TextBlock
            m_name_dirty = false;
            m_station_path_built.clear();
            // Re-apply the (world-stable) text color onto the freshly built content; keep the cached color.
            m_text_color_upgrade_done = false;
            Output::send<LogLevel::Verbose>(STR("[LetMeCraft] glyph created: {}.\n"), object_name(widget));

            if (auto* add_fn = static_cast<UFunction*>(weak_get(m_fn_add)))
            {
                auto add = begin_call_with_function(widget, add_fn, STR("glyph add"));
                int32_t z_order = 50;
                set_param(add, STR("ZOrder"), &z_order, 4, STR("glyph add"));
                invoke_call(add, STR("glyph add"));
                m_added = true;
            }
            if (auto* align_fn = static_cast<UFunction*>(weak_get(m_fn_set_align)))
            {
                FVector2D alignment{};
                alignment.SetX(0.5);
                alignment.SetY(1.0);
                auto al = begin_call_with_function(widget, align_fn, STR("glyph align"));
                set_param(al, STR("Alignment"), &alignment, kFVector2DSize, STR("glyph align"));
                invoke_call(al, STR("glyph align"));
            }
            apply_visibility(widget, false);
            return widget;
        }

        // Build the prompt content once: a station-icon UImage (left) + a key-glyph UImage (right) on
        // the root CanvasPanel, and the widget's own TextBlock repositioned below them for the station
        // name (kept visible - we drive its text ourselves; the NPC's own name widget is untouched).
        auto build_content_once(UObject* widget) -> void
        {
            if (m_content_built) { return; }

            auto* tree = read_object(widget, STR("WidgetTree"));
            auto* root = read_object(tree, STR("RootWidget"));
            auto* image_class = static_cast<UClass*>(weak_get(m_image_class));
            auto* add_fn = static_cast<UFunction*>(weak_get(m_fn_add_to_canvas));
            if (!is_usable(tree) || !is_usable(root) || !image_class || !add_fn)
            {
                // Tree not ready yet: do NOT latch the flag, retry next frame (otherwise the prompt
                // would stay forever empty if the widget tree wasn't built on the first update).
                Output::send<LogLevel::Warning>(STR("[LetMeCraft] glyph: content not ready (tree/root/class/fn missing), will retry.\n"));
                return;
            }

            // Row order, left -> right: KEY glyph, then the STATION icon. The station icon is three
            // concentric layers and children draw in ADD order (later = on top), so add them outline ->
            // fill -> symbol: the earthy ring sits at the bottom, the black fill covers its centre (leaving
            // a thin ring), the earthy symbol sits on top. Text comes after (right of the icon).
            m_image_key = add_canvas_image(tree, root, image_class, add_fn, kKeyCenterX);
            m_image_station_outline = add_canvas_image(tree, root, image_class, add_fn, kStationCenterX, kStationOutlineSize);
            m_image_station_bg = add_canvas_image(tree, root, image_class, add_fn, kStationCenterX, kStationFillSize);
            m_image_station = add_canvas_image(tree, root, image_class, add_fn, kStationCenterX, kStationSymbolSize);

            // Both circle layers draw the same T_BaseCircle disc; the tint makes one an earthy ring and the
            // other a black fill. Set the brush once (never changes per station), then tint. The symbol's
            // texture+tint are (re)applied per station by set_station_icon (no symbol texture exists yet).
            if (auto* circle = resolve_texture(m_circle_tex, kCircleTexture))
            {
                if (auto* brush_fn = static_cast<UFunction*>(weak_get(m_fn_set_brush)))
                {
                    if (auto* o = weak_get(m_image_station_outline)) { set_brush(o, brush_fn, circle, STR("glyph circ-out")); }
                    if (auto* f = weak_get(m_image_station_bg)) { set_brush(f, brush_fn, circle, STR("glyph circ-fill")); }
                }
                // Outline: the resolved earthy text color if we have it, else the earthy fallback; the
                // post-build upgrade re-applies the text color if it resolves later. Fill: ALWAYS black.
                float oc[4]; pick_color(m_text_color, m_text_color_valid, kTintEarthy, oc);
                apply_brush_tint(weak_get(m_image_station_outline), oc, 0, circle, STR("glyph tint-out"));
                apply_brush_tint(weak_get(m_image_station_bg), kTintFill, 1, circle, STR("glyph tint-fill"));
            }

            // Reuse the widget's TextBlock for the station name: place it to the RIGHT of the icons,
            // vertically centred on the row (NOT below - that collided with the game's NPC name plate).
            if (auto* text_block = find_text_block(widget))
            {
                m_text_block = text_block;
                if (auto* slot = read_object(text_block, STR("Slot")))
                {
                    // The reused TextBlock comes from the BP with its own anchors (the name plate was
                    // centre/right anchored), which made SetPosition land it far off to the side. Reset
                    // its anchors to the top-left point (0,0) so our absolute coords share the same
                    // space as the freshly-added image slots; then position it right of the icons.
                    set_topleft_anchor(slot);
                    FVector2D pos{}; pos.SetX(kTextLeftX); pos.SetY(0.0);
                    FVector2D align{}; align.SetX(0.0); align.SetY(0.5);
                    call_slot(slot, m_fn_slot_pos, STR("InPosition"), pos, STR("glyph textpos"));
                    call_slot(slot, m_fn_slot_align, STR("InAlignment"), align, STR("glyph textalign"));
                }
            }

            // Latch only once the key + all three station layers are real; a half-built attempt retries.
            const bool ok = is_usable(weak_get(m_image_key)) && is_usable(weak_get(m_image_station_outline)) &&
                            is_usable(weak_get(m_image_station_bg)) && is_usable(weak_get(m_image_station));
            m_content_built = ok;
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] glyph content built={} (key={} outline={} fill={} symbol={} text={}).\n"),
                ok ? STR("ok") : STR("retry"),
                weak_get(m_image_key) ? STR("ok") : STR("-"),
                weak_get(m_image_station_outline) ? STR("ok") : STR("-"),
                weak_get(m_image_station_bg) ? STR("ok") : STR("-"),
                weak_get(m_image_station) ? STR("ok") : STR("-"),
                weak_get(m_text_block) ? STR("ok") : STR("-"));
        }

        // NewObject a UImage, add it to the root CanvasPanel, and size/centre its slot at (x, 0).
        auto add_canvas_image(UObject* tree, UObject* root, UClass* image_class, UFunction* add_fn, double x, double px = kIconSize) -> UObject*
        {
            UObject* image = nullptr;
            try { image = UObjectGlobals::NewObject<UObject>(tree, image_class); } catch (...) {}
            if (!is_usable(image))
            {
                Output::send<LogLevel::Warning>(STR("[LetMeCraft] glyph: NewObject(Image) failed.\n"));
                return nullptr;
            }

            auto add = begin_call_with_function(root, add_fn, STR("glyph addcanvas"));
            set_param(add, STR("Content"), &image, 8, STR("glyph addcanvas"));
            if (!invoke_call(add, STR("glyph addcanvas"))) { return nullptr; }
            auto* slot = get_object_param(add, STR("ReturnValue"), STR("glyph addcanvas"));

            if (is_usable(slot))
            {
                FVector2D pos{}; pos.SetX(x); pos.SetY(0.0);
                FVector2D size{}; size.SetX(px); size.SetY(px);
                FVector2D mid{}; mid.SetX(0.5); mid.SetY(0.5);
                call_slot(slot, m_fn_slot_pos, STR("InPosition"), pos, STR("glyph slotpos"));
                call_slot(slot, m_fn_slot_size, STR("InSize"), size, STR("glyph slotsize"));
                call_slot(slot, m_fn_slot_align, STR("InAlignment"), mid, STR("glyph slotalign"));
            }
            return image;
        }

        auto call_slot(UObject* slot, FWeakObjectPtr& fn_cache, const TCHAR* param, const FVector2D& value, const TCHAR* ctx) -> void
        {
            auto* fn = static_cast<UFunction*>(weak_get(fn_cache));
            if (!fn) { return; }
            auto call = begin_call_with_function(slot, fn, ctx);
            auto local = value;
            set_param(call, param, &local, kFVector2DSize, ctx);
            invoke_call(call, ctx);
        }

        // SetAnchors(InAnchors) with a zero-initialized param buffer => InAnchors = {Min(0,0),Max(0,0)},
        // a top-left point anchor. Size-agnostic (we don't memcpy a value), so it works whatever the
        // FAnchors layout/size is on this build - the zeroed params slot already encodes (0,0,0,0).
        auto set_topleft_anchor(UObject* slot) -> void
        {
            auto* fn = static_cast<UFunction*>(weak_get(m_fn_slot_anchors));
            if (!is_usable(slot) || !fn) { return; }
            auto call = begin_call_with_function(slot, fn, STR("glyph anchors"));
            invoke_call(call, STR("glyph anchors"));
        }

        // One-time discovery: the native interaction prompt's station icons (circular black/orange) and
        // the gamepad button glyphs live at runtime-only paths we couldn't confirm from the repo. Sweep
        // loaded Texture2D objects whose path looks interaction/hotkey-related and log them so the next
        // build can map station->correct texture and use the real gamepad-button name. Removed after.
        // First TextBlock found under the widget (W_DisplayName_GothicFont_C has exactly one,
        // Text_CharacterName, directly in the root CanvasPanel).
        auto find_text_block(UObject* widget) -> UObject*
        {
            UObject* found = nullptr;
            UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                if (!obj) { return LoopAction::Continue; }
                auto* cls = obj->GetClassPrivate();
                if (!cls || cls->GetName().find(STR("TextBlock")) == StringType::npos) { return LoopAction::Continue; }
                for (auto* outer = obj->GetOuterPrivate(); outer; outer = outer->GetOuterPrivate())
                {
                    if (outer == widget) { found = obj; return LoopAction::Break; }
                }
                return LoopAction::Continue;
            });
            return found;
        }

        auto set_key_icon(bool gamepad) -> void
        {
            const int device = gamepad ? 1 : 0;
            if (device == m_key_device) { return; }
            auto* image = weak_get(m_image_key);
            auto* brush_fn = static_cast<UFunction*>(weak_get(m_fn_set_brush));
            if (!is_usable(image) || !brush_fn) { return; }

            // Prefer the requested device's glyph; if it didn't resolve (the gamepad path may be wrong
            // on this build) fall back to the other so the WHOLE prompt still shows instead of
            // vanishing - the m_key_device<0 gate in update() otherwise hides everything on a gamepad.
            auto* tex = gamepad ? weak_get(m_tex_y) : weak_get(m_tex_e);
            if (!tex)
            {
                if (!m_logged_key_fallback)
                {
                    m_logged_key_fallback = true;
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] glyph key texture for device={} not resolved; using fallback so the prompt still shows.\n"), device);
                }
                tex = gamepad ? weak_get(m_tex_e) : weak_get(m_tex_y);
            }
            if (!tex) { return; }

            if (set_brush(image, brush_fn, tex, STR("glyph keyicon")))
            {
                m_key_device = device;
                if (!m_logged_icon)
                {
                    m_logged_icon = true;
                    Output::send<LogLevel::Verbose>(STR("[LetMeCraft] glyph key icon set (device={}).\n"), device);
                }
            }
        }

        // The generic interaction icon - confirmed loaded by the glyph-tex sweep. Used when the
        // per-station texture path doesn't resolve, so we show a REAL icon instead of UImage's default
        // white box. (glyphicon9 will plug in the exact per-station icons once the sweep names them.)
        static constexpr const TCHAR* kFallbackStationTex = STR("/Game/UI/Textures/Common/Icons/T_Interaction_Execute.T_Interaction_Execute");

        auto set_station_icon(const TCHAR* path) -> void
        {
            if (!path) { return; }
            if (m_station_path_built == path && weak_get(m_station_tex)) { return; }

            UObject* tex = nullptr;
            try { tex = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); } catch (...) {}
            bool fell_back = false;
            if (!tex)
            {
                try { tex = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kFallbackStationTex); } catch (...) {}
                fell_back = true;
            }

            auto* image = weak_get(m_image_station);
            auto* brush_fn = static_cast<UFunction*>(weak_get(m_fn_set_brush));
            if (!tex || !is_usable(image) || !brush_fn) { return; }

            if (set_brush(image, brush_fn, tex, STR("glyph staticon")))
            {
                m_station_tex = tex;
                m_station_path_built = path;
                // The symbol's brush just changed (and the first station was never tinted), so (re)apply
                // the symbol tint now: the resolved earthy text color if we have it, else the earthy
                // fallback. apply_brush_tint re-sets the brush internally to repaint, preserving the texture.
                float sc[4]; pick_color(m_text_color, m_text_color_valid, kTintEarthy, sc);
                apply_brush_tint(image, sc, 2, tex, STR("glyph tint-sym"));
                if (fell_back && !m_logged_station_fallback)
                {
                    m_logged_station_fallback = true;
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] glyph station icon '{}' not loaded; using generic Execute icon for now.\n"), path);
                }
            }
        }

        auto set_brush(UObject* image, UFunction* brush_fn, UObject* tex, const TCHAR* ctx) -> bool
        {
            auto call = begin_call_with_function(image, brush_fn, ctx);
            set_param(call, STR("Texture"), &tex, 8, ctx);
            set_bool_param(call, STR("bMatchSize"), false, ctx);
            return invoke_call(call, ctx);
        }

        // Tint a UImage's brush to an RGBA color. SetColorAndOpacity never visibly recolored a
        // SetBrushFromTexture image on this build, so we use only PROVEN primitives:
        //  PRIMARY (A): write the 16-byte FLinearColor straight into the live Brush.TintColor.SpecifiedColor
        //    via the same property traversal that read_brush_resource used (proven), then re-set the brush
        //    texture (SetBrushFromTexture, proven) to force Slate to repaint - it only touches ResourceObject
        //    so the TintColor we just wrote is preserved. No new UFunction has to resolve for this to work.
        //  FALLBACK (B): /Script/UMG.Image:SetBrushTintColor(FSlateColor) - size discovered at call time.
        // purpose: 0=outline 1=fill 2=symbol (selects the one-shot log latch). retexture = the texture the
        // image currently carries (re-set to repaint). Degrades to untinted on failure; never throws out.
        auto apply_brush_tint(UObject* image, const float (&rgba)[4], int purpose,
                              UObject* retexture, const TCHAR* ctx) -> void
        {
            if (!is_usable(image)) { return; }
            bool& latch = (purpose == 0) ? m_logged_tint_outline : (purpose == 1) ? m_logged_tint_fill : m_logged_tint_symbol;
            const TCHAR* pn = (purpose == 0) ? STR("outline") : (purpose == 1) ? STR("fill") : STR("symbol");

            // ---- PRIMARY (A): direct Brush.TintColor.SpecifiedColor write. ----
            bool wrote_direct = false;
            run_guarded(STR("glyph tint-direct"), [&] {
                auto* brush_prop = CastField<FStructProperty>(image->GetPropertyByNameInChain(STR("Brush")));
                if (!brush_prop) { return; }
                void* brush_ptr = brush_prop->ContainerPtrToValuePtr<void>(image);
                if (!brush_ptr) { return; }
                auto brush_struct = brush_prop->GetStruct(); // TObjectPtr<UScriptStruct>
                if (!brush_struct) { return; }

                auto* tint_prop = CastField<FStructProperty>(brush_struct->FindProperty(FName(STR("TintColor"), FNAME_Find)));
                if (!tint_prop) { return; }
                void* tint_ptr = tint_prop->ContainerPtrToValuePtr<void>(brush_ptr);
                if (!tint_ptr) { return; }
                auto tint_struct = tint_prop->GetStruct();
                if (!tint_struct) { return; }

                auto* spec_prop = CastField<FStructProperty>(tint_struct->FindProperty(FName(STR("SpecifiedColor"), FNAME_Find)));
                if (!spec_prop) { return; }
                const int32_t spec_off = spec_prop->GetOffset_Internal();
                if (spec_off < 0 || spec_prop->GetSize() < 16) { return; } // FLinearColor = 16 bytes
                std::memcpy(static_cast<uint8_t*>(tint_ptr) + spec_off, &rgba[0], 16);

                // ColorUseRule = 0 (ESlateColorStylingMode::UseColor_Specified) so SpecifiedColor is used.
                if (auto* rule_prop = tint_struct->FindProperty(FName(STR("ColorUseRule"), FNAME_Find)))
                {
                    const int32_t ro = rule_prop->GetOffset_Internal();
                    if (ro >= 0 && rule_prop->GetSize() >= 1)
                    {
                        uint8_t v = 0;
                        std::memcpy(static_cast<uint8_t*>(tint_ptr) + ro, &v, 1);
                    }
                }
                wrote_direct = true;
            });

            if (wrote_direct)
            {
                if (auto* brush_fn = static_cast<UFunction*>(weak_get(m_fn_set_brush)))
                {
                    if (is_usable(retexture)) { set_brush(image, brush_fn, retexture, ctx); } // repaint; keeps TintColor
                }
                if (!latch)
                {
                    latch = true;
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] glyph tint {} OK via direct-brush ({:.2f},{:.2f},{:.2f},{:.2f}).\n"),
                        pn, rgba[0], rgba[1], rgba[2], rgba[3]);
                }
                return;
            }

            // ---- FALLBACK (B): SetBrushTintColor(FSlateColor TintColor). ----
            if (auto* fn = static_cast<UFunction*>(weak_get(m_fn_set_brush_tint)))
            {
                auto call = begin_call_with_function(image, fn, ctx);
                if (auto* prop = find_call_property(call, STR("TintColor"), -1, ctx)) // -1: discover size only
                {
                    const int32_t sz = prop->GetSize();
                    if (sz >= 16 && static_cast<size_t>(prop->GetOffset_Internal()) + static_cast<size_t>(sz) <= call.params.size())
                    {
                        std::vector<uint8_t> buf(static_cast<size_t>(sz), 0); // zero => ColorUseRule = 0
                        std::memcpy(buf.data(), &rgba[0], 16);                // FLinearColor at offset 0
                        set_param(call, STR("TintColor"), buf.data(), sz, ctx); // real size, never -1
                        if (invoke_call(call, ctx) && !latch)
                        {
                            latch = true;
                            Output::send<LogLevel::Verbose>(
                                STR("[LetMeCraft] glyph tint {} OK via SetBrushTintColor (sz={}).\n"), pn, sz);
                        }
                        return;
                    }
                }
            }

            if (!latch)
            {
                latch = true;
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] glyph tint {} FAILED (direct write + SetBrushTintColor both unavailable); icon shows untinted.\n"), pn);
            }
        }

        auto rebuild_name(const StringType& name) -> void
        {
            if (name == m_name_built) { return; }
            m_name_built = name;
            m_name_text = FText(name.c_str()); // Conv_StringToText - game thread only.
            m_name_dirty = true;
        }

        // Push the station name onto the TextBlock ONCE per change (not every frame). In this vendored
        // RC::Unreal build FText is treated as POD: copy/operator= duplicate the Data/SharedRefCollector
        // pointers WITHOUT AddRef and there is no releasing destructor (see FText.cpp), so re-asserting
        // 10-60x/sec would risk a refcount desync on the shared ITextData. The widget is standalone with
        // no text binding fighting us, so a single SetText holds. dirty is cleared only on success.
        auto apply_name() -> void
        {
            if (!m_name_dirty) { return; }
            auto* text_block = weak_get(m_text_block);
            auto* set_text_fn = static_cast<UFunction*>(weak_get(m_fn_set_text));
            if (!is_usable(text_block) || !set_text_fn) { return; }

            auto call = begin_call_with_function(text_block, set_text_fn, STR("glyph settext"));
            if (auto* prop = find_call_property(call, STR("InText"), -1, STR("glyph settext")))
            {
                *reinterpret_cast<FText*>(call.params.data() + prop->GetOffset_Internal()) = m_name_text;
                if (invoke_call(call, STR("glyph settext"))) { m_name_dirty = false; }
            }
        }

        auto position_and_label(UObject* controller, UObject* widget, UObject* avatar) -> void
        {
            FVector head{};
            if (!head_anchor(avatar, head)) { hide(); return; }

            auto* project_fn = static_cast<UFunction*>(weak_get(m_fn_project));
            if (!project_fn) { hide(); return; }
            auto proj = begin_call_with_function(controller, project_fn, STR("glyph project"));
            set_param(proj, STR("WorldLocation"), &head, static_cast<int32_t>(sizeof(FVector)), STR("glyph project"));
            set_bool_param(proj, STR("bPlayerViewportRelative"), false, STR("glyph project"));
            if (!invoke_call(proj, STR("glyph project"))) { hide(); return; }
            if (!get_bool_param(proj, STR("ReturnValue"), STR("glyph project"))) { hide(); return; }

            FVector2D screen{};
            get_param(proj, STR("ScreenLocation"), &screen, kFVector2DSize, STR("glyph project"));
            screen.SetY(screen.GetY() - kAboveHeadPx);

            show();
            if (auto* pos_fn = static_cast<UFunction*>(weak_get(m_fn_set_pos)))
            {
                auto pc = begin_call_with_function(widget, pos_fn, STR("glyph setpos"));
                set_param(pc, STR("Position"), &screen, kFVector2DSize, STR("glyph setpos"));
                set_bool_param(pc, STR("bRemoveDPIScale"), true, STR("glyph setpos"));
                invoke_call(pc, STR("glyph setpos"));
            }
            apply_name();
        }

        auto show() -> void { if (!m_visible) { apply_visibility(weak_get(m_widget), true); } }
        auto hide() -> void { if (m_visible) { apply_visibility(weak_get(m_widget), false); } }

        auto apply_visibility(UObject* widget, bool visible) -> void
        {
            auto* fn = static_cast<UFunction*>(weak_get(m_fn_set_vis));
            if (!widget || !fn) { return; }
            auto call = begin_call_with_function(widget, fn, STR("glyph vis"));
            uint8_t value = visible ? kVisSelfHitTestInvisible : kVisCollapsed;
            set_param(call, STR("InVisibility"), &value, 1, STR("glyph vis"));
            if (invoke_call(call, STR("glyph vis"))) { m_visible = visible; }
        }

        // The skeleton bone dump confirmed the head bone is "Head"; use it directly (it follows the
        // animation, so the icon stays over the head even when the cook is seated). Capsule top is the
        // fallback if the socket call ever fails.
        auto head_anchor(UObject* avatar, FVector& out) -> bool
        {
            if (!is_usable(avatar)) { return false; }
            const auto base = read_actor_location(avatar);
            if (!base.found) { return false; }

            auto* mesh = read_object(avatar, STR("Mesh"));
            auto* socket_fn = static_cast<UFunction*>(weak_get(m_fn_socket));
            if (is_usable(mesh) && socket_fn)
            {
                FVector socket{};
                if (socket_location(mesh, socket_fn, STR("Head"), socket) &&
                    socket.Z() > base.value.Z() - 140.0 && socket.Z() < base.value.Z() + 400.0)
                {
                    out = FVector{socket.X(), socket.Y(), socket.Z() + kHeadExtraZ};
                    return true;
                }
            }

            // Fallback: actor location is the capsule centre; top of capsule ~ above the head.
            double up = 96.0;
            if (auto* capsule = read_object(avatar, STR("CapsuleComponent")))
            {
                if (is_usable(capsule))
                {
                    if (auto* half = capsule->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight")))
                    {
                        up = static_cast<double>(*half) + kHeadExtraZ;
                    }
                }
            }
            out = FVector{base.value.X(), base.value.Y(), base.value.Z() + up};
            return true;
        }

        auto socket_location(UObject* mesh, UFunction* socket_fn, const TCHAR* socket_name, FVector& out) -> bool
        {
            FName name{socket_name, FNAME_Find};
            auto call = begin_call_with_function(mesh, socket_fn, STR("glyph socket"));
            set_param(call, STR("InSocketName"), &name, static_cast<int32_t>(sizeof(FName)), STR("glyph socket"));
            if (!invoke_call(call, STR("glyph socket"))) { return false; }
            return get_param(call, STR("ReturnValue"), &out, static_cast<int32_t>(sizeof(FVector)), STR("glyph socket"));
        }

        GameObjects& m_objects;

        bool m_warmed{};
        int m_warmup_attempts{};
        Clock::time_point m_next_warmup_at{};
        FWeakObjectPtr m_fn_create{};
        FWeakObjectPtr m_fn_add{};
        FWeakObjectPtr m_fn_set_pos{};
        FWeakObjectPtr m_fn_set_align{};
        FWeakObjectPtr m_fn_set_vis{};
        FWeakObjectPtr m_fn_project{};
        FWeakObjectPtr m_fn_socket{};
        FWeakObjectPtr m_fn_set_brush{};
        FWeakObjectPtr m_fn_add_to_canvas{};
        FWeakObjectPtr m_fn_slot_pos{};
        FWeakObjectPtr m_fn_slot_size{};
        FWeakObjectPtr m_fn_slot_align{};
        FWeakObjectPtr m_fn_slot_anchors{};
        FWeakObjectPtr m_fn_tint{};           // SetColorAndOpacity (legacy; diagnostic only)
        FWeakObjectPtr m_fn_set_brush_tint{}; // SetBrushTintColor (fallback tint mechanism B)
        FWeakObjectPtr m_fn_set_text{};
        FWeakObjectPtr m_widget_class{};
        FWeakObjectPtr m_image_class{};
        FWeakObjectPtr m_tex_e{};
        FWeakObjectPtr m_tex_y{};
        FWeakObjectPtr m_station_tex{};
        FWeakObjectPtr m_circle_tex{};
        FWeakObjectPtr m_widget{};
        FWeakObjectPtr m_image_key{};
        FWeakObjectPtr m_image_station{};         // (3) earthy symbol, on top
        FWeakObjectPtr m_image_station_bg{};      // (2) black fill
        FWeakObjectPtr m_image_station_outline{}; // (1) earthy ring, drawn first (bottom)
        FWeakObjectPtr m_text_block{};

        FText m_name_text{};
        StringType m_name_built{};
        StringType m_station_path_built{};

        bool m_name_dirty{};
        bool m_added{};
        bool m_visible{};
        bool m_content_built{};
        int m_key_device{-1};
        int m_pos_fault_logged{};
        Clock::time_point m_next_tex_retry{};
        bool m_logged_warm{};
        bool m_logged_icon{};
        bool m_logged_tex{};
        bool m_logged_key_fallback{};
        bool m_logged_station_fallback{};
        bool m_logged_tint_outline{};
        bool m_logged_tint_fill{};
        bool m_logged_tint_symbol{};
        // ---- glyphicon15: prompt-text color read (ring + symbol match the text) ----
        float m_text_color[4]{};          // TextBlock ColorAndOpacity SpecifiedColor (rule==0, non-white)
        bool m_text_color_valid{};
        bool m_text_color_ready{};        // got a usable color, or gave up -> stop polling
        bool m_text_color_upgrade_done{}; // one-shot: post-build re-tint ran for this widget
        int m_text_color_attempts{};
        Clock::time_point m_next_text_color_at{};
        bool m_logged_text_color_values{};
    };
}

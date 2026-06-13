#pragma once

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/UnrealInitializer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <exception>
#include <excpt.h>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace RC::Unreal::Windows
{
    extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(HMODULE hModule, const char* lpProcName);
}

namespace lmc
{
    using Clock = std::chrono::steady_clock;

    // --- SEH backstop -------------------------------------------------------
    // run_guarded only covers C++ exceptions; an access violation is a hardware
    // (SEH) exception and kills the whole game (v0.7.14 in-game crash: AV in the
    // kill-guard GAS scan reading 0x100000048). Minimal mirrors of
    // EXCEPTION_RECORD/EXCEPTION_POINTERS - the layout is fixed by the Win64 ABI
    // and <Windows.h> is not included in this TU.
    struct SehExceptionRecord
    {
        unsigned long ExceptionCode;
        unsigned long ExceptionFlags;
        SehExceptionRecord* InnerRecord;
        void* ExceptionAddress;
        unsigned long NumberParameters;
        unsigned long long ExceptionInformation[15];
    };

    struct SehExceptionPointers
    {
        SehExceptionRecord* ExceptionRecord;
        void* ContextRecord;
    };

    struct SehCrashInfo
    {
        unsigned long code{};
        void* instruction_address{};
        unsigned long long fault_address{};
    };

    // C++ exceptions travel as this SEH code; they belong to the try/catch
    // blocks (run_guarded, the tick's own catch) and must keep propagating.
    constexpr unsigned long kCxxExceptionSehCode = 0xE06D7363UL;

    inline auto seh_capture(SehCrashInfo& crash, unsigned long code, void* info_raw) -> int
    {
        if (code == kCxxExceptionSehCode) { return EXCEPTION_CONTINUE_SEARCH; }

        crash.code = code;
        if (auto* info = static_cast<SehExceptionPointers*>(info_raw); info && info->ExceptionRecord)
        {
            crash.instruction_address = info->ExceptionRecord->ExceptionAddress;
            if (info->ExceptionRecord->NumberParameters >= 2)
            {
                // For access violations [0] is read(0)/write(1), [1] the address.
                crash.fault_address = info->ExceptionRecord->ExceptionInformation[1];
            }
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // The function lexically containing __try must not need C++ unwinding
    // (C2712), so it holds no destructible locals; the callable's own frames
    // are unrestricted.
    template <typename Callable>
    auto invoke_with_seh(const Callable& callable, SehCrashInfo& crash) -> bool
    {
        __try
        {
            callable();
            return true;
        }
        __except (seh_capture(crash, _exception_code(), _exception_info()))
        {
            return false;
        }
    }

    constexpr double kMaxTargetDistance = 700.0;
    constexpr double kMaxTargetDistanceSquared = kMaxTargetDistance * kMaxTargetDistance;
    // The manual E trigger fires when the player stands up to 4.0m from the
    // STATION pivot (user-calibrated 2026-06-13, was 3.5m; the pivot can sit
    // ~1m past the usable face - v0.7.10). v0.8.7: there is no minimum gate -
    // the mod auto-walks the player to kApproachStopDistance, so a repeat E
    // pressed while standing point-blank at the station must not be rejected.
    constexpr double kMaxStationDistance = 400.0;
    constexpr double kMaxStationDistanceSquared = kMaxStationDistance * kMaxStationDistance;
    // Every station's UInteractiveComponent reports interactDist=250 (v0.8.6
    // diag) - the player simply cannot be sensed from beyond 2.5m, which was
    // the remaining failure mode (both v0.8.6 misses started at 3.0-3.2m).
    // The mod walks the player in to 2.0m: inside the radius with 0.5m of
    // margin, and far enough to feel natural (1.4m and 1.6m both read as
    // "pressed against the station" to the user).
    constexpr double kApproachStopDistance = 200.0;
    constexpr double kHeldFallbackDistance = 900.0;
    constexpr double kHeldFallbackDistanceSquared = kHeldFallbackDistance * kHeldFallbackDistance;
    // No teleports: teleport + per-second enforcement provoked the game's own routine
    // simulation into warping NPCs hundreds of meters (v0.6.3 logs). v0.8.0 (user
    // redesign): the NPC walks BEHIND THE PLAYER'S BACK - the retreat point lies on
    // the station->player ray extended past the player (the player faces the station
    // when pressing E, so that point is behind them). It is parked there for the
    // whole hold and resumes its routine when the hold expires.
    constexpr double kBehindPlayerDistance = 200.0;
    // The routine drags the NPC back toward the station between cancels; when it
    // drifts farther than this from the behind-point, the move is re-issued (1Hz).
    constexpr double kBehindHoldTolerance = 150.0;
    constexpr double kBehindHoldToleranceSquared = kBehindHoldTolerance * kBehindHoldTolerance;
    constexpr float kRetreatAcceptance = 40.0f;
    constexpr auto kManualRequestCooldown = std::chrono::milliseconds{1800};
    constexpr auto kRetreatDelay = std::chrono::milliseconds{250};
    constexpr auto kStandStillDelay = std::chrono::milliseconds{1000};
    // 17s (user request v0.8.2, was 10s): the NPC stands behind the player for
    // 17 seconds, then the hold ends and its routine resumes naturally. The
    // parking now survives a successful take too - the player works at the
    // station while the NPC keeps standing behind until the hold expires.
    constexpr auto kHoldDuration = std::chrono::seconds{17};
    constexpr auto kHoldEnforceInterval = std::chrono::milliseconds{1000};
    constexpr auto kClaimRetryInterval = std::chrono::milliseconds{500};
    // A few attempts are enough: either the player-claim works within ~2s or it never
    // will (requirement checks); endless retries were a major FPS drain in v0.7.2.
    // NOTE: a claim held by the player does NOT lock the station for the player
    // (proven by Huno in v0.7.3); the spot-cooldown DOES lock it and is not used.
    constexpr int kClaimMaxAttempts = 4;
    // The bridge: poll "is the spot free" and start the player's interaction the
    // moment it is. The player's movement is locked for at most kMoveLockDuration.
    // 1s (was 1.5s): the spot is now force-released at eviction start, so the
    // station unlocks for the player sooner (v0.8.0 user feedback: "stations take
    // too long to unlock").
    constexpr auto kPlayerUseDelay = std::chrono::milliseconds{1000};
    // Take attempts are gated on the approach finishing: all three water-barrel
    // grabs in the v0.8.8 log fired on attempt=1 while the player was still
    // WALKING past the barrel, and each cost seconds of mismatch recovery.
    // The grace is the fallback when the approach cannot finish (blocked path,
    // E pressed from an unwalkable angle) - past it attempts run regardless.
    constexpr auto kApproachActivationGrace = std::chrono::milliseconds{2500};
    // Attempts repeat until the auto-take window (player_use_until, =
    // kMoveLockDuration) closes OR the attempt cap below hits. Flat
    // 10Hz cadence for every attempt: each one is a cancel+activate pair, the
    // claim release lands ~100ms after a cancel and the routine re-claims in
    // ~250ms, so any slower fallback loses the race - v0.8.3 showed the
    // routine parking in a claim-only state (no active ability) where the old
    // 250ms fallback missed the release window every single time.
    constexpr auto kPlayerUseBurstInterval = std::chrono::milliseconds{100};
    // Hard cap on take attempts (v0.8.12). Every observed success across
    // v0.8.6-v0.8.11 landed by attempt 19 (forge=1, whetstone=18, cauldron=19);
    // the one window that ran past that (84 attempts vs Snaf's stuck cauldron
    // claim, v0.8.11 log 21:38) never succeeded, kept the player frozen the
    // full 10s, and the cancel/claim/activate churn preceded a game-side AV
    // crash (G1R+0x1e59833, poisoned pointer 0x100000010). 35 = ~1.8x margin
    // over the worst observed success; past it the spot is stuck and more
    // hammering only raises the crash exposure.
    constexpr int kPlayerUseMaxAttempts = 35;
    // When the take window closes WITHOUT a success the spot stays with the
    // NPC - holding it parked (and kill-guard-fighting its routine) for the
    // rest of the 17s is pure GAS churn against a station the player did not
    // get. Shorten the remaining hold to this tail instead.
    constexpr auto kFailedTakeHoldTail = std::chrono::seconds{3};
    // Also the auto-take window: attempts stop when the lock ends so a freely
    // walking player is never snapped into a passing interactive. The NPC's
    // 17s parking continues beyond it.
    constexpr auto kMoveLockDuration = std::chrono::seconds{10};
    // ResetIgnoreMoveInput is retried until it actually executes: the flag is cleared
    // only after a successful call, so a failed unlock can never strand the player.
    constexpr auto kMoveUnlockRetryInterval = std::chrono::milliseconds{250};
    constexpr int kMoveUnlockMaxAttempts = 20;
    // One-time resolution of the controller UFunctions. Attempts are SPACED OUT and
    // never permanently abandoned: right after a save load the GUObjectArray scan can
    // keep throwing for several seconds straight (v0.7.8 burned a 50-per-tick budget
    // in 2.7s and disabled the lock for the whole session).
    constexpr auto kWarmupRetryInterval = std::chrono::milliseconds{500};
    // After a mismatch where the NPC itself stole the interaction target, it is sent
    // away again - at most this often.
    constexpr auto kReRetreatInterval = std::chrono::milliseconds{1000};
    // The activation targets the interactive nearest to the player, so it must not
    // run at all while the evicted NPC stands next to the player (v0.7.11: every
    // mismatch grabbed the NPC at 1-2m while filterActor/sensorTarget held the
    // station). Attempts are spent waiting until the NPC clears this radius.
    constexpr double kNpcClearDistance = 250.0;
    constexpr double kNpcClearDistanceSquared = kNpcClearDistance * kNpcClearDistance;
    // The activation runs its own live nearest-interactive query (v0.7.10) -
    // the sensor/ability pins cannot override it. In a station cluster the
    // query can prefer a NEIGHBOR of the target (Huno's anvil: the forge won
    // at the 2.0m stop and the attempt=1 activation walked the player to the
    // wrong station - v0.8.14 log 00:26:38). Attempts are now gated on the
    // query returning OUR station; while a neighbor wins, the approach stop
    // shrinks per attempt so the player creeps in until the station is the
    // closest pick. The default 2.0m stop (user-confirmed) is untouched for
    // the conflict-free case.
    constexpr double kApproachMinStopDistance = 120.0;
    constexpr double kApproachStopShrinkStep = 20.0;

    struct BoolRead
    {
        bool found{};
        bool value{};
    };

    struct VectorRead
    {
        bool found{};
        FVector value{};
    };

    struct CraftingCandidate
    {
        UObject* ability{};
        UObject* root_task{};
        UObject* asc{};
        UObject* owner{};
        UObject* avatar{};
        double distance_squared{std::numeric_limits<double>::max()};
        StringType owner_name{};
        StringType avatar_name{};
    };

    struct CraftingSearchResult
    {
        bool found{};
        CraftingCandidate candidate{};
    };

    struct RequestResult
    {
        bool called{};
        CraftingCandidate candidate{};
    };

    // One evicted NPC. Raw UObject* must never be cached across frames: the GC can
    // free the object and is_usable() on a dangling pointer is UB; FWeakObjectPtr
    // validates through the GUObjectArray serial number and resolves to nullptr.
    // Lifecycle: request-end already sent -> NPC walks ~2m away from the station
    // (MoveToLocation) -> stands idle until resume_at while the interaction spot is
    // claimed for the player so the daily routine cannot re-assign it.
    struct ActiveEviction
    {
        Clock::time_point move_at{};
        Clock::time_point stand_still_from{};
        Clock::time_point next_enforce_at{};
        Clock::time_point next_claim_at{};
        Clock::time_point player_use_at{};
        // Fallback for the arrival gate: attempts run once the approach is done
        // OR this passes (the approach may never finish on a blocked path).
        Clock::time_point approach_grace_until{};
        // End of the auto-take window (eviction start + kMoveLockDuration). The
        // parking hold (resume_at) is longer; once this passes the mod stops
        // poking the interact ability so a freely-walking player cannot be
        // snapped into a random interactive by a late activation.
        Clock::time_point player_use_until{};
        Clock::time_point resume_at{};
        Clock::time_point re_retreat_at{};
        FWeakObjectPtr owner{};
        FWeakObjectPtr avatar{};
        FWeakObjectPtr controller{};
        FWeakObjectPtr station{};
        // The NPC's crafting ability INSTANCE survives the whole eviction (GAS
        // abilities are instanced-per-actor on the ASC; every v0.8.x log shows
        // one instance per NPC across all cancels) - cached so the 10Hz combo
        // and the 1Hz kill guard evaluate it directly instead of FindAllOf
        // sweeping the entire object array (the v0.8.10 stutter).
        FWeakObjectPtr ability{};
        // v0.8.16: the NPC's own UGothicAbilitySystemComponent - AddTag/RemoveTag
        // target for the routine-blocker tag.
        FWeakObjectPtr asc{};
        FName spot{};
        FName action{};
        // v0.8.16 routine blocker: a loose tag from the crafting ability's
        // ActivationBlockedTags held on the ASC for the WHOLE eviction (17s hold /
        // 3s failed tail) so the routine cannot re-activate the interaction task
        // in the same tick as our force-release (the v0.8.15 claim wars).
        FName blocker_tag{};
        // The behind-the-player parking point, captured once on the first retreat
        // so later re-issues keep steering to the SAME spot (the player is
        // move-locked, but the camera/rotation may spin freely).
        FVector retreat_dest{};
        bool retreat_dest_known{};
        StringType owner_name{};
        StringType avatar_name{};
        alignas(8) unsigned char claim_token[16]{};
        int claim_attempts{};
        int player_use_attempts{};
        bool spot_known{};
        bool move_issued{};
        bool has_claim{};
        bool claim_gave_up{};
        bool player_use_done{};
        // Set ONLY on a successful take (player_use_done alone also marks a
        // closed/failed window). Once the player runs the station the routine
        // cannot reclaim it, so the kill guard stops cancelling - post-success
        // GAS churn on the NPC ability is crash exposure for zero benefit
        // (v0.8.11: the game died in its own code right after such a take).
        bool player_took_station{};
        bool npc_interaction_disabled{};
        // Arrival gate: set once the player reached the approach stop; no
        // activation while they are still walking (v0.8.9).
        bool approach_done{};
        // Effective approach stop for THIS eviction: starts at the 2.0m
        // default and shrinks while the player's sensor prefers a neighboring
        // interactive over the target station (the activation gate).
        double approach_stop_distance{kApproachStopDistance};
        // v0.8.16 routine blocker state: known = a safe tag was selected from
        // ActivationBlockedTags; applied = it currently sits on the NPC's ASC.
        bool blocker_known{};
        bool blocker_applied{};
    };

    using BYTE = Windows::BYTE;
    using DWORD = Windows::DWORD;
    using HMODULE = Windows::HMODULE;
    using SHORT = short;
    using WORD = unsigned short;

    struct XInputGamepad
    {
        WORD wButtons{};
        BYTE bLeftTrigger{};
        BYTE bRightTrigger{};
        SHORT sThumbLX{};
        SHORT sThumbLY{};
        SHORT sThumbRX{};
        SHORT sThumbRY{};
    };

    struct XInputState
    {
        DWORD dwPacketNumber{};
        XInputGamepad Gamepad{};
    };

    using XInputGetStateFn = DWORD(__stdcall*)(DWORD, XInputState*);

    constexpr WORD kXInputGamepadY = 0x8000;

    auto contains(const StringType& haystack, const StringViewType needle) -> bool
    {
        return haystack.find(needle) != StringType::npos;
    }

    auto contains_any(const StringType& haystack, std::initializer_list<const TCHAR*> needles) -> bool
    {
        for (const auto* needle : needles)
        {
            if (contains(haystack, needle)) { return true; }
        }
        return false;
    }

    auto is_usable(UObject* object) -> bool
    {
        if (!object) { return false; }

        // GetObjectItem is a bare IndexToObject(InternalIndex) lookup with no
        // identity check. A dangling pointer whose memory was reused yields a
        // garbage index; when that index lands in range, the lookup returns some
        // unrelated LIVE item and every flag test below passes - and the caller
        // then dereferences the garbage object (v0.7.14 in-game crash: a stale
        // RootInteractionTask passed here, its bogus class pointer was walked at
        // +0x48 -> EXCEPTION_ACCESS_VIOLATION). A live object's item always
        // points back at the object itself.
        auto* item = object->GetObjectItem();
        if (!item || item->GetUObject() != object) { return false; }
        if (item->IsUnreachable() || item->IsPendingKill()) { return false; }
        if (object->HasAnyFlags(RF_BeginDestroyed) || object->HasAnyFlags(RF_FinishDestroyed)) { return false; }
        if (object->HasAnyFlags(RF_ClassDefaultObject)) { return false; }

        return true;
    }

    auto object_name(UObject* object) -> StringType
    {
        return is_usable(object) ? object->GetFullName() : STR("<null>");
    }

    auto as_actor(UObject* object) -> AActor*
    {
        return is_usable(object) ? Cast<AActor>(object) : nullptr;
    }

    auto weak_get(const FWeakObjectPtr& weak) -> UObject*
    {
        auto* object = weak.Get();
        return is_usable(object) ? object : nullptr;
    }

    // CDO-tolerant weak resolve: is_usable() rejects CDOs by design, but the
    // /Script library CDOs cached for the spot/token calls are exactly that.
    // Same item-identity and destruction checks as is_usable, minus the CDO
    // rejection.
    auto weak_get_cdo(const FWeakObjectPtr& weak) -> UObject*
    {
        auto* object = weak.Get();
        if (!object) { return nullptr; }
        auto* item = object->GetObjectItem();
        if (!item || item->GetUObject() != object) { return nullptr; }
        if (item->IsUnreachable() || item->IsPendingKill()) { return nullptr; }
        if (object->HasAnyFlags(RF_BeginDestroyed) || object->HasAnyFlags(RF_FinishDestroyed)) { return nullptr; }
        return object;
    }

    auto read_bool(UObject* object, const TCHAR* property_name) -> BoolRead
    {
        if (!is_usable(object)) { return {}; }

        auto* property = CastField<FBoolProperty>(object->GetPropertyByNameInChain(property_name));
        if (!property) { return {}; }

        return {true, property->GetPropertyValueInContainer(object)};
    }

    auto read_object(UObject* object, const TCHAR* property_name) -> UObject*
    {
        if (!is_usable(object)) { return nullptr; }

        auto* property = CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(property_name));
        if (!property) { return nullptr; }

        auto* value_ptr = property->ContainerPtrToValuePtr<void>(object);
        auto* value = property->GetObjectPropertyValue(value_ptr);
        return is_usable(value) ? value : nullptr;
    }

    auto write_object_property(UObject* object, const TCHAR* property_name, UObject* value) -> bool
    {
        if (!is_usable(object)) { return false; }

        auto* property = CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(property_name));
        if (!property) { return false; }

        auto* value_ptr = property->ContainerPtrToValuePtr<void>(object);
        if (!value_ptr) { return false; }

        property->SetObjectPropertyValue(value_ptr, value);
        return true;
    }

    struct NameRead
    {
        bool found{};
        FName value{};
    };

    // Reads the leading FName of a property value. Covers FNameProperty directly and
    // struct properties whose first member is an FName (FInteractionSpotHandle,
    // FGameplayTag) — enough for the track-B recon logging.
    auto read_fname_at(UObject* object, const TCHAR* property_name) -> NameRead
    {
        if (!is_usable(object)) { return {}; }

        auto* property = object->GetPropertyByNameInChain(property_name);
        if (!property || property->GetSize() < static_cast<int32_t>(sizeof(FName))) { return {}; }

        auto* value_ptr = property->ContainerPtrToValuePtr<void>(object);
        if (!value_ptr) { return {}; }

        return {true, *static_cast<FName*>(value_ptr)};
    }

    auto fname_to_text(const FName& name) -> StringType
    {
        auto value = name;
        return value.ToString();
    }

    struct TagContainerRead
    {
        bool found{};
        std::vector<FName> tags{};
    };

    // FGameplayTagContainer layout: { TArray<FGameplayTag> GameplayTags;
    // TArray<FGameplayTag> ParentTags; }; TArray header = { void* Data;
    // int32 Num; int32 Max; }; FGameplayTag = one FName (8 bytes, per the
    // G1R dump). Only the leading GameplayTags array is read; the bounds
    // checks turn a garbage layout into found=false instead of a crash.
    auto read_tag_container(UObject* object, const TCHAR* property_name) -> TagContainerRead
    {
        if (!is_usable(object)) { return {}; }

        auto* property = object->GetPropertyByNameInChain(property_name);
        if (!property || property->GetSize() < 16) { return {}; }

        auto* value_ptr = property->ContainerPtrToValuePtr<unsigned char>(object);
        if (!value_ptr) { return {}; }

        struct RawArray
        {
            void* data{};
            int32_t num{};
            int32_t max{};
        } header{};
        std::memcpy(&header, value_ptr, sizeof(header));

        if (header.num < 0 || header.max < header.num || header.max > 256) { return {}; }
        if (header.num > 0 && !header.data) { return {}; }

        TagContainerRead result{};
        result.found = true;
        result.tags.reserve(static_cast<size_t>(header.num));
        for (int32_t index = 0; index < header.num; ++index)
        {
            FName tag{};
            std::memcpy(&tag, static_cast<unsigned char*>(header.data) + index * 8, 8);
            result.tags.push_back(tag);
        }
        return result;
    }

    auto tags_to_text(const TagContainerRead& container) -> StringType
    {
        if (!container.found) { return STR("<missing>"); }
        if (container.tags.empty()) { return STR("[]"); }

        StringType text{STR("[")};
        for (size_t index = 0; index < container.tags.size(); ++index)
        {
            if (index > 0) { text += STR(","); }
            text += fname_to_text(container.tags[index]);
        }
        text += STR("]");
        return text;
    }

    auto to_lower_copy(StringType text) -> StringType
    {
        std::transform(text.begin(), text.end(), text.begin(), [](TCHAR ch) {
            return static_cast<TCHAR>(::towlower(ch));
        });
        return text;
    }

    // Only a tag that plainly names an interaction/busy state is safe to put on
    // a live NPC; anything else (Dead, Combat, ...) risks AI side effects, so
    // no match = no tag (v0.8.15 behavior, the diag log informs the next try).
    auto choose_blocker_tag(const TagContainerRead& activation_blocked) -> NameRead
    {
        if (!activation_blocked.found) { return {}; }

        for (const auto& tag : activation_blocked.tags)
        {
            const auto lowered = to_lower_copy(fname_to_text(tag));
            if (contains_any(lowered,
                    {STR("interact"), STR("dialog"), STR("convers"), STR("talk"),
                     STR("busy"), STR("block"), STR("trade")}))
            {
                return {true, tag};
            }
        }
        return {};
    }

    // Runtime-verified ProcessEvent call: every parameter is located by name in the
    // UFunction and its size is asserted against what we pass, so a layout mismatch
    // degrades into a logged no-op instead of a wild memory write.
    struct FunctionCall
    {
        UObject* object{};
        UFunction* function{};
        std::vector<uint8_t> params{};
        bool ok{};
    };

    auto begin_call_with_function(UObject* object, UFunction* function, const TCHAR* context) -> FunctionCall
    {
        FunctionCall call{};
        if (!object || !function)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: call aborted (object={} function={}).\n"),
                context,
                object ? STR("ok") : STR("null"),
                function ? STR("ok") : STR("null"));
            return call;
        }

        call.object = object;
        call.function = function;
        call.params.resize(static_cast<size_t>(function->GetParmsSize()), 0);
        call.ok = true;
        return call;
    }

    auto begin_call(UObject* object, const TCHAR* function_name, const TCHAR* context) -> FunctionCall
    {
        if (!is_usable(object))
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: target object unusable for {}.\n"),
                context,
                function_name);
            return {};
        }

        auto* function = object->GetFunctionByNameInChain(function_name);
        if (!function)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: function {} not found on {}.\n"),
                context,
                function_name,
                object_name(object));
            return {};
        }

        return begin_call_with_function(object, function, context);
    }

    // For engine-native functions the UFunction is resolved by its full path first:
    // this avoids per-instance FuncMap chain walks and is immune to exotic class state.
    auto begin_native_call(UObject* object, const TCHAR* function_path, const TCHAR* function_name, const TCHAR* context) -> FunctionCall
    {
        if (!is_usable(object))
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: target object unusable for {}.\n"),
                context,
                function_name);
            return {};
        }

        if (auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, function_path))
        {
            return begin_call_with_function(object, function, context);
        }

        return begin_call(object, function_name, context);
    }

    // UFunction lookup over the super chain ONLY (UField::Next + SuperStruct reads).
    // IncludeInterfaces is the actual root cause of the "Array failed invariants
    // check" throws on this build's PlayerController chain: with that flag the
    // iterator reads the UClass::Interfaces TArray (offset 0x1D8), which does not
    // parse as a TArray on the G1R player controller classes - and RE-UE4SS's own
    // GetFunctionByNameInChain passes IncludeSuper|IncludeInterfaces (UObject.cpp:639),
    // which is why it threw since v0.7.4. Property walks never include interfaces,
    // which is why they always worked on the same chain. SetIgnoreMoveInput and
    // ResetIgnoreMoveInput are native on /Script/Engine.PlayerController, so the
    // super chain is all we need.
    auto find_function_via_children(UObject* object, const TCHAR* function_name) -> UFunction*
    {
        if (!is_usable(object)) { return nullptr; }

        const FName target{function_name, FNAME_Find};
        for (auto* function : TFieldRange<UFunction>(object->GetClassPrivate(), EFieldIterationFlags::IncludeSuper))
        {
            if (function && function->GetNamePrivate() == target) { return function; }
        }
        return nullptr;
    }

    auto find_call_property(FunctionCall& call, const TCHAR* name, int32_t size, const TCHAR* context) -> FProperty*
    {
        if (!call.ok) { return nullptr; }

        auto* property = call.function->FindProperty(FName(name, FNAME_Find));
        if (!property)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: param {} not found on {}, call aborted.\n"),
                context,
                name,
                call.function->GetName());
            call.ok = false;
            return nullptr;
        }

        const auto offset = property->GetOffset_Internal();
        if ((size >= 0 && property->GetSize() != size) || offset < 0 ||
            static_cast<size_t>(offset) + static_cast<size_t>(property->GetSize()) > call.params.size())
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: param {} layout mismatch (expected size {}, actual size {}, offset {}), call aborted.\n"),
                context,
                name,
                size,
                property->GetSize(),
                offset);
            call.ok = false;
            return nullptr;
        }

        return property;
    }

    auto set_param(FunctionCall& call, const TCHAR* name, const void* data, int32_t size, const TCHAR* context) -> void
    {
        if (auto* property = find_call_property(call, name, size, context))
        {
            std::memcpy(call.params.data() + property->GetOffset_Internal(), data, static_cast<size_t>(size));
        }
    }

    auto set_bool_param(FunctionCall& call, const TCHAR* name, bool value, const TCHAR* context) -> void
    {
        auto* property = find_call_property(call, name, -1, context);
        if (!property) { return; }

        auto* bool_property = CastField<FBoolProperty>(property);
        if (!bool_property)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: param {} is not a bool, call aborted.\n"),
                context,
                name);
            call.ok = false;
            return;
        }

        bool_property->SetPropertyValueInContainer(call.params.data(), value);
    }

    auto invoke_call(FunctionCall& call, const TCHAR* context) -> bool
    {
        if (!call.ok) { return false; }

        try
        {
            call.object->ProcessEvent(call.function, call.params.data());
            return true;
        }
        catch (const std::exception& exception)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: {} recovered from exception: {}.\n"),
                context,
                call.function->GetName(),
                ensure_str(exception.what()));
        }
        catch (...)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: {} recovered from unknown exception.\n"),
                context,
                call.function->GetName());
        }

        call.ok = false;
        return false;
    }

    auto get_param(FunctionCall& call, const TCHAR* name, void* data, int32_t size, const TCHAR* context) -> bool
    {
        if (auto* property = find_call_property(call, name, size, context))
        {
            std::memcpy(data, call.params.data() + property->GetOffset_Internal(), static_cast<size_t>(size));
            return true;
        }
        return false;
    }

    auto get_bool_param(FunctionCall& call, const TCHAR* name, const TCHAR* context) -> bool
    {
        auto* property = find_call_property(call, name, -1, context);
        if (!property) { return false; }

        auto* bool_property = CastField<FBoolProperty>(property);
        if (!bool_property) { return false; }

        return bool_property->GetPropertyValueInContainer(call.params.data());
    }

    auto get_object_param(FunctionCall& call, const TCHAR* name, const TCHAR* context) -> UObject*
    {
        auto* property = find_call_property(call, name, -1, context);
        if (!property) { return nullptr; }

        auto* object_property = CastField<FObjectPropertyBase>(property);
        if (!object_property) { return nullptr; }

        return object_property->GetObjectPropertyValue(
            object_property->ContainerPtrToValuePtr<void>(call.params.data()));
    }

    // AAIController::MoveToLocation — the NPC walks to the destination on its own.
    auto issue_move_to_location(
        UObject* controller, const FVector& destination, const TCHAR* context) -> bool
    {
        auto call = begin_native_call(
            controller,
            STR("/Script/AIModule.AIController:MoveToLocation"),
            STR("MoveToLocation"),
            context);
        auto dest = destination;
        auto acceptance = kRetreatAcceptance;
        UObject* filter_class = nullptr;
        set_param(call, STR("Dest"), &dest, static_cast<int32_t>(sizeof(FVector)), context);
        set_param(call, STR("AcceptanceRadius"), &acceptance, 4, context);
        set_bool_param(call, STR("bStopOnOverlap"), true, context);
        set_bool_param(call, STR("bUsePathfinding"), true, context);
        set_bool_param(call, STR("bProjectDestinationToNavigation"), true, context);
        set_bool_param(call, STR("bCanStrafe"), false, context);
        set_param(call, STR("FilterClass"), &filter_class, 8, context);
        set_bool_param(call, STR("bAllowPartialPath"), true, context);
        return invoke_call(call, context);
    }

    auto root_task_is_crafting(UObject* root_task) -> bool
    {
        const auto name = object_name(root_task);
        return contains_any(
            name,
            {
                STR("Alchemy"),
                STR("Anvil"),
                STR("Cauldron"),
                STR("Cook"),
                STR("Forge"),
                STR("Fry"),
                STR("Grind"),
                STR("Leatherworking"),
                STR("Roast"),
                STR("Saw"),
                STR("Smith"),
                STR("Stove"),
                STR("Tanning"),
                STR("Workbench"),
                STR("WorkBench"),
                STR("Whet"),
            });
    }

    auto read_actor_location_raw(UObject* actor) -> VectorRead
    {
        auto* root_component = read_object(actor, STR("RootComponent"));
        if (!root_component) { return {}; }

        auto* location = root_component->GetValuePtrByPropertyNameInChain<FVector>(STR("RelativeLocation"));
        if (!location) { return {}; }

        return {true, *location};
    }

    auto read_actor_location(UObject* actor) -> VectorRead
    {
        auto* native_actor = as_actor(actor);
        if (native_actor)
        {
            try
            {
                return {true, native_actor->K2_GetActorLocation()};
            }
            catch (const std::exception& exception)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] K2_GetActorLocation recovered from exception on {}: {}.\n"),
                    object_name(actor),
                    ensure_str(exception.what()));
            }
            catch (...)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] K2_GetActorLocation recovered from unknown exception on {}.\n"),
                    object_name(actor));
            }
        }

        return read_actor_location_raw(actor);
    }

    auto distance_squared(const FVector& a, const FVector& b) -> double
    {
        const auto dx = a.X() - b.X();
        const auto dy = a.Y() - b.Y();
        const auto dz = a.Z() - b.Z();
        return dx * dx + dy * dy + dz * dz;
    }

    auto find_player_controller() -> UObject*
    {
        std::vector<UObject*> player_controllers{};
        UObjectGlobals::FindAllOf(STR("PlayerController"), player_controllers);
        for (auto* player_controller : player_controllers)
        {
            if (!is_usable(player_controller)) { continue; }
            if (read_object(player_controller, STR("Pawn")) || read_object(player_controller, STR("AcknowledgedPawn")))
            {
                return player_controller;
            }
        }

        return nullptr;
    }

    auto find_player_actor() -> UObject*
    {
        if (auto* player_controller = find_player_controller())
        {
            if (auto* pawn = read_object(player_controller, STR("Pawn"))) { return pawn; }
            if (auto* pawn = read_object(player_controller, STR("AcknowledgedPawn"))) { return pawn; }
        }

        for (const auto* class_name : {STR("PlayerCharacterBP_C"), STR("GothicPlayerCharacter")})
        {
            std::vector<UObject*> players{};
            UObjectGlobals::FindAllOf(class_name, players);
            for (auto* player : players)
            {
                if (is_usable(player) && read_actor_location(player).found) { return player; }
            }
        }

        return nullptr;
    }

    auto find_ability_system(UObject* ability, UObject* root_task) -> UObject*
    {
        if (auto* asc = read_object(ability, STR("AbilitySystemComponent"))) { return asc; }
        if (auto* asc = read_object(root_task, STR("AbilitySystemComponent"))) { return asc; }
        if (auto* asc = read_object(root_task, STR("ASC"))) { return asc; }
        return nullptr;
    }

    auto find_avatar(UObject* ability, UObject* root_task) -> UObject*
    {
        if (auto* avatar = read_object(ability, STR("AvatarActor"))) { return avatar; }
        if (auto* avatar = read_object(root_task, STR("AvatarActor"))) { return avatar; }
        if (auto* asc = find_ability_system(ability, root_task)) { return read_object(asc, STR("AvatarActor")); }
        return nullptr;
    }

    auto find_owner(UObject* ability, UObject* root_task) -> UObject*
    {
        if (auto* owner = read_object(ability, STR("OwnerActor"))) { return owner; }
        if (auto* owner = read_object(root_task, STR("OwnerActor"))) { return owner; }
        if (auto* asc = find_ability_system(ability, root_task)) { return read_object(asc, STR("OwnerActor")); }
        return nullptr;
    }

    auto find_candidate_actor(UObject* avatar, UObject* owner) -> UObject*
    {
        if (read_actor_location(avatar).found) { return avatar; }
        if (read_actor_location(owner).found) { return owner; }
        return avatar ? avatar : owner;
    }

    auto matches_target(
        const StringType& owner_name,
        const StringType& avatar_name,
        const StringType* target_owner_name,
        const StringType* target_avatar_name) -> bool
    {
        const auto has_owner_target = target_owner_name && !target_owner_name->empty();
        const auto has_avatar_target = target_avatar_name && !target_avatar_name->empty();
        if (!has_owner_target && !has_avatar_target) { return true; }

        return (has_owner_target && owner_name == *target_owner_name) ||
               (has_avatar_target && avatar_name == *target_avatar_name);
    }

    auto call_object_return(UObject* object, const TCHAR* function_name) -> UObject*
    {
        if (!is_usable(object)) { return nullptr; }

        auto* function = object->GetFunctionByNameInChain(function_name);
        if (!function) { return nullptr; }

        struct Params
        {
            UObject* ReturnValue{};
        } params{};

        object->ProcessEvent(function, &params);
        return is_usable(params.ReturnValue) ? params.ReturnValue : nullptr;
    }

    auto call_no_params(UObject* object, const TCHAR* function_name) -> bool
    {
        if (!is_usable(object)) { return false; }

        auto* function = object->GetFunctionByNameInChain(function_name);
        if (!function) { return false; }

        struct Params
        {
        } params{};

        object->ProcessEvent(function, &params);
        return true;
    }

    auto call_no_params_safely(UObject* object, const TCHAR* function_name, const TCHAR* context) -> bool
    {
        try
        {
            return call_no_params(object, function_name);
        }
        catch (const std::exception& exception)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {} {} recovered from exception: {}.\n"),
                context,
                function_name,
                ensure_str(exception.what()));
        }
        catch (...)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {} {} recovered from unknown exception.\n"),
                context,
                function_name);
        }

        return false;
    }

    // Run a callback under a C++ try/catch so one throwing phase neither kills
    // the others nor escapes anonymously. Free function: every component uses it.
    template <typename Callback>
    auto run_guarded(const TCHAR* phase, Callback&& callback) -> bool
    {
        try
        {
            callback();
            return true;
        }
        catch (const std::exception& exception)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {} phase recovered from exception: {}.\n"),
                phase,
                ensure_str(exception.what()));
        }
        catch (...)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {} phase recovered from unknown exception.\n"),
                phase);
        }

        return false;
    }
}

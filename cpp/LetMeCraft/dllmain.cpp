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
#include <Unreal/Core/Containers/Map.hpp>
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

namespace
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
    // ~1m past the usable face - v0.7.10). v0.8.7: the min gate is gone - the
    // mod now auto-walks the player to kApproachStopDistance, so a repeat E
    // pressed while standing point-blank at the station must not be rejected.
    // The old reason for a floor (the look-based query going blind at close
    // range) is covered by the live sensor + the approach itself.
    constexpr double kMinStationDistance = 0.0;
    constexpr double kMinStationDistanceSquared = kMinStationDistance * kMinStationDistance;
    constexpr double kMaxStationDistance = 400.0;
    constexpr double kMaxStationDistanceSquared = kMaxStationDistance * kMaxStationDistance;
    // Every station's UInteractiveComponent reports interactDist=250 (v0.8.6
    // diag) - the player simply cannot be sensed from beyond 2.5m, which was
    // the remaining failure mode (both v0.8.6 misses started at 3.0-3.2m).
    // The mod walks the player in to 2.0m: inside the radius with 0.5m of
    // margin, and far enough to feel natural (1.4m and 1.6m both read as
    // "pressed against the station" to the user).
    constexpr double kApproachStopDistance = 200.0;
    constexpr double kApproachStopDistanceSquared = kApproachStopDistance * kApproachStopDistance;
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
    // Fallback when the player-claim fails its requirement checks (e.g. "Character has
    // no GASAbility for this action"): a cooldown written straight into the live
    // FInteractionSpot record suppresses routine re-claims without any user checks.
    constexpr float kSpotCooldownSeconds = 75.0f;
    constexpr int32_t kSpotCooldownDurationOffset = 0xB8;
    constexpr int32_t kSpotLastTimeUsedOffset = 0xBC;
    constexpr int32_t kInteractionSpotValueSize = 0x118;

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
        StringType source{};
        alignas(8) unsigned char claim_token[16]{};
        float saved_cooldown_duration{};
        float saved_last_time_used{};
        int claim_attempts{};
        int player_use_attempts{};
        bool spot_known{};
        bool move_issued{};
        bool has_claim{};
        bool claim_gave_up{};
        bool has_cooldown_write{};
        bool player_use_done{};
        // Set ONLY on a successful take (player_use_done alone also marks a
        // closed/failed window). Once the player runs the station the routine
        // cannot reclaim it, so the kill guard stops cancelling - post-success
        // GAS churn on the NPC ability is crash exposure for zero benefit
        // (v0.8.11: the game died in its own code right after such a take).
        bool player_took_station{};
        bool npc_interaction_disabled{};
        // One log line each for "approach started" / "approach done" - the
        // drive itself runs every engine tick and must not spam.
        bool approach_started{};
        bool approach_done{};
        // Effective approach stop for THIS eviction: starts at the 2.0m
        // default and shrinks while the player's sensor prefers a neighboring
        // interactive over the target station (the activation gate).
        double approach_stop_distance{kApproachStopDistance};
        // Hot-loop log diet (v0.8.11): the 10Hz combo force-release and the 1Hz
        // behind-hold re-issue logged every occurrence - dozens of synchronous
        // file+console writes per second on top of the object-array sweeps.
        bool combo_release_logged{};
        int behind_hold_reissues{};
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

    auto same_object(UObject* lhs, UObject* rhs) -> bool
    {
        return is_usable(lhs) && is_usable(rhs) && lhs == rhs;
    }

    auto weak_get(const FWeakObjectPtr& weak) -> UObject*
    {
        auto* object = weak.Get();
        return is_usable(object) ? object : nullptr;
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

    auto fname_text(const NameRead& name) -> StringType
    {
        if (!name.found) { return STR("<none>"); }

        auto value = name.value;
        return value.ToString();
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

    // Releases an interaction-spot claim token through the BlueprintFunctionLibrary CDO
    // (is_usable() rejects CDOs by design, hence the dedicated path).
    auto call_token_library_unclaim(unsigned char (&token)[16], const TCHAR* context) -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/G1R.InteractionSpotTokenLibrary:Unclaim"));
        auto* library = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/G1R.Default__InteractionSpotTokenLibrary"));
        if (!function || !library)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: token library unavailable (function={} cdo={}).\n"),
                context,
                function ? STR("ok") : STR("missing"),
                library ? STR("ok") : STR("missing"));
            return false;
        }

        auto call = begin_call_with_function(library, function, context);
        set_param(call, STR("Handle"), token, 16, context);
        if (!invoke_call(call, context)) { return false; }

        get_param(call, STR("Handle"), token, 16, context);
        return true;
    }

    // AAIController::MoveToLocation — the NPC walks to the destination on its own.
    auto issue_move_to_location(
        UObject* controller, const FVector& destination, const TCHAR* context, bool quiet = false) -> bool
    {
        if (!quiet)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} MoveToLocation preparing controller={}.\n"),
                context,
                object_name(controller));
        }

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
        if (!invoke_call(call, context)) { return false; }

        unsigned char request_result{};
        get_param(call, STR("ReturnValue"), &request_result, 1, context);
        if (!quiet)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} MoveToLocation result={} dest=({}, {}, {}).\n"),
                context,
                static_cast<int>(request_result),
                dest.X(),
                dest.Y(),
                dest.Z());
        }
        return true;
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

    auto post_check_delay_for_index(int index) -> Clock::duration
    {
        switch (index)
        {
        case 0:
            return std::chrono::seconds{2};
        case 1:
            return std::chrono::seconds{15};
        default:
            return std::chrono::seconds{120};
        }
    }

    auto post_check_label_for_index(int index) -> const TCHAR*
    {
        switch (index)
        {
        case 0:
            return STR("post-check 2s");
        case 1:
            return STR("post-check 15s");
        default:
            return STR("post-check 120s");
        }
    }

}

class LetMeCraft : public CppUserModBase
{
public:
    LetMeCraft() : CppUserModBase()
    {
        ModName = STR("LetMeCraft");
        ModVersion = STR("0.8.18");
        ModDescription = STR("Moves the nearest NPC away from an occupied crafting station");
        ModAuthors = STR("Roma + Codex");
    }

    ~LetMeCraft() override
    {
        for (const auto id : {m_engine_tick_callback_id, m_load_map_callback_id, m_init_game_state_callback_id})
        {
            if (id != Unreal::Hook::ERROR_ID) { Unreal::Hook::UnregisterCallback(id); }
        }
    }

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] loaded. Press E or gamepad Y near an occupied crafting NPC.\n"));
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] v0.8.18: mod renamed MoveNPCsCxx -> LetMeCraft (game Mods folder, mods.txt entry, log prefix, build target); no behavior changes vs v0.8.17 (E range 4.0m, routine blocker tag).\n"));

        register_keydown_event(Input::Key::E, [this]() {
            queue_manual_request(STR("keyboard E"));
        });

        m_engine_tick_callback_id = Unreal::Hook::RegisterEngineTickPreCallback(
            [this](auto&, UEngine*, float, bool) {
                on_engine_tick_game_thread();
            },
            {false, false, STR("LetMeCraft"), STR("GameThreadTick")});

        // Holds and pending displacements must not survive a map change or save-game
        // load: the actors they reference belong to the old world.
        m_load_map_callback_id = Unreal::Hook::RegisterLoadMapPreCallback(
            [this](auto&, auto&&...) {
                clear_transient_state(STR("LoadMap"));
            },
            {false, true, STR("LetMeCraft"), STR("ClearStateOnLoadMap")});

        m_init_game_state_callback_id = Unreal::Hook::RegisterInitGameStatePostCallback(
            [this](auto&, auto&&...) {
                clear_transient_state(STR("InitGameState"));
            },
            {false, true, STR("LetMeCraft"), STR("ClearStateOnInitGameState")});
    }

    auto on_update() -> void override
    {
        poll_gamepad_y();
    }

private:
    auto queue_manual_request(const TCHAR* source) -> void
    {
        std::lock_guard lock{m_pending_request_mutex};
        m_has_pending_manual_request = true;
        m_pending_manual_source = source;
    }

    auto take_manual_request(StringType& source) -> bool
    {
        std::lock_guard lock{m_pending_request_mutex};
        if (!m_has_pending_manual_request) { return false; }

        source = m_pending_manual_source;
        m_pending_manual_source.clear();
        m_has_pending_manual_request = false;
        return true;
    }

    auto clear_transient_state(const TCHAR* reason) -> void
    {
        const auto eviction_count = m_evictions.size();
        const auto had_post_check = m_post_check_active;

        // Claim tokens are dropped without Unclaim: the world that owned the claims is
        // being torn down together with the spots themselves.
        unlock_player_movement(reason);
        unfreeze_player_sensor(reason);
        // The old world's controller and sensor die with the map and the new ones
        // spawn with default state: a failed unlock/unfreeze must not keep retrying
        // across worlds.
        m_move_input_locked = false;
        m_move_unlock_attempts = 0;
        m_sensor_frozen = false;
        m_sensor_pinned = false;
        // The function caches stay (permanent /Script objects), but the retry budget
        // restarts so a new world gets fresh warm-up attempts immediately.
        m_warmup_attempts = 0;
        m_next_warmup_at = {};
        // v0.8.16: best-effort tag removal before the evictions are dropped -
        // on LoadMap-pre the old ASCs are still alive; on InitGameState-post
        // weak_get resolves to null and this is a no-op. Also covers the SEH
        // recovery path (recover_after_crash calls this).
        for (auto& eviction : m_evictions)
        {
            run_guarded(STR("routine blocker clear"), [&] {
                remove_routine_blocker(eviction, reason);
            });
        }
        m_evictions.clear();
        m_cached_subsystem = static_cast<UObject*>(nullptr);
        m_cached_player_controller = static_cast<UObject*>(nullptr);
        m_cached_player_state = static_cast<UObject*>(nullptr);
        m_cached_player_sensor = static_cast<UObject*>(nullptr);
        m_cached_player_interact_ability = static_cast<UObject*>(nullptr);
        m_post_check_active = false;
        m_post_check_owner_name.clear();
        m_post_check_avatar_name.clear();
        m_post_check_started_at = {};
        m_post_check_index = 0;

        {
            std::lock_guard lock{m_pending_request_mutex};
            m_has_pending_manual_request = false;
            m_pending_manual_source.clear();
        }

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] clear_transient_state reason={} droppedEvictions={} droppedPostCheck={}.\n"),
            reason,
            eviction_count,
            had_post_check ? STR("true") : STR("false"));
    }

    auto on_engine_tick_game_thread() -> void
    {
        SehCrashInfo crash{};
        if (invoke_with_seh([this] { tick_game_thread_impl(); }, crash)) { return; }

        Output::send<LogLevel::Error>(
            STR("[LetMeCraft] engine tick caught SEH fault code=0x{:X} ip=0x{:X} access=0x{:X}; aborting all evictions.\n"),
            crash.code,
            reinterpret_cast<uintptr_t>(crash.instruction_address),
            static_cast<uintptr_t>(crash.fault_address));

        SehCrashInfo recovery_crash{};
        if (!invoke_with_seh([this] { recover_after_crash(); }, recovery_crash))
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] crash recovery itself faulted code=0x{:X}; remaining state clears on next map load.\n"),
                recovery_crash.code);
        }
    }

    // Best-effort rollback after a hardware fault aborted the tick mid-flight:
    // a crashed eviction must not leave its NPC permanently non-interactive or
    // the player locked in place. Every step is guarded independently.
    auto recover_after_crash() -> void
    {
        for (auto& eviction : m_evictions)
        {
            if (!eviction.npc_interaction_disabled) { continue; }
            run_guarded(STR("crash recovery npc re-enable"), [&] { set_npc_interaction_enabled(eviction, true); });
        }
        run_guarded(STR("crash recovery clear"), [&] { clear_transient_state(STR("SEH fault")); });
    }

    auto tick_game_thread_impl() -> void
    {
        try
        {
            StringType source{};
            if (take_manual_request(source))
            {
                request_end_nearest_crafter_once(source.c_str());
            }

            tick_evictions();
            tick_post_check();

            // Hard failsafe: the movement lock can never outlive its time budget.
            if (m_move_input_locked && Clock::now() >= m_move_unlock_at)
            {
                unlock_player_movement(STR("timeout"));
            }

            // Sensor failsafe: sensing must never stay frozen without a live bridge.
            if (m_sensor_frozen && m_evictions.empty())
            {
                unfreeze_player_sensor(STR("failsafe"));
            }

            warm_up_controller_functions();
        }
        catch (const std::exception& exception)
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] engine tick recovered from exception: {}.\n"),
                ensure_str(exception.what()));
        }
        catch (...)
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] engine tick recovered from unknown exception.\n"));
        }
    }

    auto request_end_nearest_crafter_once(const TCHAR* source) -> void
    {
        if (!Unreal::IsInGameThreadRaw())
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] {} request ignored: not running on game thread.\n"),
                source);
            return;
        }

        // bShowMouseCursor is the cheapest available "UI is open" signal; whether the
        // Gothic menus actually set it is checklist item T9 of the test protocol.
        if (auto* player_controller = find_player_controller_cached())
        {
            const auto cursor = read_bool(player_controller, STR("bShowMouseCursor"));
            if (cursor.found && cursor.value)
            {
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] {} request ignored: UI cursor is visible.\n"),
                    source);
                return;
            }
        }

        const auto now = Clock::now();
        if (now < m_next_manual_request_time)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} request ignored by cooldown.\n"),
                source);
            return;
        }

        m_next_manual_request_time = now + kManualRequestCooldown;

        log_track_b_recon(source);

        const auto result = request_end_matching_crafter(source, true);
        if (result.called)
        {
            // Separate guards: a throw inside start_eviction (the v0.7.4 FuncMap bug
            // skipped the move lock AND the post-check) must not cancel the rest.
            run_guarded(STR("start eviction"), [&] { start_eviction(result.candidate, source, now); });
            run_guarded(STR("schedule post-check"), [&] { schedule_post_check(result.candidate, now); });
            return;
        }

        if (auto* eviction = find_nearby_eviction())
        {
            run_guarded(STR("refresh eviction"), [&] { refresh_eviction(*eviction, source, now); });
        }
    }

    // Track-B recon: log the stable objects v0.7.0 will call into
    // (UInteractionSubsystem::ClaimSpot, player's AGothicPlayerState).
    auto log_track_b_recon(const TCHAR* source) -> void
    {
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] {} recon interactionSubsystem={} playerState={}.\n"),
            source,
            object_name(find_interaction_subsystem()),
            object_name(find_player_state()));
    }

    // FindAllOf walks the entire object array (expensive); the stable singletons are
    // cached in weak pointers and only re-discovered when they actually die.
    auto find_interaction_subsystem() -> UObject*
    {
        if (auto* cached = weak_get(m_cached_subsystem)) { return cached; }

        std::vector<UObject*> subsystems{};
        UObjectGlobals::FindAllOf(STR("InteractionSubsystem"), subsystems);
        for (auto* subsystem : subsystems)
        {
            if (is_usable(subsystem))
            {
                m_cached_subsystem = subsystem;
                return subsystem;
            }
        }
        return nullptr;
    }

    auto find_player_actor_cached() -> UObject*
    {
        if (auto* player_controller = find_player_controller_cached())
        {
            if (auto* pawn = read_object(player_controller, STR("Pawn"))) { return pawn; }
            if (auto* pawn = read_object(player_controller, STR("AcknowledgedPawn"))) { return pawn; }
        }
        return find_player_actor();
    }

    auto find_player_controller_cached() -> UObject*
    {
        if (auto* cached = weak_get(m_cached_player_controller)) { return cached; }

        if (auto* player_controller = find_player_controller())
        {
            m_cached_player_controller = player_controller;
            return player_controller;
        }
        return nullptr;
    }

    auto find_player_state() -> UObject*
    {
        if (auto* cached = weak_get(m_cached_player_state)) { return cached; }

        if (auto* player_controller = find_player_controller_cached())
        {
            if (auto* player_state = read_object(player_controller, STR("PlayerState")))
            {
                m_cached_player_state = player_state;
                return player_state;
            }
        }
        return nullptr;
    }

    // The player's UInteractSensor decides what the interact key would target; its
    // m_CurrentInteractionActor gates the handoff and the auto-use (otherwise the
    // interact ability fires at whatever is focused - e.g. starts a dialogue with
    // the evicted NPC).
    auto find_player_sensor() -> UObject*
    {
        if (auto* cached = weak_get(m_cached_player_sensor)) { return cached; }

        auto* pawn = find_player_actor_cached();
        if (!pawn) { return nullptr; }

        std::vector<UObject*> sensors{};
        UObjectGlobals::FindAllOf(STR("InteractSensor"), sensors);
        for (auto* sensor : sensors)
        {
            if (!is_usable(sensor)) { continue; }
            for (auto* outer = sensor->GetOuterPrivate(); outer; outer = outer->GetOuterPrivate())
            {
                if (outer == pawn)
                {
                    m_cached_player_sensor = sensor;
                    return sensor;
                }
            }
        }
        return nullptr;
    }

    // The sensor retargets m_CurrentInteractionActor on its own sensing ticks, so a
    // single write is not enough: in the v0.7.5 test the activated interaction kept
    // picking the nearest interactive instead - the evicted NPC (dialogue) or a
    // closer station. Freezing the sensing for the player-use window pins the target.
    // Unfreeze is triple-failsafed: eviction end, map change, engine-tick fallback.
    auto freeze_player_sensor() -> void
    {
        if (m_sensor_frozen) { return; }
        auto* sensor = find_player_sensor();
        if (!sensor) { return; }

        auto call = begin_call(sensor, STR("SetSensingUpdatesEnabled"), STR("sensor freeze"));
        set_bool_param(call, STR("bEnabled"), false, STR("sensor freeze"));
        if (!invoke_call(call, STR("sensor freeze"))) { return; }

        m_sensor_frozen = true;
        Output::send<LogLevel::Verbose>(STR("[LetMeCraft] sensor sensing frozen.\n"));
    }

    auto unfreeze_player_sensor(const TCHAR* reason) -> void
    {
        // v0.8.6: sensing is no longer frozen for the take window, but the
        // m_FilterActor pin written per attempt still needs clearing when the
        // window closes - that cleanup used to live behind the frozen flag.
        if (m_sensor_pinned)
        {
            run_guarded(STR("sensor unpin"), [&] {
                if (auto* sensor = find_player_sensor())
                {
                    write_object_property(sensor, STR("m_FilterActor"), nullptr);
                }
            });
            m_sensor_pinned = false;
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] sensor filter unpinned reason={}.\n"), reason);
        }

        if (!m_sensor_frozen) { return; }

        // Call first, clear the flag only on success: the engine-tick failsafe keeps
        // retrying while the flag is set, so sensing can never stay disabled.
        auto unfrozen = false;
        run_guarded(STR("sensor unfreeze"), [&] {
            auto* sensor = find_player_sensor();
            write_object_property(sensor, STR("m_FilterActor"), nullptr);
            auto call = begin_call(sensor, STR("SetSensingUpdatesEnabled"), STR("sensor unfreeze"));
            set_bool_param(call, STR("bEnabled"), true, STR("sensor unfreeze"));
            unfrozen = invoke_call(call, STR("sensor unfreeze"));
        });
        if (!unfrozen)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] sensor unfreeze failed reason={}, will retry.\n"),
                reason);
            return;
        }

        m_sensor_frozen = false;
        Output::send<LogLevel::Verbose>(STR("[LetMeCraft] sensor sensing unfrozen reason={}.\n"), reason);
    }

    auto is_spot_unclaimed(const FName& spot) -> bool
    {
        auto* subsystem = find_interaction_subsystem();
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/G1R.InteractionSpotHandleLibrary:IsUnclaimed"));
        auto* library = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/G1R.Default__InteractionSpotHandleLibrary"));
        if (!subsystem || !function || !library) { return false; }

        auto call = begin_call_with_function(library, function, STR("spot free check"));
        auto handle = spot;
        set_param(call, STR("Handle"), &handle, 8, STR("spot free check"));
        set_param(call, STR("WorldContextObject"), &subsystem, 8, STR("spot free check"));
        if (!invoke_call(call, STR("spot free check"))) { return false; }

        return get_bool_param(call, STR("ReturnValue"), STR("spot free check"));
    }

    // The player presses E to take the station: their movement is locked for a few
    // seconds while the NPC steps aside, then the interaction starts automatically.
    // ResetIgnoreMoveInput clears the whole ignore-counter, so the unlock can never
    // leave the player stuck.
    // Resolve a UFunction once and keep it in a weak cache (/Script function objects
    // live for the whole session). Tries the full-path lookup first, then the
    // FuncMap-independent children-walk: on this build the PlayerController chain is
    // exactly where both the name-based FuncMap walk (v0.7.4) and the path lookup
    // (v0.7.5) failed, so the resolved route is logged for the test protocol.
    auto find_cached_function(
        FWeakObjectPtr& cache,
        UObject* chain_object,
        const TCHAR* function_path,
        const TCHAR* function_name,
        const TCHAR* context) -> UFunction*
    {
        if (auto* cached = weak_get(cache)) { return static_cast<UFunction*>(cached); }

        // Children-walk FIRST: it only touches the target's class chain and is the
        // only route that has ever resolved the controller functions in-game
        // (v0.7.7). The global StaticFindObject scan can throw for MINUTES straight
        // (v0.7.9 log) - each route runs under its own guard so one throwing route
        // cannot eat the other.
        UFunction* function = nullptr;
        const TCHAR* route = STR("children-walk");
        try
        {
            function = find_function_via_children(chain_object, function_name);
        }
        catch (...)
        {
        }
        if (!function)
        {
            route = STR("path");
            try
            {
                function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, function_path);
            }
            catch (...)
            {
            }
        }
        // No warning here: the warm-up retries on a schedule and does its own
        // throttled failure logging.
        if (!function) { return nullptr; }

        cache = static_cast<UObject*>(function);
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] {}: {} resolved via {}.\n"),
            context,
            function_name,
            route);
        return function;
    }

    // Both move-lock UFunctions are resolved ONCE on a quiet engine tick: any object
    // lookup (StaticFindObject's GUObjectArray scan included) throws "Array failed
    // invariants check" when it runs in the SAME tick as OnRequestEndQuick, tripping
    // over the half-destroyed GAS objects (proven by v0.7.4-0.7.6 logs: the identical
    // lookups succeed on every later tick). /Script functions are permanent, so the
    // warm-up happens once per session and lock/unlock then run cache-only.
    auto warm_up_controller_functions() -> void
    {
        if (m_controller_functions_warmed) { return; }

        const auto now = Clock::now();
        if (now < m_next_warmup_at) { return; }
        m_next_warmup_at = now + kWarmupRetryInterval;

        // The path lookup needs no controller; only the children-walk fallback does.
        auto* player_controller = find_player_controller_cached();

        // Without a controller the children-walk has nothing to walk; do not burn
        // a logged attempt on the menu world.
        if (!player_controller) { return; }

        ++m_warmup_attempts;
        run_guarded(STR("controller function warm-up"), [&] {
            const auto* set_function = find_cached_function(
                m_set_ignore_move_input_function,
                player_controller,
                STR("/Script/Engine.PlayerController:SetIgnoreMoveInput"),
                STR("SetIgnoreMoveInput"),
                STR("move lock warm-up"));
            const auto* reset_function = find_cached_function(
                m_reset_ignore_move_input_function,
                player_controller,
                STR("/Script/Engine.PlayerController:ResetIgnoreMoveInput"),
                STR("ResetIgnoreMoveInput"),
                STR("move unlock warm-up"));
            m_controller_functions_warmed = set_function && reset_function;
        });

        if (!m_controller_functions_warmed && (m_warmup_attempts == 1 || m_warmup_attempts % 20 == 0))
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] controller function warm-up attempt {} unresolved - retrying every {}ms.\n"),
                m_warmup_attempts,
                std::chrono::duration_cast<std::chrono::milliseconds>(kWarmupRetryInterval).count());
        }
    }

    // SetIgnoreMoveInput only suppresses the movement AXIS - jumping is an action
    // input and stays live (v0.7.13 user report: the player can jump while
    // move-locked). ACharacter::JumpMaxCount=0 makes CanJump fail; the previous
    // value is restored on unlock. Property chain walks are safe in any tick.
    auto set_player_jump_enabled(bool enabled) -> void
    {
        auto* player = find_player_actor_cached();
        if (!player) { return; }
        auto* property = CastField<FIntProperty>(player->GetPropertyByNameInChain(STR("JumpMaxCount")));
        if (!property)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] jump toggle skipped: JumpMaxCount not found on player.\n"));
            return;
        }
        if (!enabled)
        {
            const auto current = property->GetPropertyValueInContainer(player);
            if (current > 0) { m_saved_jump_max_count = current; }
            property->SetPropertyValueInContainer(player, 0);
        }
        else
        {
            property->SetPropertyValueInContainer(
                player, m_saved_jump_max_count > 0 ? m_saved_jump_max_count : 1);
        }
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] player jump {}.\n"), enabled ? STR("re-enabled") : STR("blocked"));
    }

    auto lock_player_movement() -> void
    {
        auto* player_controller = find_player_controller_cached();
        if (!player_controller) { return; }

        // Cache-only on purpose: no object lookups may run in the cancel tick.
        auto* function = static_cast<UFunction*>(weak_get(m_set_ignore_move_input_function));
        if (!function)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] move lock skipped: SetIgnoreMoveInput not warmed up yet.\n"));
            return;
        }

        auto call = begin_call_with_function(player_controller, function, STR("move lock"));
        set_bool_param(call, STR("bNewMoveInput"), true, STR("move lock"));
        if (!invoke_call(call, STR("move lock"))) { return; }

        m_move_input_locked = true;
        m_move_unlock_attempts = 0;
        m_move_unlock_at = Clock::now() + kMoveLockDuration;
        run_guarded(STR("jump block"), [&] { set_player_jump_enabled(false); });
        Output::send<LogLevel::Verbose>(STR("[LetMeCraft] player movement locked (max {}s).\n"),
            std::chrono::duration_cast<std::chrono::seconds>(kMoveLockDuration).count());
    }

    auto unlock_player_movement(const TCHAR* reason) -> void
    {
        if (!m_move_input_locked) { return; }

        // Call first, clear the flag only on success: a silently failed
        // ResetIgnoreMoveInput must never strand the player move-locked.
        auto unlocked = false;
        run_guarded(STR("move unlock"), [&] {
            auto* player_controller = find_player_controller_cached();
            // Cache-only, same as the lock: filled by the warm-up tick.
            auto* function = static_cast<UFunction*>(weak_get(m_reset_ignore_move_input_function));
            if (!player_controller || !function) { return; }

            auto call = begin_call_with_function(player_controller, function, STR("move unlock"));
            unlocked = invoke_call(call, STR("move unlock"));
        });

        if (!unlocked)
        {
            ++m_move_unlock_attempts;
            if (m_move_unlock_attempts < kMoveUnlockMaxAttempts)
            {
                // Re-arm the engine-tick failsafe as the retry driver.
                m_move_unlock_at = Clock::now() + kMoveUnlockRetryInterval;
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] move unlock failed reason={} attempt={}, retrying.\n"),
                    reason,
                    m_move_unlock_attempts);
                return;
            }
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] move unlock abandoned after {} attempts reason={}.\n"),
                m_move_unlock_attempts,
                reason);
        }

        m_move_input_locked = false;
        m_move_unlock_attempts = 0;
        run_guarded(STR("jump unblock"), [&] { set_player_jump_enabled(true); });
        Output::send<LogLevel::Verbose>(STR("[LetMeCraft] player movement unlocked reason={}.\n"), reason);
    }

    auto find_nearby_eviction() -> ActiveEviction*
    {
        auto* player = find_player_actor_cached();
        const auto player_location = read_actor_location(player);
        if (!player_location.found) { return nullptr; }

        ActiveEviction* best{};
        auto best_distance_squared = std::numeric_limits<double>::max();
        for (auto& eviction : m_evictions)
        {
            auto* actor = weak_get(eviction.avatar);
            if (!actor) { continue; }

            const auto actor_location = read_actor_location(actor);
            if (!actor_location.found) { continue; }

            const auto current_distance_squared = distance_squared(player_location.value, actor_location.value);
            if (current_distance_squared > kHeldFallbackDistanceSquared) { continue; }
            if (current_distance_squared >= best_distance_squared) { continue; }

            best = &eviction;
            best_distance_squared = current_distance_squared;
        }

        return best;
    }

    auto refresh_eviction(ActiveEviction& eviction, const TCHAR* source, Clock::time_point now) -> void
    {
        eviction.resume_at = now + kHoldDuration;
        // Repeat E re-arms the auto-take too: a fresh interaction window and a
        // fresh move lock, exactly like a new eviction would get. The approach
        // restarts as well - the player may have wandered off since the last
        // window, and the arrival gate must hold attempts until they are back.
        eviction.player_use_until = now + kMoveLockDuration;
        eviction.player_use_done = false;
        // Repeat E re-arms the bridge, so the kill guard must run again - and
        // the attempt counter restarts so the v0.8.12 cap measures THIS window.
        eviction.player_took_station = false;
        eviction.player_use_attempts = 0;
        eviction.approach_started = false;
        eviction.approach_done = false;
        eviction.approach_stop_distance = kApproachStopDistance;
        eviction.approach_grace_until = now + kApproachActivationGrace;
        eviction.combo_release_logged = false;
        // Keeps the 1-in-5 loud cadence aligned with the new window (the v0.8.11
        // audit: never resetting this phase-shifted the loud re-issues).
        eviction.behind_hold_reissues = 0;
        run_guarded(STR("move lock"), [&] { lock_player_movement(); });

        // v0.8.16: the tag normally survives a repeat-E (the eviction object is
        // reused) - never double-add; re-add only when it verifiably vanished.
        run_guarded(STR("routine blocker refresh"), [&] {
            if (!eviction.blocker_known) { return; }
            if (!eviction.blocker_applied)
            {
                apply_routine_blocker(eviction);
                return;
            }
            auto* asc = weak_get(eviction.asc);
            if (!asc) { return; }
            auto check = begin_call(asc, STR("HasTagExact"), STR("routine blocker refresh"));
            set_param(check, STR("GameplayTag"), &eviction.blocker_tag, 8, STR("routine blocker refresh"));
            if (invoke_call(check, STR("routine blocker refresh")) &&
                !get_bool_param(check, STR("ReturnValue"), STR("routine blocker refresh")))
            {
                eviction.blocker_applied = false;
                apply_routine_blocker(eviction);
            }
        });

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] {} eviction refreshed owner={} avatar={} spot={} holdMs={}.\n"),
            source,
            eviction.owner_name,
            eviction.avatar_name,
            fname_to_text(eviction.spot),
            std::chrono::duration_cast<std::chrono::milliseconds>(kHoldDuration).count());
    }

    auto start_eviction(const CraftingCandidate& candidate, const TCHAR* source, Clock::time_point now) -> void
    {
        // Capture everything suppression needs while the transient GAS objects are
        // still alive: spot name, action tag, station actor, AI controller.
        const auto root_spot = read_fname_at(candidate.root_task, STR("Spot"));
        const auto ability_spot = read_fname_at(candidate.ability, STR("m_InteractionSpot"));
        const auto action = read_fname_at(candidate.root_task, STR("Action"));
        auto* station = read_object(candidate.ability, STR("m_InteractiveActor"));
        auto* controller = find_ai_controller_from_candidate(candidate);

        for (auto& eviction : m_evictions)
        {
            if (!eviction.owner_name.empty() && eviction.owner_name == candidate.owner_name)
            {
                refresh_eviction(eviction, source, now);
                return;
            }
        }

        ActiveEviction eviction{};
        eviction.move_at = now + kRetreatDelay;
        eviction.stand_still_from = now + kStandStillDelay;
        eviction.next_enforce_at = now + kStandStillDelay;
        eviction.next_claim_at = now + kClaimRetryInterval;
        eviction.player_use_at = now + kPlayerUseDelay;
        eviction.approach_grace_until = now + kApproachActivationGrace;
        eviction.player_use_until = now + kMoveLockDuration;
        eviction.resume_at = now + kHoldDuration;
        eviction.owner = candidate.owner;
        eviction.avatar = candidate.avatar;
        eviction.controller = controller;
        eviction.station = station;
        eviction.ability = candidate.ability;
        eviction.asc = candidate.asc;
        eviction.spot = root_spot.found ? root_spot.value : ability_spot.value;
        eviction.spot_known = root_spot.found || ability_spot.found;
        eviction.action = action.value;
        eviction.owner_name = candidate.owner_name;
        eviction.avatar_name = candidate.avatar_name;
        eviction.source = source;
        run_guarded(STR("routine blocker select"), [&] { select_routine_blocker(eviction, candidate); });
        m_evictions.push_back(eviction);

        // Free the spot right away: in v0.8.0 the first force-release came only
        // with the claim phase (~2s in), so the station stayed locked by the
        // NPC's claim for the first seconds of every eviction.
        run_guarded(STR("initial spot release"), [&] {
            if (auto* subsystem = find_interaction_subsystem())
            {
                force_release_owner_claims(subsystem, m_evictions.back(), STR("eviction start"));
            }
        });

        // v0.8.16: block re-activation at the source - the routine restarts its
        // interaction task within the SAME world tick as our force-release,
        // which the per-tick claim poll can never win (the v0.8.15 35-cancel
        // wars on Kharim/Snaf).
        run_guarded(STR("routine blocker apply"), [&] { apply_routine_blocker(m_evictions.back()); });

        run_guarded(STR("move lock"), [&] { lock_player_movement(); });

        // Snap the view at the station right away: the sensor's sensed set only
        // admits a station while it is usable AND inside the view cone, and the
        // ~1s between eviction start and the first take attempt is the window
        // where both finally hold (v0.8.5 log: component healthy, sensor null -
        // the player simply was not looking at the small station while its set
        // could still update).
        run_guarded(STR("face station early"), [&] { face_station(station); });

        run_guarded(STR("eviction started log"), [&] {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} eviction started owner={} avatar={} spot={} action={} controller={} station={} holdMs={} activeEvictions={}.\n"),
                source,
                candidate.owner_name,
                candidate.avatar_name,
                eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")},
                action.found ? fname_to_text(eviction.action) : StringType{STR("<none>")},
                object_name(controller),
                object_name(station),
                std::chrono::duration_cast<std::chrono::milliseconds>(kHoldDuration).count(),
                m_evictions.size());
        });
    }

    auto select_crafting_candidate(
        const TCHAR* source,
        const StringType* target_owner_name,
        const StringType* target_avatar_name,
        bool verbose,
        bool require_player_range = true) -> CraftingSearchResult
    {
        if (!Unreal::IsInGameThreadRaw())
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] {} request aborted: GAS scan attempted off game thread.\n"),
                source);
            return {};
        }

        std::vector<UObject*> abilities{};
        UObjectGlobals::FindAllOf(STR("GameplayAbilityInteractFreePoint"), abilities);

        int scanned{};
        int skipped_inactive{};
        int skipped_uncancelable{};
        int skipped_end_requested{};
        int skipped_no_root{};
        int skipped_non_crafting{};
        int skipped_no_actor{};
        int skipped_too_far{};
        int skipped_no_station{};
        int skipped_station_too_far{};
        int skipped_station_too_close{};
        int skipped_target_mismatch{};
        int candidate_count{};

        auto* player = find_player_actor_cached();
        const auto player_location = read_actor_location(player);
        if (verbose)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} request: scanning {} GameplayAbilityInteractFreePoint instances. player={} playerLocation={}\n"),
                source,
                abilities.size(),
                object_name(player),
                player_location.found ? STR("true") : STR("false"));
        }

        CraftingCandidate best{};

        for (auto* ability : abilities)
        {
            if (!is_usable(ability)) { continue; }
            ++scanned;

            const auto active = read_bool(ability, STR("bIsActive"));
            const auto cancelable = read_bool(ability, STR("bIsCancelable"));
            const auto end_requested = read_bool(ability, STR("bEndRequested"));
            auto* root_task = read_object(ability, STR("RootInteractionTask"));
            auto* asc = find_ability_system(ability, root_task);
            auto* avatar = find_avatar(ability, root_task);
            auto* owner = find_owner(ability, root_task);
            const auto owner_name = object_name(owner);
            const auto avatar_name = object_name(avatar);

            if (!active.found || !active.value)
            {
                ++skipped_inactive;
                continue;
            }

            if (!cancelable.found || !cancelable.value)
            {
                ++skipped_uncancelable;
                continue;
            }

            if (end_requested.found && end_requested.value)
            {
                ++skipped_end_requested;
                continue;
            }

            if (!root_task)
            {
                ++skipped_no_root;
                continue;
            }

            if (!root_task_is_crafting(root_task))
            {
                ++skipped_non_crafting;
                continue;
            }

            if (!matches_target(owner_name, avatar_name, target_owner_name, target_avatar_name))
            {
                ++skipped_target_mismatch;
                continue;
            }

            auto* candidate_actor = find_candidate_actor(avatar, owner);
            auto candidate_location = read_actor_location(candidate_actor);
            if (!candidate_location.found)
            {
                ++skipped_no_actor;
                continue;
            }

            auto current_distance_squared = 0.0;
            if (player_location.found)
            {
                current_distance_squared = distance_squared(player_location.value, candidate_location.value);
                if (require_player_range && current_distance_squared > kMaxTargetDistanceSquared)
                {
                    ++skipped_too_far;
                    continue;
                }
            }

            // The 1-3m station window applies to the manual trigger only; the kill
            // guard and the combo re-find the SAME eviction target regardless of
            // where the player has wandered meanwhile.
            auto* station_actor = read_object(ability, STR("m_InteractiveActor"));
            auto station_distance_squared = -1.0;
            if (player_location.found)
            {
                const auto station_location = read_actor_location(station_actor);
                if (station_location.found)
                {
                    station_distance_squared = distance_squared(player_location.value, station_location.value);
                }
            }
            if (require_player_range && player_location.found)
            {
                const TCHAR* rejection = nullptr;
                if (station_distance_squared < 0.0)
                {
                    ++skipped_no_station;
                    rejection = STR("no station location");
                }
                else if (station_distance_squared > kMaxStationDistanceSquared)
                {
                    ++skipped_station_too_far;
                    rejection = STR("station too far");
                }
                else if (station_distance_squared < kMinStationDistanceSquared)
                {
                    ++skipped_station_too_close;
                    rejection = STR("station too close");
                }
                if (rejection)
                {
                    if (verbose)
                    {
                        // The counters alone proved insufficient (v0.7.9: every E
                        // rejected with no way to tell the measured distance).
                        const auto root_spot = read_fname_at(root_task, STR("Spot"));
                        Output::send<LogLevel::Verbose>(
                            STR("[LetMeCraft] candidate rejected: {} - stationDistanceSquared={} (allowed {}..{}) spot={} station={} owner={} avatarDistanceSquared={}.\n"),
                            rejection,
                            station_distance_squared,
                            kMinStationDistanceSquared,
                            kMaxStationDistanceSquared,
                            fname_text(root_spot),
                            object_name(station_actor),
                            owner_name,
                            current_distance_squared);
                    }
                    continue;
                }
            }

            ++candidate_count;
            if (verbose)
            {
                // Track-B recon: spot + action identify the exact interaction spot the
                // NPC occupies, ownerGlobalId is the stable NPC re-lookup key. All four
                // are plain property reads.
                const auto ability_spot = read_fname_at(ability, STR("m_InteractionSpot"));
                const auto root_spot = read_fname_at(root_task, STR("Spot"));
                const auto root_action = read_fname_at(root_task, STR("Action"));
                const auto owner_global_id = read_fname_at(owner, STR("CharacterGlobalId"));

                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] candidate ability={} asc={} owner={} avatar={} root={} active={} cancelable={} endRequested={} distanceSquared={} stationDistanceSquared={} spot={} rootSpot={} action={} ownerGlobalId={}\n"),
                    object_name(ability),
                    object_name(asc),
                    owner_name,
                    avatar_name,
                    object_name(root_task),
                    active.found ? (active.value ? STR("true") : STR("false")) : STR("<missing>"),
                    cancelable.found ? (cancelable.value ? STR("true") : STR("false")) : STR("<missing>"),
                    end_requested.found ? (end_requested.value ? STR("true") : STR("false")) : STR("<missing>"),
                    current_distance_squared,
                    station_distance_squared,
                    fname_text(ability_spot),
                    fname_text(root_spot),
                    fname_text(root_action),
                    fname_text(owner_global_id));
            }

            if (!best.ability || current_distance_squared < best.distance_squared)
            {
                best = {ability, root_task, asc, owner, avatar, current_distance_squared, owner_name, avatar_name};
            }
        }

        if (!best.ability)
        {
            if (verbose)
            {
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] no matching crafting candidate: scanned={} candidates={} inactive={} uncancelable={} end_requested={} no_root={} non_crafting={} no_actor={} too_far={} no_station={} station_too_far={} station_too_close={} target_mismatch={}.\n"),
                    scanned,
                    candidate_count,
                    skipped_inactive,
                    skipped_uncancelable,
                    skipped_end_requested,
                    skipped_no_root,
                    skipped_non_crafting,
                    skipped_no_actor,
                    skipped_too_far,
                    skipped_no_station,
                    skipped_station_too_far,
                    skipped_station_too_close,
                    skipped_target_mismatch);
            }
            return {};
        }

        return {true, best};
    }

    auto call_request_end_quick(const CraftingCandidate& candidate, const TCHAR* source) -> bool
    {
        if (!is_usable(candidate.ability)) { return false; }

        auto* request_end_quick = candidate.ability->GetFunctionByNameInChain(STR("OnRequestEndQuick"));
        if (!request_end_quick)
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] OnRequestEndQuick not found on {}; no call made.\n"),
                object_name(candidate.ability));
            return false;
        }

        struct EmptyParams
        {
        } params{};

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] {} calling OnRequestEndQuick on ability={} owner={} avatar={} root={} distanceSquared={}.\n"),
            source,
            object_name(candidate.ability),
            candidate.owner_name,
            candidate.avatar_name,
            object_name(candidate.root_task),
            candidate.distance_squared);
        candidate.ability->ProcessEvent(request_end_quick, &params);
        Output::send<LogLevel::Verbose>(STR("[LetMeCraft] OnRequestEndQuick returned.\n"));
        return true;
    }

    auto request_end_matching_crafter(const TCHAR* source, bool verbose) -> RequestResult
    {
        const auto search = select_crafting_candidate(source, nullptr, nullptr, verbose);
        if (!search.found) { return {}; }

        const auto called = call_request_end_quick(search.candidate, source);
        return {called, search.candidate};
    }

    // The steady-state replacement for select_crafting_candidate inside an
    // eviction: evaluate the CACHED ability instance directly (~10 property
    // reads) instead of FindAllOf-sweeping the whole object array. The 10Hz
    // combo + 1Hz kill guard sweeps were the v0.8.10 stutter. Same per-ability
    // checks as the scan loop; found=false when the routine's ability is
    // currently idle (nothing to cancel) - exactly like a scan miss.
    auto evaluate_eviction_ability(ActiveEviction& eviction) -> CraftingSearchResult
    {
        auto* ability = weak_get(eviction.ability);
        if (!ability || !is_usable(ability)) { return {}; }

        const auto active = read_bool(ability, STR("bIsActive"));
        const auto cancelable = read_bool(ability, STR("bIsCancelable"));
        const auto end_requested = read_bool(ability, STR("bEndRequested"));
        if (!active.found || !active.value) { return {}; }
        if (!cancelable.found || !cancelable.value) { return {}; }
        if (end_requested.found && end_requested.value) { return {}; }

        auto* root_task = read_object(ability, STR("RootInteractionTask"));
        if (!root_task || !root_task_is_crafting(root_task)) { return {}; }

        auto* asc = find_ability_system(ability, root_task);
        auto* owner = find_owner(ability, root_task);
        auto* avatar = find_avatar(ability, root_task);

        auto distance = std::numeric_limits<double>::max();
        const auto player_location = read_actor_location(find_player_actor_cached());
        const auto candidate_location = read_actor_location(find_candidate_actor(avatar, owner));
        if (player_location.found && candidate_location.found)
        {
            distance = distance_squared(player_location.value, candidate_location.value);
        }

        return {true,
                {ability, root_task, asc, owner, avatar, distance,
                 eviction.owner_name, eviction.avatar_name}};
    }

    auto find_ai_ability_from_candidate(const CraftingCandidate& candidate) -> UObject*
    {
        if (auto* ai_ability = read_object(candidate.owner, STR("AIAbility"))) { return ai_ability; }
        if (auto* ai_ability = read_object(candidate.avatar, STR("AIAbility"))) { return ai_ability; }
        if (auto* ai_ability = call_object_return(candidate.owner, STR("GetAIAbility"))) { return ai_ability; }
        if (auto* ai_ability = call_object_return(candidate.avatar, STR("GetAIAbility"))) { return ai_ability; }

        for (const auto* class_name : {STR("GameplayAbility_CharacterAI"), STR("GameplayAbility_CharacterAI_Gothic")})
        {
            std::vector<UObject*> ai_abilities{};
            UObjectGlobals::FindAllOf(class_name, ai_abilities);
            for (auto* ai_ability : ai_abilities)
            {
                if (!is_usable(ai_ability)) { continue; }

                const auto owner_name = object_name(find_owner(ai_ability, nullptr));
                const auto avatar_name = object_name(find_avatar(ai_ability, nullptr));
                if (owner_name == candidate.owner_name || avatar_name == candidate.avatar_name)
                {
                    return ai_ability;
                }
            }
        }

        return nullptr;
    }

    auto find_ai_controller_from_candidate(const CraftingCandidate& candidate) -> UObject*
    {
        if (auto* controller = read_object(candidate.avatar, STR("Controller"))) { return controller; }
        if (auto* controller = read_object(candidate.owner, STR("Controller"))) { return controller; }

        auto* ai_ability = find_ai_ability_from_candidate(candidate);
        if (auto* controller = call_object_return(ai_ability, STR("GetAIController"))) { return controller; }

        return nullptr;
    }

    // Gothic AI is GAS-driven (UGameplayAbility_CharacterAI), not BehaviorTree-driven:
    // GothicAIController has no BrainComponent (v0.6.1 logs: brain=<null> on every NPC),
    // so the old StopLogic/RestartLogic path could never run and was removed.
    auto pause_ai_for_hold(UObject* controller, const TCHAR* source) -> bool
    {
        return call_no_params_safely(controller, STR("BP_PauseMove"), source);
    }

    // The activation targets the interactive nearest to the player and ignores both
    // the frozen sensor and m_FilterActor (v0.7.10-0.7.12 logs), and fighting the
    // NPC's movement lost every time (the routine overrides MoveToLocation within a
    // tick - v0.7.12: the NPC hovered at 0.7-1.7m for the whole window). So the NPC
    // itself is made non-interactive for the eviction window instead:
    // AGothicCharacter::m_InteractiveComponent (@0x9B0) exposes
    // SetForceDisableInteraction(bool) - the game's own temporary hide-from-
    // interaction switch. Deliberately NOT SetCanBeUsed: that one takes a 'save'
    // parameter and persists into savegames.
    auto set_npc_interaction_enabled(ActiveEviction& eviction, bool enabled) -> bool
    {
        auto* avatar = weak_get(eviction.avatar);
        auto* component = read_object(avatar, STR("m_InteractiveComponent"));
        if (!component)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] npc interaction toggle skipped: no m_InteractiveComponent on avatar={}.\n"),
                eviction.avatar_name);
            return false;
        }

        auto call = begin_call(component, STR("SetForceDisableInteraction"), STR("npc interaction toggle"));
        set_bool_param(call, STR("Value"), !enabled, STR("npc interaction toggle"));
        const auto called = invoke_call(call, STR("npc interaction toggle"));
        if (!called)
        {
            // Fallback belt: write the flag directly - the property exists even if
            // the function-call path fails.
            auto* property = CastField<FBoolProperty>(
                component->GetPropertyByNameInChain(STR("m_ForceDisableInteraction")));
            if (!property) { return false; }
            property->SetPropertyValueInContainer(component, !enabled);
        }

        eviction.npc_interaction_disabled = !enabled;
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] npc interaction {} avatar={} via={}.\n"),
            enabled ? STR("re-enabled") : STR("disabled"),
            eviction.avatar_name,
            called ? STR("SetForceDisableInteraction") : STR("direct flag write"));
        return true;
    }

    auto issue_retreat(ActiveEviction& eviction, bool quiet = false) -> void
    {
        auto* avatar = weak_get(eviction.avatar);
        auto* controller = weak_get(eviction.controller);
        const auto avatar_location = read_actor_location(avatar);
        if (!avatar || !controller || !avatar_location.found)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] retreat skipped owner={} avatar={} controller={}.\n"),
                eviction.owner_name,
                avatar ? STR("ok") : STR("gone"),
                controller ? STR("ok") : STR("gone"));
            return;
        }

        // Destination: behind the player's back (user redesign v0.8.0). The player
        // faces the station when pressing E, so the point on the station->player
        // ray extended kBehindPlayerDistance past the player is behind them. The
        // point is captured once per eviction and reused by every re-issue.
        if (!eviction.retreat_dest_known)
        {
            const auto player_location = read_actor_location(find_player_actor_cached());
            if (!player_location.found)
            {
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] retreat skipped: no player location owner={}.\n"),
                    eviction.owner_name);
                return;
            }

            auto dx = 0.0;
            auto dy = 0.0;
            const auto station_location = read_actor_location(weak_get(eviction.station));
            if (station_location.found)
            {
                dx = player_location.value.X() - station_location.value.X();
                dy = player_location.value.Y() - station_location.value.Y();
            }
            auto length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0)
            {
                // No station actor: walk the NPC->player direction past the player.
                dx = player_location.value.X() - avatar_location.value.X();
                dy = player_location.value.Y() - avatar_location.value.Y();
                length = std::sqrt(dx * dx + dy * dy);
            }
            if (length < 1.0)
            {
                dx = 1.0;
                dy = 0.0;
                length = 1.0;
            }

            eviction.retreat_dest = FVector{
                player_location.value.X() + (dx / length) * kBehindPlayerDistance,
                player_location.value.Y() + (dy / length) * kBehindPlayerDistance,
                player_location.value.Z()};
            eviction.retreat_dest_known = true;
        }

        issue_move_to_location(controller, eviction.retreat_dest, STR("retreat"), quiet);
    }

    auto claim_spot_once(UObject* subsystem, UObject* player_state, ActiveEviction& eviction) -> bool
    {
        auto claim = begin_call(subsystem, STR("ClaimSpot"), STR("claim"));
        set_param(claim, STR("Spot"), &eviction.spot, 8, STR("claim"));
        set_param(claim, STR("User"), &player_state, 8, STR("claim"));
        set_param(claim, STR("Action"), &eviction.action, 8, STR("claim"));
        if (!invoke_call(claim, STR("claim"))) { return false; }
        get_param(claim, STR("ReturnValue"), eviction.claim_token, 16, STR("claim"));

        auto verify = begin_call(subsystem, STR("IsSpotClaimedBy"), STR("claim verify"));
        set_param(verify, STR("Spot"), &eviction.spot, 8, STR("claim verify"));
        set_param(verify, STR("User"), &player_state, 8, STR("claim verify"));
        if (!invoke_call(verify, STR("claim verify"))) { return false; }
        if (!get_bool_param(verify, STR("ReturnValue"), STR("claim verify"))) { return false; }

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] claim ok spot={} action={} user={} owner={}.\n"),
            fname_to_text(eviction.spot),
            fname_to_text(eviction.action),
            object_name(player_state),
            eviction.owner_name);
        return true;
    }

    // HandleClaimerDestroyed drops every spot claim the NPC holds. Proven safe and
    // idempotent (releasing claims of an owner with none is a no-op), and it cannot
    // touch a claim held by the player's G1RPlayerState - so the claim phase, the
    // kill guard and the player-use combo can all call it freely. The name-based
    // begin_call is intentional: the FuncMap throw is specific to the PlayerController,
    // lookups on the InteractionSubsystem are proven working since v0.7.0.
    auto force_release_owner_claims(
        UObject* subsystem, ActiveEviction& eviction, const TCHAR* context, bool quiet = false) -> bool
    {
        auto* owner_actor = weak_get(eviction.owner);
        if (!subsystem || !owner_actor) { return false; }

        auto release = begin_call(subsystem, STR("HandleClaimerDestroyed"), context);
        set_param(release, STR("Actor"), &owner_actor, 8, context);
        if (!invoke_call(release, context)) { return false; }

        // v0.8.14: the spot keeps a SECOND ledger - active USES (UseSpot tokens),
        // separate from claims. A crafting NPC holds a use token continuously
        // across task restarts, which is why ClaimSpot kept failing with "No
        // tokens left" while the claims list read empty (the v0.8.13 Kharim
        // wars: zero spotFree=true across five full windows - no release gap
        // exists for claims alone). HandleUserDestroyed is the game's own
        // despawn-cleanup for that ledger (actors despawn mid-use routinely,
        // e.g. the observed mid-session NPC recreations), called here for both
        // identities the ledger could key on.
        auto release_owner_uses = begin_call(subsystem, STR("HandleUserDestroyed"), context);
        set_param(release_owner_uses, STR("Actor"), &owner_actor, 8, context);
        invoke_call(release_owner_uses, context);
        if (auto* avatar_actor = weak_get(eviction.avatar))
        {
            auto release_avatar_uses = begin_call(subsystem, STR("HandleUserDestroyed"), context);
            set_param(release_avatar_uses, STR("Actor"), &avatar_actor, 8, context);
            invoke_call(release_avatar_uses, context);
        }

        if (!quiet)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} force-release: dropped all spot claims+uses of owner={}.\n"),
                context,
                eviction.owner_name);
        }
        return true;
    }

    // v0.8.16: pick the routine-blocker tag and emit the one-per-eviction
    // diagnostic. Runs at eviction start while the candidate's GAS objects are
    // still alive (same-tick property READS are proven safe - start_eviction
    // already does them). The blocker exists because the routine restarts its
    // interaction task within the SAME world tick as our force-release (the
    // v0.8.15 claim wars: the per-tick poll never once saw the spot free in a
    // failed window) - no cancel/poll cadence can win an event-driven re-claim,
    // it has to be blocked at the activation source.
    auto select_routine_blocker(ActiveEviction& eviction, const CraftingCandidate& candidate) -> void
    {
        const auto activation_blocked = read_tag_container(candidate.ability, STR("ActivationBlockedTags"));
        const auto cancel_if_gains = read_tag_container(candidate.root_task, STR("CancelIfCharacterGainsAnyOf"));
        auto owned_while_active = read_tag_container(candidate.root_task, STR("OwnedTagsWhileActive"));
        if (!owned_while_active.found)
        {
            // The UAbilityTask_AI branch spells it OwnedTagsToAddWhileActive.
            owned_while_active = read_tag_container(candidate.root_task, STR("OwnedTagsToAddWhileActive"));
        }
        const auto blocked_owned = read_tag_container(candidate.root_task, STR("BlockedOwnedTags"));

        const auto chosen = choose_blocker_tag(activation_blocked);
        eviction.blocker_tag = chosen.value;
        eviction.blocker_known = chosen.found;

        if (chosen.found)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] routine blocker selected tag={} owner={} activationBlocked={} cancelIfGains={} ownedWhileActive={} blockedOwned={}.\n"),
                fname_to_text(eviction.blocker_tag),
                eviction.owner_name,
                tags_to_text(activation_blocked),
                tags_to_text(cancel_if_gains),
                tags_to_text(owned_while_active),
                tags_to_text(blocked_owned));
        }
        else
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] no safe blocker tag - running without one (v0.8.15 behavior). ActivationBlockedTags={} CancelIfCharacterGainsAnyOf={} OwnedTagsWhileActive={} BlockedOwnedTags={} owner={}.\n"),
                tags_to_text(activation_blocked),
                tags_to_text(cancel_if_gains),
                tags_to_text(owned_while_active),
                tags_to_text(blocked_owned),
                eviction.owner_name);
        }
    }

    auto apply_routine_blocker(ActiveEviction& eviction) -> void
    {
        if (!eviction.blocker_known || eviction.blocker_applied) { return; }

        auto* asc = weak_get(eviction.asc);
        if (!asc)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] routine blocker skipped: ASC unavailable owner={}.\n"),
                eviction.owner_name);
            return;
        }

        auto call = begin_call(asc, STR("AddTag"), STR("routine blocker apply"));
        set_param(call, STR("NewTag"), &eviction.blocker_tag, 8, STR("routine blocker apply"));
        if (!invoke_call(call, STR("routine blocker apply"))) { return; }

        eviction.blocker_applied = true;
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] routine blocker applied tag={} owner={}.\n"),
            fname_to_text(eviction.blocker_tag),
            eviction.owner_name);
    }

    // Single funnel for tag removal - called from EVERY eviction exit path
    // (finish, avatar-gone drop, clear_transient_state) so the tag cannot leak:
    // a leaked tag would suppress the NPC's interactions for the session. Loose
    // tags never serialize into savegames, so even the worst case clears on a
    // map reload.
    auto remove_routine_blocker(ActiveEviction& eviction, const TCHAR* reason) -> void
    {
        if (!eviction.blocker_applied) { return; }

        auto* asc = weak_get(eviction.asc);
        if (!asc)
        {
            // The ASC died with its world/actor; the tag died with it.
            eviction.blocker_applied = false;
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] routine blocker not removed: ASC gone reason={} owner={}.\n"),
                reason,
                eviction.owner_name);
            return;
        }

        auto call = begin_call(asc, STR("RemoveTag"), STR("routine blocker remove"));
        set_param(call, STR("TagToRemove"), &eviction.blocker_tag, 8, STR("routine blocker remove"));
        if (!invoke_call(call, STR("routine blocker remove")))
        {
            eviction.blocker_applied = false;
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] routine blocker REMOVE FAILED tag={} owner={} reason={} - tag stays until map reload (not save-persistent).\n"),
                fname_to_text(eviction.blocker_tag),
                eviction.owner_name,
                reason);
            return;
        }

        eviction.blocker_applied = false;
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] routine blocker removed tag={} owner={} reason={}.\n"),
            fname_to_text(eviction.blocker_tag),
            eviction.owner_name,
            reason);
    }

    auto try_claim_spot(ActiveEviction& eviction, bool allow_force_release) -> bool
    {
        auto* subsystem = find_interaction_subsystem();
        auto* player_state = find_player_state();
        if (!subsystem || !player_state) { return false; }

        if (claim_spot_once(subsystem, player_state, eviction)) { return true; }
        if (!allow_force_release) { return false; }

        // Deterministic fallback for the v0.7.0 Kharim case ("No tokens left, spot was
        // already claimed by State_..."): some NPCs never release their token, or
        // re-claim it within the same frame, so polling can never win the race.
        // Release every claim the NPC holds and take the spot in the same game-thread
        // tick. Done only once per eviction: if the claim still fails afterwards, the
        // blocker is a requirement check that no amount of retries can fix.
        if (!force_release_owner_claims(subsystem, eviction, STR("claim"))) { return false; }

        return claim_spot_once(subsystem, player_state, eviction);
    }

    // Returns the player's own GameplayAbilityInteract INSTANCE (not the class): the
    // instance carries m_InteractionActor, which is both pre-aimed at the station and
    // read back after activation to verify what the interaction actually targeted.
    // Weak-cached: this runs on every 10Hz take attempt and the FindAllOf sweep
    // was part of the v0.8.10 stutter.
    auto find_player_interact_ability_instance(UObject* player_state) -> UObject*
    {
        const auto outer_chain_reaches = [](UObject* object, UObject* target) {
            for (auto* outer = object->GetOuterPrivate(); outer; outer = outer->GetOuterPrivate())
            {
                if (outer == target) { return true; }
            }
            return false;
        };

        if (auto* cached = weak_get(m_cached_player_interact_ability))
        {
            if (is_usable(cached) && outer_chain_reaches(cached, player_state)) { return cached; }
        }

        std::vector<UObject*> abilities{};
        UObjectGlobals::FindAllOf(STR("GameplayAbilityInteract"), abilities);
        for (auto* ability : abilities)
        {
            if (!is_usable(ability)) { continue; }

            // Pointer-walk the outer chain instead of building full-name strings.
            if (outer_chain_reaches(ability, player_state))
            {
                m_cached_player_interact_ability = ability;
                return ability;
            }
        }

        return nullptr;
    }

    // The sensor's live target query filters by UInteractiveComponent::IsUsable().
    // The NPC's crafting marks the station's component m_IsBeingUsed=true and our
    // hard cancel (OnRequestEndQuick + claim force-release) skips the vanilla
    // teardown that resets it - the v0.8.4 diag showed nearest=<null> at the
    // whetstone/cauldron even with the view snapped at them from 1.9m, while the
    // multi-spot forge survived. Read the component's flags (diagnostics) and
    // clear the transient ones; SetCanBeUsed is NEVER called - its 'save' param
    // persists into savegames (closed track).
    auto make_station_usable(ActiveEviction& eviction, UObject* station) -> void
    {
        auto* component = read_object(station, STR("m_InteractiveComponent"));
        if (!component)
        {
            if (eviction.player_use_attempts <= 1)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] station usable skipped: no m_InteractiveComponent on station spot={}.\n"),
                    fname_to_text(eviction.spot));
            }
            return;
        }

        const auto is_being_used = read_bool(component, STR("m_IsBeingUsed"));
        const auto can_be_used = read_bool(component, STR("m_CanBeUsed"));
        const auto force_disabled = read_bool(component, STR("m_ForceDisableInteraction"));

        auto usable_call = begin_call(component, STR("IsUsable"), STR("station usable"));
        const auto is_usable = invoke_call(usable_call, STR("station usable")) &&
                               get_bool_param(usable_call, STR("ReturnValue"), STR("station usable"));

        const auto needs_fix = (is_being_used.found && is_being_used.value) ||
                               (force_disabled.found && force_disabled.value);
        if (needs_fix || eviction.player_use_attempts <= 2 || eviction.player_use_attempts % 10 == 0)
        {
            const auto flag_text = [](const BoolRead& flag) {
                return !flag.found ? STR("?") : flag.value ? STR("true") : STR("false");
            };
            // The component's own interact radii decide whether the player can be
            // sensed at all from where they stand - log them to settle the
            // "is 1.9-2.6m even inside the small station's range" question.
            const auto read_distance = [&](const TCHAR* function_name) -> double {
                auto distance_call = begin_call(component, function_name, STR("station usable"));
                if (!invoke_call(distance_call, STR("station usable"))) { return -1.0; }
                auto* property = find_call_property(distance_call, STR("ReturnValue"), 4, STR("station usable"));
                if (!property) { return -1.0; }
                float value{};
                std::memcpy(&value, distance_call.params.data() + property->GetOffset_Internal(), sizeof(value));
                return static_cast<double>(value);
            };
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] station state attempt={} isUsable={} isBeingUsed={} canBeUsed={} forceDisable={} interactDist={} closeDist={} farDist={} spot={}.\n"),
                eviction.player_use_attempts,
                is_usable ? STR("true") : STR("false"),
                flag_text(is_being_used),
                flag_text(can_be_used),
                flag_text(force_disabled),
                read_distance(STR("GetPlayerInteractDistance")),
                read_distance(STR("GetPlayerCloseInteractDistance")),
                read_distance(STR("GetPlayerFarInteractDistance")),
                fname_to_text(eviction.spot));
        }

        if (is_being_used.found && is_being_used.value)
        {
            auto call = begin_call(component, STR("SetIsBeingUsed"), STR("station usable"));
            set_bool_param(call, STR("Value"), false, STR("station usable"));
            const auto called = invoke_call(call, STR("station usable"));
            if (!called)
            {
                auto* property = CastField<FBoolProperty>(
                    component->GetPropertyByNameInChain(STR("m_IsBeingUsed")));
                if (property) { property->SetPropertyValueInContainer(component, false); }
            }
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] station SetIsBeingUsed(false) applied spot={} via={}.\n"),
                fname_to_text(eviction.spot),
                called ? STR("SetIsBeingUsed") : STR("direct flag write"));
        }

        if (force_disabled.found && force_disabled.value)
        {
            auto call = begin_call(component, STR("SetForceDisableInteraction"), STR("station usable"));
            set_bool_param(call, STR("Value"), false, STR("station usable"));
            const auto called = invoke_call(call, STR("station usable"));
            if (!called)
            {
                auto* property = CastField<FBoolProperty>(
                    component->GetPropertyByNameInChain(STR("m_ForceDisableInteraction")));
                if (property) { property->SetPropertyValueInContainer(component, false); }
            }
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] station SetForceDisableInteraction(false) applied spot={} via={}.\n"),
                fname_to_text(eviction.spot),
                called ? STR("SetForceDisableInteraction") : STR("direct flag write"));
        }

        if (can_be_used.found && !can_be_used.value && eviction.player_use_attempts <= 2)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] station canBeUsed=false spot={} - not touching (SetCanBeUsed persists to saves).\n"),
                fname_to_text(eviction.spot));
        }
    }

    // The interact ability resolves its target through the player's LOOK-based
    // live sensor query (UInteractSensor carries player/object peripheral-vision
    // cosines): with the view turned away the query returns null and TryActivate
    // fails - the v0.8.3 diag showed canInteract=false nearest=<null> for whole
    // 9s windows. Turn the controller toward the station before every take
    // attempt; the user pressed E on this station, so snapping the view to it
    // during the short locked window is the expected outcome anyway.
    auto face_station(UObject* station) -> void
    {
        auto* controller = find_player_controller_cached();
        const auto player_location = read_actor_location(find_player_actor_cached());
        const auto station_location = read_actor_location(station);
        if (!controller || !player_location.found || !station_location.found) { return; }

        const auto dx = station_location.value.X() - player_location.value.X();
        const auto dy = station_location.value.Y() - player_location.value.Y();
        if (dx * dx + dy * dy < 1.0) { return; }

        struct FRotatorMirror
        {
            double pitch;
            double yaw;
            double roll;
        } rotation{0.0, std::atan2(dy, dx) * 180.0 / 3.14159265358979323846, 0.0};

        auto call = begin_native_call(
            controller,
            STR("/Script/Engine.Controller:SetControlRotation"),
            STR("SetControlRotation"),
            STR("face station"));
        set_param(call, STR("NewRotation"), &rotation, sizeof(rotation), STR("face station"));
        invoke_call(call, STR("face station"));
    }

    // Walk the player up to the station: every station's component reports
    // interactDist=250 (v0.8.6 diag), so a player standing beyond 2.5m can
    // never be sensed - both v0.8.6 misses started at 3.0-3.2m. Movement input
    // is fed every engine tick while the take window is open; bForce=true
    // bypasses our own SetIgnoreMoveInput lock (which keeps blocking the
    // user's input), so the mod alone steers the character.
    auto drive_player_toward_station(ActiveEviction& eviction) -> void
    {
        auto* station = weak_get(eviction.station);
        auto* pawn = find_player_actor_cached();
        const auto player_location = read_actor_location(pawn);
        const auto station_location = read_actor_location(station);
        if (!pawn || !station || !player_location.found || !station_location.found) { return; }

        const auto dx = station_location.value.X() - player_location.value.X();
        const auto dy = station_location.value.Y() - player_location.value.Y();
        const auto distance_2d_squared = dx * dx + dy * dy;
        const auto stop_squared =
            eviction.approach_stop_distance * eviction.approach_stop_distance;
        if (distance_2d_squared <= stop_squared)
        {
            // Mark done even when no walking was needed (E pressed up close) -
            // the arrival gate keys off this flag and must not sit out the
            // whole grace in that case.
            if (!eviction.approach_done)
            {
                eviction.approach_done = true;
                if (eviction.approach_started)
                {
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] approach done distance={} spot={}.\n"),
                        std::sqrt(distance_2d_squared),
                        fname_to_text(eviction.spot));
                }
            }
            return;
        }

        if (!eviction.approach_started)
        {
            eviction.approach_started = true;
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] approach started distance={} spot={}.\n"),
                std::sqrt(distance_2d_squared),
                fname_to_text(eviction.spot));
        }

        const auto length = std::sqrt(distance_2d_squared);
        struct FVectorMirror
        {
            double x;
            double y;
            double z;
        } direction{dx / length, dy / length, 0.0};

        auto call = begin_native_call(
            pawn,
            STR("/Script/Engine.Pawn:AddMovementInput"),
            STR("AddMovementInput"),
            STR("approach station"));
        set_param(call, STR("WorldDirection"), &direction, sizeof(direction), STR("approach station"));
        float scale = 1.0f;
        set_param(call, STR("ScaleValue"), &scale, sizeof(scale), STR("approach station"));
        set_bool_param(call, STR("bForce"), true, STR("approach station"));
        invoke_call(call, STR("approach station"));
    }

    // "The player steps up to the station": wait until the interaction spot is
    // actually free (or held by our own claim), then start the player's interact
    // ability with the station as the explicit target.
    auto try_player_use_station(ActiveEviction& eviction) -> bool
    {
        auto* station = weak_get(eviction.station);
        auto* player_state = find_player_state();
        auto* asc = read_object(player_state, STR("AbilitySystemComponent"));
        auto* ability_instance = find_player_interact_ability_instance(player_state);
        auto* ability_class = ability_instance ? ability_instance->GetClassPrivate() : nullptr;
        if (!station || !asc || !ability_class)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] player use skipped: station={} asc={} abilityClass={}.\n"),
                station ? STR("ok") : STR("missing"),
                asc ? STR("ok") : STR("missing"),
                ability_class ? object_name(ability_class) : StringType{STR("<missing>")});
            return false;
        }

        // Same-tick combo: cancel the NPC's reacquired crafting ability, drop its spot
        // claims and activate the player's interaction inside ONE game-thread tick.
        // The AI routine can only react on a later tick, so the re-claim race that
        // v0.7.4 lost on WhetStone/Cauldron cannot interleave.
        // v0.8.3: the cancel half runs on EVERY attempt - even when the spot
        // reads free and even while we hold our own claim. The v0.8.2 log
        // proved the routine re-activates its crafting ability within
        // ~50-250ms of a cancel, and that ACTIVE (not yet claiming) ability
        // blocks the player's TryActivate: every success across v0.8.1/0.8.2
        // landed within ~150ms of a cancel, while burst windows where the
        // free spot made the combo skip its cancels produced 10-80 straight
        // activated=false over 1-9 seconds.
        {
            auto request_end_called = false;
            // Evaluate the cached ability instead of a full-object scan; the
            // scan remains only as a fallback for a dead weak ptr (unseen so
            // far - the instance survives the whole eviction in every log).
            auto search = evaluate_eviction_ability(eviction);
            if (!search.found && !weak_get(eviction.ability))
            {
                search = select_crafting_candidate(
                    STR("player use combo"),
                    &eviction.owner_name,
                    &eviction.avatar_name,
                    false,
                    false);
                if (search.found)
                {
                    eviction.ability = search.candidate.ability;
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] cached eviction ability died, re-found via scan owner={}.\n"),
                        eviction.owner_name);
                }
            }
            if (search.found)
            {
                // Full 10Hz cancel cadence (the v0.8.13 1-in-3 throttle is
                // reverted: it eased the pressure that made sticky routines
                // give up - Kharim went 0-for-5 windows under it after winning
                // on the 3rd window without it).
                request_end_called = call_request_end_quick(search.candidate, STR("player use combo"));
            }

            if (!eviction.has_claim)
            {
                const auto released = force_release_owner_claims(
                    find_interaction_subsystem(), eviction, STR("player use combo"),
                    eviction.combo_release_logged);
                if (released) { eviction.combo_release_logged = true; }

                // With an unknown spot freeness cannot be verified - proceed
                // anyway, same as the v0.7.4 gate did.
                const auto spot_free = !eviction.spot_known || is_spot_unclaimed(eviction.spot);

                // Same gate as the station-attempt log: a stuck-spot war printed
                // one of these per 100ms attempt in the v0.8.11 session (75
                // lines); spot_free stays loud - it is the transition that
                // matters.
                const auto combo_loud = eviction.player_use_attempts <= 2 ||
                                        eviction.player_use_attempts % 5 == 0 || spot_free;
                if ((request_end_called || !spot_free) && combo_loud)
                {
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] player use combo attempt={} requestEnd={} released={} spotFree={} spot={}.\n"),
                        eviction.player_use_attempts,
                        request_end_called ? STR("true") : STR("false"),
                        released ? STR("true") : STR("false"),
                        spot_free ? STR("true") : STR("false"),
                        eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")});
                }

                // Still held: retry. After a cancel the fast (100ms) recheck
                // lands inside the release window (the claim frees ~100ms after
                // a cancel, the routine re-claims in ~250ms - v0.8.1 log).
                if (!spot_free) { return false; }
            }
        }

        // Claim the spot for the player in the SAME tick as the cancel above -
        // the routine can only react next tick (the v0.8.3 combo principle).
        // The standalone claim phase ran on its own 500ms timer and missed the
        // ~100-250ms post-cancel free window 2-3 times per eviction; the user
        // saw that as the station "unlocking for milliseconds" until claim ok.
        // The v0.8.9 log: the successful activation always lands on the attempt
        // right AFTER claim ok - the claim has to survive one world tick before
        // the game reads the spot as available to the player.
        auto claimed_this_tick = false;
        if (eviction.spot_known && !eviction.has_claim)
        {
            run_guarded(STR("inline claim"), [&] {
                if (try_claim_spot(eviction, false))
                {
                    eviction.has_claim = true;
                    claimed_this_tick = true;
                }
            });
        }

        // Activation gate (v0.8.15): the activation runs its own live
        // nearest-interactive query that the pins below cannot override
        // (v0.7.10), and in a station cluster a NEIGHBOR can win it - Huno's
        // anvil lost to the adjacent forge at the 2.0m stop and the attempt=1
        // activation walked the player to the WRONG station (v0.8.14 log).
        // No activation until the sensor reports OUR station; while a neighbor
        // holds it, shrink the approach stop so the drive creeps the player in
        // until the station is the closest pick. Placed BEFORE the handoff so
        // a gated attempt keeps holding our claim (no release/re-claim churn).
        if (auto* station_actor = weak_get(eviction.station))
        {
            if (auto* sensor = find_player_sensor())
            {
                auto nearest_call = begin_call(sensor, STR("GetNearestInteraction"), STR("activation gate"));
                UObject* nearest = nullptr;
                if (invoke_call(nearest_call, STR("activation gate")))
                {
                    nearest = get_object_param(nearest_call, STR("ReturnValue"), STR("activation gate"));
                }
                if (nearest != station_actor)
                {
                    // Keep the view aimed at the station - the live query is
                    // look-based and the pin block below does not run on a
                    // gated attempt.
                    run_guarded(STR("face station (gated)"), [&] { face_station(station_actor); });
                    if (nearest && eviction.approach_stop_distance > kApproachMinStopDistance)
                    {
                        eviction.approach_stop_distance = std::max(
                            kApproachMinStopDistance,
                            eviction.approach_stop_distance - kApproachStopShrinkStep);
                        eviction.approach_done = false;
                    }
                    if (eviction.player_use_attempts <= 2 || eviction.player_use_attempts % 5 == 0)
                    {
                        Output::send<LogLevel::Verbose>(
                            STR("[LetMeCraft] activation gated attempt={}: sensor nearest={} != station, approachStop={}.\n"),
                            eviction.player_use_attempts,
                            object_name(nearest),
                            eviction.approach_stop_distance);
                    }
                    return false;
                }
            }
        }

        // Hand our claim over right before the player's own interaction claims
        // it - but never release a claim acquired THIS tick (it has not been
        // seen by a world tick yet; releasing it here would just re-open the
        // routine's re-claim race the inline claim exists to close).
        if (eviction.has_claim && !claimed_this_tick &&
            call_token_library_unclaim(eviction.claim_token, STR("unclaim (handoff)")))
        {
            eviction.has_claim = false;
        }

        // Fallback gate, active only when the NPC's interactivity could NOT be
        // force-disabled: never activate while the evicted NPC stands next to the
        // player - the activation targets the nearest interactive and would grab
        // the NPC (v0.7.11). With the disable in effect (v0.7.13) the NPC is
        // invisible to the interaction search and no waiting is needed.
        if (auto* avatar = eviction.npc_interaction_disabled ? nullptr : weak_get(eviction.avatar))
        {
            const auto avatar_location = read_actor_location(avatar);
            const auto player_location = read_actor_location(find_player_actor_cached());
            if (avatar_location.found && player_location.found)
            {
                const auto npc_distance_squared =
                    distance_squared(avatar_location.value, player_location.value);
                if (npc_distance_squared < kNpcClearDistanceSquared)
                {
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] player use waiting attempt={}: evicted NPC within clear radius (npcDistanceSquared={} < {}).\n"),
                        eviction.player_use_attempts,
                        npc_distance_squared,
                        kNpcClearDistanceSquared);
                    const auto now = Clock::now();
                    if (now >= eviction.re_retreat_at)
                    {
                        eviction.re_retreat_at = now + kReRetreatInterval;
                        run_guarded(STR("re-retreat"), [&] { issue_retreat(eviction); });
                    }
                    return false;
                }
            }
        }

        // Pin the target: clear the station's stale "busy" flags, turn the player's
        // view at the station (the live target query is look-based), then aim both
        // the sensor and the ability instance at the station explicitly. Sensing
        // stays LIVE for the whole window (v0.8.6): the sensor's sensed set only
        // picks up the just-freed station on its own sensing ticks, and freezing
        // locked the set in its pre-eviction state - empty for small stations
        // (v0.8.5 log: component healthy, nearest=<null> all window). The old
        // target-steal reason for the freeze is covered now: the NPC is
        // force-disabled and the mismatch-guard ends wrong grabs (proven on the
        // water barrel).
        make_station_usable(eviction, station);
        face_station(station);
        if (auto* sensor = find_player_sensor())
        {
            write_object_property(sensor, STR("m_CurrentInteractionActor"), station);
            // v0.7.10 proved the activation ignores the written/frozen sensor field
            // (sensorTarget held OUR station, the ability still targeted the NPC) -
            // it does its own live nearest-interactive query. m_FilterActor is the
            // sensor's only filter knob; pin it to the station for the use window
            // (cleared in unfreeze_player_sensor). Semantics are undocumented - if
            // it is an EXCLUDE filter the verify+EndAndCall net below still holds.
            write_object_property(sensor, STR("m_FilterActor"), station);
            m_sensor_pinned = true;
        }
        write_object_property(ability_instance, STR("m_InteractionActor"), station);
        write_object_property(ability_instance, STR("m_InteractiveActor"), station);

        auto call = begin_native_call(
            asc,
            STR("/Script/GameplayAbilities.AbilitySystemComponent:TryActivateAbilityByClass"),
            STR("TryActivateAbilityByClass"),
            STR("player use"));
        set_param(call, STR("InAbilityToActivate"), &ability_class, 8, STR("player use"));
        set_bool_param(call, STR("bAllowRemoteActivation"), false, STR("player use"));
        if (!invoke_call(call, STR("player use"))) { return false; }

        const auto activated = get_bool_param(call, STR("ReturnValue"), STR("player use"));
        if (activated || eviction.player_use_attempts <= 2 || eviction.player_use_attempts % 5 == 0)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] player use station attempt={} activated={} abilityClass={} spot={}.\n"),
                eviction.player_use_attempts,
                activated ? STR("true") : STR("false"),
                object_name(ability_class),
                fname_to_text(eviction.spot));
        }
        if (!activated)
        {
            // Sensor-eye diagnostic (~1Hz at the burst cadence): does the player's
            // own interaction sensor consider anything interactable right now? If
            // canInteract=false / nearest=null while the spot is FREE, the blocker
            // is the game's requirement check, not the routine's churn - that is
            // the plan-C trigger (UseSpot/gameplay-event with explicit target).
            if (eviction.player_use_attempts % 10 == 0)
            {
                if (auto* sensor = find_player_sensor())
                {
                    auto can_call = begin_call(sensor, STR("CanInteract"), STR("player use diag"));
                    const auto can_interact = invoke_call(can_call, STR("player use diag")) &&
                                              get_bool_param(can_call, STR("ReturnValue"), STR("player use diag"));
                    auto nearest_call = begin_call(sensor, STR("GetNearestInteraction"), STR("player use diag"));
                    UObject* nearest = nullptr;
                    if (invoke_call(nearest_call, STR("player use diag")))
                    {
                        nearest = get_object_param(nearest_call, STR("ReturnValue"), STR("player use diag"));
                    }
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] player use diag attempt={} canInteract={} nearest={}.\n"),
                        eviction.player_use_attempts,
                        can_interact ? STR("true") : STR("false"),
                        object_name(nearest));
                }
            }
            return false;
        }

        // Verify what the interaction actually targeted: if the activation re-picked
        // its own target (the NPC -> dialogue, or a closer station), end it right
        // away and retry on the cadence instead of reporting a false success.
        auto* actual_target = read_object(ability_instance, STR("m_InteractionActor"));
        if (actual_target != station)
        {
            // Diagnostic: if the sensor still holds OUR station here, the activation
            // reads its target elsewhere (live nearest-interactive compute) - that is
            // the plan-C trigger; if it holds something else, the freeze is leaky.
            auto* sensor = find_player_sensor();
            auto* sensor_target = sensor ? read_object(sensor, STR("m_CurrentInteractionActor")) : nullptr;
            auto* filter_actor = sensor ? read_object(sensor, STR("m_FilterActor")) : nullptr;
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] player use target mismatch attempt={} target={} station={} sensorTarget={} filterActor={} - ending wrong interaction.\n"),
                eviction.player_use_attempts,
                object_name(actual_target),
                object_name(station),
                object_name(sensor_target),
                object_name(filter_actor));
            call_no_params_safely(ability_instance, STR("EndAndCall"), STR("player use target mismatch"));

            // The evicted NPC standing next to the player keeps stealing the target:
            // send it away again (it drifts back between kill-guard cancels).
            const auto now = Clock::now();
            if (actual_target == weak_get(eviction.avatar) && now >= eviction.re_retreat_at)
            {
                eviction.re_retreat_at = now + kReRetreatInterval;
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] re-retreating NPC: it stole the interaction target.\n"));
                run_guarded(STR("re-retreat"), [&] { issue_retreat(eviction); });
            }
            return false;
        }

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] player use target verified station={}.\n"),
            object_name(station));
        return true;
    }

    auto log_claim_fail_reasons(ActiveEviction& eviction) -> void
    {
        auto* subsystem = find_interaction_subsystem();
        auto* player_state = find_player_state();
        if (!subsystem || !player_state) { return; }

        auto call = begin_call(subsystem, STR("DebugCanPawnClaimSpotFailReason"), STR("claim debug"));
        set_param(call, STR("Spot"), &eviction.spot, 8, STR("claim debug"));
        set_param(call, STR("PotentialUser"), &player_state, 8, STR("claim debug"));
        set_param(call, STR("Action"), &eviction.action, 8, STR("claim debug"));
        if (!invoke_call(call, STR("claim debug"))) { return; }

        struct RawArray
        {
            void* data{};
            int32_t num{};
            int32_t max{};
        };
        struct RawString
        {
            const wchar_t* data{};
            int32_t num{};
            int32_t max{};
        };

        RawArray reasons{};
        if (!get_param(call, STR("ReturnValue"), &reasons, 16, STR("claim debug"))) { return; }

        Output::send<LogLevel::Warning>(
            STR("[LetMeCraft] claim gave up spot={} action={} reasonCount={}.\n"),
            fname_to_text(eviction.spot),
            fname_to_text(eviction.action),
            reasons.num);

        const auto count = (reasons.data && reasons.num > 0) ? (reasons.num < 8 ? reasons.num : 8) : 0;
        for (auto index = 0; index < count; ++index)
        {
            const auto* element = static_cast<const RawString*>(reasons.data) + index;
            if (element->data && element->num > 0)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft]   claim fail reason: {}\n"),
                    StringType{element->data, static_cast<size_t>(element->num - 1)});
            }
        }
        // The returned TArray<FString> buffers are intentionally leaked: rare
        // diagnostic path, freeing them would require the engine allocator.
    }

    auto read_world_time_seconds() -> double
    {
        auto* world_context = find_player_actor_cached();
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.GameplayStatics:GetTimeSeconds"));
        auto* statics = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        if (!world_context || !function || !statics) { return -1.0; }

        auto call = begin_call_with_function(statics, function, STR("world time"));
        set_param(call, STR("WorldContextObject"), &world_context, 8, STR("world time"));
        if (!invoke_call(call, STR("world time"))) { return -1.0; }

        double seconds{};
        if (!get_param(call, STR("ReturnValue"), &seconds, 8, STR("world time"))) { return -1.0; }
        return seconds;
    }

    // Locates the live FInteractionSpot value inside InteractionSubsystem.SpotsByName
    // (TMap<FName, FInteractionSpot>: key 8/4, value 0x118/8 — verified in the SDK dump).
    auto find_spot_record(UObject* subsystem, const FName& spot) -> unsigned char*
    {
        auto* property = CastField<FMapProperty>(subsystem->GetPropertyByNameInChain(STR("SpotsByName")));
        if (!property) { return nullptr; }

        auto* map = property->ContainerPtrToValuePtr<FScriptMap>(subsystem);
        if (!map) { return nullptr; }

        const auto layout = FScriptMap::GetScriptLayout(8, 4, kInteractionSpotValueSize, 8);
        for (int32_t index = 0, max_index = map->GetMaxIndex(); index < max_index; ++index)
        {
            if (!map->IsValidIndex(index)) { continue; }

            auto* pair = static_cast<unsigned char*>(map->GetData(index, layout));
            if (std::memcmp(pair, &spot, 8) != 0) { continue; }

            return pair + layout.ValueOffset;
        }

        return nullptr;
    }

    auto apply_spot_cooldown(ActiveEviction& eviction) -> bool
    {
        auto* subsystem = find_interaction_subsystem();
        if (!subsystem) { return false; }

        auto* record = find_spot_record(subsystem, eviction.spot);
        if (!record)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] spot cooldown: record not found spot={}.\n"),
                fname_to_text(eviction.spot));
            return false;
        }

        const auto now_seconds = read_world_time_seconds();
        if (now_seconds < 0.0) { return false; }

        auto* cooldown_duration = reinterpret_cast<float*>(record + kSpotCooldownDurationOffset);
        auto* last_time_used = reinterpret_cast<float*>(record + kSpotLastTimeUsedOffset);
        eviction.saved_cooldown_duration = *cooldown_duration;
        eviction.saved_last_time_used = *last_time_used;
        *cooldown_duration = kSpotCooldownSeconds;
        *last_time_used = static_cast<float>(now_seconds);
        eviction.has_cooldown_write = true;

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] spot cooldown applied spot={} duration={} worldTime={} (was duration={} lastUsed={}).\n"),
            fname_to_text(eviction.spot),
            kSpotCooldownSeconds,
            now_seconds,
            eviction.saved_cooldown_duration,
            eviction.saved_last_time_used);
        return true;
    }

    auto restore_spot_cooldown(ActiveEviction& eviction) -> void
    {
        if (!eviction.has_cooldown_write) { return; }
        eviction.has_cooldown_write = false;

        auto* subsystem = find_interaction_subsystem();
        if (!subsystem) { return; }

        auto* record = find_spot_record(subsystem, eviction.spot);
        if (!record) { return; }

        *reinterpret_cast<float*>(record + kSpotCooldownDurationOffset) = eviction.saved_cooldown_duration;
        *reinterpret_cast<float*>(record + kSpotLastTimeUsedOffset) = eviction.saved_last_time_used;

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] spot cooldown restored spot={} duration={} lastUsed={}.\n"),
            fname_to_text(eviction.spot),
            eviction.saved_cooldown_duration,
            eviction.saved_last_time_used);
    }

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

    auto finish_eviction(ActiveEviction& eviction, const TCHAR* reason) -> void
    {
        // First, before the unclaim: a throw later in this function must not
        // leave the routine-blocker tag on the NPC.
        run_guarded(STR("routine blocker remove"), [&] {
            remove_routine_blocker(eviction, reason);
        });

        auto unclaimed = false;
        if (eviction.has_claim)
        {
            unclaimed = call_token_library_unclaim(eviction.claim_token, STR("unclaim"));
        }
        if (eviction.npc_interaction_disabled)
        {
            run_guarded(STR("npc interaction re-enable"), [&] {
                set_npc_interaction_enabled(eviction, true);
            });
        }
        unlock_player_movement(reason);
        unfreeze_player_sensor(reason);

        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] eviction finished reason={} owner={} avatar={} spot={} unclaim={}.\n"),
            reason,
            eviction.owner_name,
            eviction.avatar_name,
            fname_to_text(eviction.spot),
            eviction.has_claim ? (unclaimed ? STR("ok") : STR("failed")) : STR("<none>"));
    }

    // Returns false when the eviction entry should be removed by the caller.
    // Every phase runs under its own guard: one throwing phase neither kills the
    // others nor escapes to the engine-tick handler anonymously.
    auto tick_eviction(ActiveEviction& eviction, Clock::time_point now) -> bool
    {
        auto* avatar = weak_get(eviction.avatar);
        if (!avatar)
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] eviction dropped: avatar gone owner={} avatar={}.\n"),
                eviction.owner_name,
                eviction.avatar_name);
            // Separate guard: an unclaim throw inside the drop cleanup must not
            // skip the tag removal (the ASC may outlive the avatar).
            run_guarded(STR("routine blocker remove"), [&] {
                remove_routine_blocker(eviction, STR("avatar gone"));
            });
            run_guarded(STR("drop cleanup"), [&] {
                if (eviction.has_claim)
                {
                    call_token_library_unclaim(eviction.claim_token, STR("unclaim (avatar gone)"));
                }
                if (eviction.npc_interaction_disabled)
                {
                    set_npc_interaction_enabled(eviction, true);
                }
                unlock_player_movement(STR("avatar gone"));
                unfreeze_player_sensor(STR("avatar gone"));
            });
            return false;
        }

        if (!eviction.move_issued && now >= eviction.move_at)
        {
            eviction.move_issued = true;
            run_guarded(STR("retreat"), [&] { issue_retreat(eviction); });
            // Quiet tick (+250ms after the cancel): safe to resolve the component.
            run_guarded(STR("npc interaction disable"), [&] {
                set_npc_interaction_enabled(eviction, false);
            });
        }

        if (eviction.spot_known && !eviction.has_claim && !eviction.claim_gave_up &&
            !eviction.player_use_done && now >= eviction.next_claim_at)
        {
            // The NPC releases its own claim shortly after OnRequestEndQuick; a few
            // retries, force-release once. No cooldown fallback: a spot cooldown
            // locks the station for the player too (v0.7.3 lesson). The attempts
            // counter also acts as the failsafe if the claim phase keeps throwing.
            eviction.next_claim_at = now + kClaimRetryInterval;
            ++eviction.claim_attempts;
            if (eviction.claim_attempts > kClaimMaxAttempts + 2)
            {
                eviction.claim_gave_up = true;
            }
            else
            {
                run_guarded(STR("claim"), [&] {
                    if (try_claim_spot(eviction, eviction.claim_attempts == 2))
                    {
                        eviction.has_claim = true;
                    }
                    else if (eviction.claim_attempts >= kClaimMaxAttempts)
                    {
                        eviction.claim_gave_up = true;
                        log_claim_fail_reasons(eviction);
                    }
                });
            }
        }

        // The auto-take window is the move-lock span (10s), shorter than the 17s
        // parking hold. Once it closes, stop poking the interact ability and give
        // the player back full control - late activations while the player walks
        // around would grab whatever interactive they pass (v0.8.1 risk).
        // v0.8.12: the attempt cap closes it early - a window that ran past
        // every historical success point is a stuck spot, not a slow one.
        if (!eviction.player_use_done &&
            (now >= eviction.player_use_until ||
             eviction.player_use_attempts >= kPlayerUseMaxAttempts))
        {
            const auto capped = eviction.player_use_attempts >= kPlayerUseMaxAttempts;
            eviction.player_use_done = true;
            run_guarded(STR("interaction window close"), [&] {
                unlock_player_movement(STR("interaction window over"));
                unfreeze_player_sensor(STR("interaction window over"));
            });
            // The take FAILED - the spot stays with the NPC. End the parking
            // (and with it the 1Hz kill-guard fight against the routine) soon
            // instead of churning for the rest of the 17s hold.
            if (now + kFailedTakeHoldTail < eviction.resume_at)
            {
                eviction.resume_at = now + kFailedTakeHoldTail;
            }
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] auto-take window closed (reason={}) after {} attempts owner={} spot={} - take failed, parking shortened to {}s.\n"),
                capped ? STR("attempt cap, spot stuck") : STR("timeout"),
                eviction.player_use_attempts,
                eviction.owner_name,
                eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")},
                std::chrono::duration_cast<std::chrono::seconds>(kFailedTakeHoldTail).count());
        }

        // Feed the approach every engine tick (AddMovementInput is per-frame
        // input) from eviction start until the take succeeds or the window
        // closes - the ~1s pre-take pause is already walking time.
        if (!eviction.player_use_done)
        {
            run_guarded(STR("approach station"), [&] { drive_player_toward_station(eviction); });
        }

        // Arrival gate: no activation while the player is still walking - the
        // v0.8.8 mid-walk activations grabbed bystander interactives (water
        // barrel) and paid seconds of mismatch recovery. The grace is the
        // blocked-path fallback.
        if (!eviction.player_use_done && now >= eviction.player_use_at &&
            (eviction.approach_done || now >= eviction.approach_grace_until))
        {
            ++eviction.player_use_attempts;

            auto activated = false;
            run_guarded(STR("player use"), [&] { activated = try_player_use_station(eviction); });
            if (activated)
            {
                // The player occupies the station now - release the player right
                // away, but KEEP the eviction alive: the NPC stays parked behind
                // the player for the full hold (user request v0.8.2) instead of
                // wandering back to the station area while the player works.
                eviction.player_use_done = true;
                eviction.player_took_station = true;
                run_guarded(STR("bridge cleanup"), [&] {
                    if (eviction.npc_interaction_disabled)
                    {
                        set_npc_interaction_enabled(eviction, true);
                    }
                    unlock_player_movement(STR("player took station"));
                    unfreeze_player_sensor(STR("player took station"));
                });
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] player took station owner={} spot={} - NPC keeps parking until the hold expires.\n"),
                    eviction.owner_name,
                    eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")});
            }
            else
            {
                eviction.player_use_at = now + kPlayerUseBurstInterval;
            }
        }

        // v0.8.13: per-engine-tick claim poll. The routine re-claims the token
        // within 0-1 world ticks of its task releasing it; the 100ms attempt
        // cadence saw ~1 in 6 of those gaps and lost the race on sticky spots
        // (whetstone/cauldron wars in the v0.8.12 log). This prehook runs
        // BEFORE each world tick, so a poll every tick catches every >=1-tick
        // gap. Placed AFTER the take attempt on purpose: a claim acquired here
        // is one world tick old when the next attempt's handoff releases it
        // (the v0.8.9 survive-one-tick rule) - same-tick acquisition inside an
        // attempt stays with the inline claim and its claimed_this_tick guard.
        // The IsUnclaimed pre-gate keeps a held token to one quiet read per
        // tick instead of a game-side "No tokens left" warning at 60Hz.
        // claim_gave_up is deliberately ignored: it only ends the 500ms
        // diagnostics phase, not the race.
        if (!eviction.player_use_done && eviction.spot_known && !eviction.has_claim)
        {
            run_guarded(STR("claim poll"), [&] {
                if (is_spot_unclaimed(eviction.spot) && try_claim_spot(eviction, false))
                {
                    eviction.has_claim = true;
                }
            });
        }

        // Park the NPC behind the player for the whole hold: the routine drags it
        // back toward the station between cancels, so when it drifts off the
        // behind-point the walk order is gently re-issued (no teleports).
        if (eviction.move_issued && eviction.retreat_dest_known &&
            now >= eviction.stand_still_from && now >= eviction.re_retreat_at)
        {
            const auto avatar_location = read_actor_location(avatar);
            if (avatar_location.found &&
                distance_squared(avatar_location.value, eviction.retreat_dest) >
                    kBehindHoldToleranceSquared)
            {
                eviction.re_retreat_at = now + kReRetreatInterval;
                // The routine fights the parking every second for up to 17s -
                // log the first re-issue and every 5th, not all of them.
                const auto loud = eviction.behind_hold_reissues % 5 == 0;
                ++eviction.behind_hold_reissues;
                if (loud)
                {
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] behind-hold: NPC drifted off the parking point, re-issuing walk owner={} reissues={}.\n"),
                        eviction.owner_name,
                        eviction.behind_hold_reissues);
                }
                run_guarded(STR("behind-hold re-retreat"), [&] { issue_retreat(eviction, !loud); });
            }
        }

        // v0.8.12: the guard stops once the player RUNS the station - the routine
        // cannot reclaim an occupied spot, so post-success cancels were pure GAS
        // teardown churn on the NPC ability (the v0.8.11 crash followed exactly
        // that churn against an NPC whose actor the game had recreated mid-fight).
        if (!eviction.player_took_station &&
            now >= eviction.stand_still_from && now >= eviction.next_enforce_at)
        {
            eviction.next_enforce_at = now + kHoldEnforceInterval;
            run_guarded(STR("kill guard"), [&] {
                // Bridge guard only: if the routine re-grabs the station while the
                // player is taking over, end it again. No pausing, no repositioning -
                // the NPC's behavior stays natural. Evaluates the cached ability
                // (scan fallback only on a dead weak ptr).
                auto search = evaluate_eviction_ability(eviction);
                if (!search.found && !weak_get(eviction.ability))
                {
                    search = select_crafting_candidate(
                        STR("kill guard"),
                        &eviction.owner_name,
                        &eviction.avatar_name,
                        false,
                        false);
                    if (search.found) { eviction.ability = search.candidate.ability; }
                }
                if (search.found)
                {
                    const auto request_end_called = call_request_end_quick(search.candidate, STR("kill guard"));
                    // Also drop the claims the reacquired ability took: cancelling the
                    // ability alone leaves the token held, which is what starved the
                    // player-use poll on Kharim/Snaf in v0.7.4.
                    const auto released = force_release_owner_claims(
                        find_interaction_subsystem(), eviction, STR("kill guard"));
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] kill guard: crafting reacquired owner={} requestEnd={} released={} claim={}.\n"),
                        eviction.owner_name,
                        request_end_called ? STR("true") : STR("false"),
                        released ? STR("true") : STR("false"),
                        eviction.has_claim ? STR("held") : STR("none"));
                }
            });
        }

        if (now >= eviction.resume_at)
        {
            run_guarded(STR("finish"), [&] { finish_eviction(eviction, STR("bridge window expired")); });
            return false;
        }

        return true;
    }

    auto tick_evictions() -> void
    {
        if (m_evictions.empty()) { return; }

        const auto now = Clock::now();
        for (auto index = m_evictions.size(); index > 0; --index)
        {
            if (!tick_eviction(m_evictions[index - 1], now))
            {
                m_evictions.erase(m_evictions.begin() + static_cast<std::ptrdiff_t>(index - 1));
            }
        }
    }

    auto schedule_post_check(const CraftingCandidate& candidate, Clock::time_point now) -> void
    {
        m_post_check_active = true;
        m_post_check_started_at = now;
        m_post_check_index = 0;
        m_post_check_at = m_post_check_started_at + post_check_delay_for_index(m_post_check_index);
        m_post_check_owner_name = candidate.owner_name;
        m_post_check_avatar_name = candidate.avatar_name;
    }

    auto tick_post_check() -> void
    {
        if (!m_post_check_active) { return; }
        if (Clock::now() < m_post_check_at) { return; }

        m_post_check_active = false;
        const auto label = post_check_label_for_index(m_post_check_index);
        const auto search = select_crafting_candidate(
            label,
            &m_post_check_owner_name,
            &m_post_check_avatar_name,
            false,
            false);

        if (search.found)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] {}: target still has active crafting ability owner={} avatar={} ability={} root={} distanceSquared={}.\n"),
                label,
                m_post_check_owner_name,
                m_post_check_avatar_name,
                object_name(search.candidate.ability),
                object_name(search.candidate.root_task),
                search.candidate.distance_squared);
        }
        else
        {
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {}: target no longer has active nearby crafting ability owner={} avatar={}.\n"),
                label,
                m_post_check_owner_name,
                m_post_check_avatar_name);
        }

        if (m_post_check_index < 2)
        {
            ++m_post_check_index;
            m_post_check_at = m_post_check_started_at + post_check_delay_for_index(m_post_check_index);
            m_post_check_active = true;
            return;
        }

        m_post_check_owner_name.clear();
        m_post_check_avatar_name.clear();
        m_post_check_started_at = {};
        m_post_check_index = 0;
    }

    auto poll_gamepad_y() -> void
    {
        if (!ensure_xinput()) { return; }

        auto y_is_down = false;
        for (DWORD user_index = 0; user_index < 4; ++user_index)
        {
            XInputState state{};
            if (m_xinput_get_state(user_index, &state) == 0 && (state.Gamepad.wButtons & kXInputGamepadY))
            {
                y_is_down = true;
                break;
            }
        }

        if (y_is_down && !m_gamepad_y_was_down)
        {
            queue_manual_request(STR("gamepad Y"));
        }

        m_gamepad_y_was_down = y_is_down;
    }

    auto ensure_xinput() -> bool
    {
        if (m_xinput_checked) { return m_xinput_get_state != nullptr; }
        m_xinput_checked = true;

        for (const auto* dll_name : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"})
        {
            m_xinput_module = Windows::LoadLibraryW(dll_name);
            if (!m_xinput_module) { continue; }

            m_xinput_get_state = reinterpret_cast<XInputGetStateFn>(Windows::GetProcAddress(m_xinput_module, "XInputGetState"));
            if (m_xinput_get_state)
            {
                Output::send<LogLevel::Verbose>(STR("[LetMeCraft] XInput loaded for gamepad Y support.\n"));
                return true;
            }
        }

        Output::send<LogLevel::Warning>(
            STR("[LetMeCraft] XInput was not available; keyboard E still works, gamepad Y is disabled.\n"));
        return false;
    }

    HMODULE m_xinput_module{};
    XInputGetStateFn m_xinput_get_state{};
    Clock::time_point m_next_manual_request_time{};
    Clock::time_point m_post_check_started_at{};
    Clock::time_point m_post_check_at{};
    std::vector<ActiveEviction> m_evictions{};
    FWeakObjectPtr m_cached_subsystem{};
    FWeakObjectPtr m_cached_player_controller{};
    FWeakObjectPtr m_cached_player_state{};
    FWeakObjectPtr m_cached_player_sensor{};
    FWeakObjectPtr m_cached_player_interact_ability{};
    StringType m_post_check_owner_name{};
    StringType m_post_check_avatar_name{};
    StringType m_pending_manual_source{};
    std::mutex m_pending_request_mutex{};
    Unreal::Hook::GlobalCallbackId m_engine_tick_callback_id{};
    Unreal::Hook::GlobalCallbackId m_load_map_callback_id{};
    Unreal::Hook::GlobalCallbackId m_init_game_state_callback_id{};
    Clock::time_point m_move_unlock_at{};
    bool m_move_input_locked{};
    int m_move_unlock_attempts{};
    int32_t m_saved_jump_max_count{1};
    bool m_sensor_frozen{};
    bool m_sensor_pinned{};
    bool m_controller_functions_warmed{};
    int m_warmup_attempts{};
    Clock::time_point m_next_warmup_at{};
    FWeakObjectPtr m_set_ignore_move_input_function{};
    FWeakObjectPtr m_reset_ignore_move_input_function{};
    int m_post_check_index{};
    bool m_xinput_checked{};
    bool m_gamepad_y_was_down{};
    bool m_post_check_active{};
    bool m_has_pending_manual_request{};
};

#define LET_ME_CRAFT_API __declspec(dllexport)

extern "C"
{
    LET_ME_CRAFT_API CppUserModBase* start_mod()
    {
        return new LetMeCraft();
    }

    LET_ME_CRAFT_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}

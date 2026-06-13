#pragma once

#include "Common.hpp"

namespace lmc
{
    // v0.8.16 routine blocker: a loose GameplayTag taken from the craft ability's
    // ActivationBlockedTags and added to the NPC's ASC for the whole eviction. GAS
    // then refuses to re-activate the interaction at the source, which no
    // cancel/poll cadence could win (the routine re-claims in the SAME world tick
    // as our force-release). Loose tags never serialize into savegames. Stateless:
    // every method operates on the passed-in ActiveEviction.
    class RoutineBlocker
    {
    public:
        // Pick the routine-blocker tag and emit the one-per-eviction diagnostic. Runs at
        // eviction start while the candidate's GAS objects are still alive (same-tick
        // property READS are proven safe - start_eviction already does them).
        auto select_routine_blocker(ActiveEviction& eviction, const CraftingCandidate& candidate) -> void
        {
            const auto activation_blocked = read_tag_container(candidate.ability, STR("ActivationBlockedTags"));

            const auto chosen = choose_blocker_tag(activation_blocked);
            eviction.blocker_tag = chosen.value;
            eviction.blocker_known = chosen.found;
            if (chosen.found) { return; }

            // Failure diagnostics only: read the root task's containers so the
            // warning lists every tag the next calibration could pick from.
            const auto cancel_if_gains = read_tag_container(candidate.root_task, STR("CancelIfCharacterGainsAnyOf"));
            auto owned_while_active = read_tag_container(candidate.root_task, STR("OwnedTagsWhileActive"));
            if (!owned_while_active.found)
            {
                // The UAbilityTask_AI branch spells it OwnedTagsToAddWhileActive.
                owned_while_active = read_tag_container(candidate.root_task, STR("OwnedTagsToAddWhileActive"));
            }
            const auto blocked_owned = read_tag_container(candidate.root_task, STR("BlockedOwnedTags"));
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] no safe blocker tag - running without one (v0.8.15 behavior). ActivationBlockedTags={} CancelIfCharacterGainsAnyOf={} OwnedTagsWhileActive={} BlockedOwnedTags={} owner={}.\n"),
                tags_to_text(activation_blocked),
                tags_to_text(cancel_if_gains),
                tags_to_text(owned_while_active),
                tags_to_text(blocked_owned),
                eviction.owner_name);
        }

        auto apply_routine_blocker(ActiveEviction& eviction) -> void
        {
            if (!eviction.blocker_known || eviction.blocker_applied) { return; }

            auto* asc = weak_get(eviction.asc);
            if (!asc) { return; }

            auto call = begin_call(asc, STR("AddTag"), STR("routine blocker apply"));
            set_param(call, STR("NewTag"), &eviction.blocker_tag, 8, STR("routine blocker apply"));
            if (!invoke_call(call, STR("routine blocker apply"))) { return; }

            eviction.blocker_applied = true;
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
        }
    };
}

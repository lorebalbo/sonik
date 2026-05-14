#pragma once
//==============================================================================
// PRD-0042: Pure binding resolver.
//
// Single static function `resolve(...)` that turns one inbound MIDI event into
// at most one ResolvedBinding by consulting the parsed Mapping.
//
// THREAD CONTRACT
//   * Callable from the MIDI callback thread (PRD-0040).
//   * No allocation, no locking, no I/O.
//   * `Mapping` is read-only input; `ResolverState` is per-device mutable
//     state (only the LSB cache mutates here).
//==============================================================================

#include "MappingTypes.h"
#include "MidiInboundEvent.h"

#include <optional>

namespace sonik::midi
{
    struct BindingResolver
    {
        BindingResolver()  = delete;
        ~BindingResolver() = delete;

        static std::optional<ResolvedBinding> resolve (const Mapping&         mapping,
                                                       ResolverState&         state,
                                                       const MidiInboundEvent& event,
                                                       ModifierMask           currentMask) noexcept;
    };
} // namespace sonik::midi

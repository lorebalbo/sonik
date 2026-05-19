#pragma once
//==============================================================================
// PRD-0042: Control Target Registry.
//
// A canonical, compile-time table of every mappable application command.
// Each target has a stable string id of the form
//   <domain>.<scope>.<command>[.<index>]
// e.g. "deck.A.transport.play", "library.load.deck.B", "mixer.crossfader".
//
// THIS FILE IS A VOCABULARY. It MUST NOT #include any header from
// Source/Features/Deck, AudioEngine, Mixer, or Library. Routing of resolved
// bindings to feature modules is PRD-0044's responsibility.
//
// Adding a new mappable command:
//   1. Add a row below using SONIK_PER_DECK / SONIK_GLOBAL.
//   2. Ensure the referenced MidiTargetCategory exists in
//      MidiTargetCategory.h and has a routing entry in MidiMessageBridge.h.
//   3. The constexpr build will fail-fast if the registry exceeds the
//      compile-time size limit or duplicates an id (see static_asserts).
//==============================================================================

#include "MidiTargetCategory.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace sonik::midi
{
    //--------------------------------------------------------------------------
    using TargetIndex = std::uint16_t;

    inline constexpr TargetIndex InvalidTargetIndex = static_cast<TargetIndex> (0xFFFFu);

    //--------------------------------------------------------------------------
    enum class TargetValueKind : std::uint8_t
    {
        Momentary,
        Toggle,
        Continuous,
        RelativeDelta,
    };

    enum class DeckScope : std::uint8_t
    {
        Global,
        PerDeck,
    };

    inline constexpr std::uint8_t GlobalDeckIndex = 255;

    //--------------------------------------------------------------------------
    struct ControlTarget
    {
        const char*        id;
        MidiTargetCategory category;
        TargetValueKind    valueKind;
        DeckScope          deckScope;
        std::uint8_t       deckIndex; // 255 (GlobalDeckIndex) when scope == Global *and* not deck-tagged.
    };

    //--------------------------------------------------------------------------
    // Compile-time registry construction. Macros below keep the per-deck
    // expansion readable while preserving the strict POD nature of each row.
    //--------------------------------------------------------------------------
    namespace detail
    {
        // clang-format off
        #define SONIK_TARGET(idStr, cat, val, scope, deck) \
            ControlTarget { (idStr), MidiTargetCategory::cat, TargetValueKind::val, DeckScope::scope, static_cast<std::uint8_t> (deck) }

        #define SONIK_PER_DECK(suffix, cat, val) \
            SONIK_TARGET("deck.A." suffix, cat, val, PerDeck, 0), \
            SONIK_TARGET("deck.B." suffix, cat, val, PerDeck, 1), \
            SONIK_TARGET("deck.C." suffix, cat, val, PerDeck, 2), \
            SONIK_TARGET("deck.D." suffix, cat, val, PerDeck, 3)

        #define SONIK_HOTCUE_ROW(n) \
            SONIK_PER_DECK("hotcue." #n ".trigger", HotCueTrigger, Momentary), \
            SONIK_PER_DECK("hotcue." #n ".delete",  HotCueDelete,  Momentary)

        #define SONIK_BEATJUMP_ROW(sz) \
            SONIK_PER_DECK("beatjump.minus." #sz, BeatJumpMinus, Momentary), \
            SONIK_PER_DECK("beatjump.plus."  #sz, BeatJumpPlus,  Momentary)

        inline constexpr ControlTarget kRegistry[] = {
            // ---- Per-deck transport --------------------------------------
            SONIK_PER_DECK("transport.play", TransportPlay, Momentary),
            SONIK_PER_DECK("transport.cue",  TransportCue,  Momentary),
            SONIK_PER_DECK("transport.sync", TransportSync, Momentary),

            // ---- Per-deck pitch / gain -----------------------------------
            SONIK_PER_DECK("pitchFader",        PitchFader,      Continuous),
            SONIK_PER_DECK("pitchRange.cycle",  PitchRangeCycle, Momentary),
            SONIK_PER_DECK("gain",              Gain,            Continuous),

            // ---- Per-deck EQ ---------------------------------------------
            SONIK_PER_DECK("eq.high", EqHigh, Continuous),
            SONIK_PER_DECK("eq.mid",  EqMid,  Continuous),
            SONIK_PER_DECK("eq.low",  EqLow,  Continuous),

            // ---- Per-deck key / time -------------------------------------
            SONIK_PER_DECK("keyLock.toggle",      KeyLockToggle,     Toggle),
            SONIK_PER_DECK("masterTempo.toggle",  MasterTempoToggle, Toggle),
            SONIK_PER_DECK("keyShift.plus",       KeyShiftPlus,      Momentary),
            SONIK_PER_DECK("keyShift.minus",      KeyShiftMinus,     Momentary),

            // ---- Per-deck jog --------------------------------------------
            SONIK_PER_DECK("jog.scratch", JogScratch, RelativeDelta),
            SONIK_PER_DECK("jog.bend",    JogBend,    RelativeDelta),
            SONIK_PER_DECK("jog.touch",   JogTouch,   Momentary),

            // ---- Per-deck loop -------------------------------------------
            SONIK_PER_DECK("loop.in",          LoopIn,         Momentary),
            SONIK_PER_DECK("loop.out",         LoopOut,        Momentary),
            SONIK_PER_DECK("loop.toggle",      LoopToggle,     Toggle),
            SONIK_PER_DECK("loop.size.halve",  LoopSizeHalve,  Momentary),
            SONIK_PER_DECK("loop.size.double", LoopSizeDouble, Momentary),

            // ---- Per-deck hot cues (8) -----------------------------------
            SONIK_HOTCUE_ROW(1), SONIK_HOTCUE_ROW(2), SONIK_HOTCUE_ROW(3), SONIK_HOTCUE_ROW(4),
            SONIK_HOTCUE_ROW(5), SONIK_HOTCUE_ROW(6), SONIK_HOTCUE_ROW(7), SONIK_HOTCUE_ROW(8),

            // ---- Per-deck beat jump (6 sizes × 2 directions) -------------
            SONIK_BEATJUMP_ROW(1),  SONIK_BEATJUMP_ROW(2),  SONIK_BEATJUMP_ROW(4),
            SONIK_BEATJUMP_ROW(8),  SONIK_BEATJUMP_ROW(16), SONIK_BEATJUMP_ROW(32),
            SONIK_PER_DECK("beatjump.size.cycle", BeatJumpSizeCycle, Momentary),

            // ---- Per-deck quantize / slip --------------------------------
            SONIK_PER_DECK("quantize.toggle", QuantizeToggle, Toggle),
            SONIK_PER_DECK("slip.toggle",     SlipToggle,     Toggle),

            // ---- Per-deck position ---------------------------------------
            SONIK_PER_DECK("position.seek", PositionSeek, Continuous),

            // ---- Global mixer --------------------------------------------
            SONIK_TARGET("mixer.crossfader",       Crossfader,     Continuous, Global, GlobalDeckIndex),
            SONIK_TARGET("mixer.master.gain",      MasterGain,     Continuous, Global, GlobalDeckIndex),
            SONIK_TARGET("mixer.headphones.gain",  HeadphonesGain, Continuous, Global, GlobalDeckIndex),

            // ---- Per-deck headphone cue (mixer namespace) ----------------
            SONIK_TARGET("mixer.deck.A.headphoneCue.toggle", HeadphoneCueToggle, Toggle, Global, 0),
            SONIK_TARGET("mixer.deck.B.headphoneCue.toggle", HeadphoneCueToggle, Toggle, Global, 1),
            SONIK_TARGET("mixer.deck.C.headphoneCue.toggle", HeadphoneCueToggle, Toggle, Global, 2),
            SONIK_TARGET("mixer.deck.D.headphoneCue.toggle", HeadphoneCueToggle, Toggle, Global, 3),

            // ---- Library navigation --------------------------------------
            SONIK_TARGET("library.scroll.up",     LibraryScrollUp,    Momentary,   Global, GlobalDeckIndex),
            SONIK_TARGET("library.scroll.down",   LibraryScrollDown,  Momentary,   Global, GlobalDeckIndex),
            SONIK_TARGET("library.focus.search",  LibraryFocusSearch, Momentary,   Global, GlobalDeckIndex),
            SONIK_TARGET("library.browse",         LibraryBrowse,      Continuous,  Global, GlobalDeckIndex),
            SONIK_TARGET("library.load.deck.A",   LibraryLoadDeck,    Momentary, Global, 0),
            SONIK_TARGET("library.load.deck.B",   LibraryLoadDeck,    Momentary, Global, 1),
            SONIK_TARGET("library.load.deck.C",   LibraryLoadDeck,    Momentary, Global, 2),
            SONIK_TARGET("library.load.deck.D",   LibraryLoadDeck,    Momentary, Global, 3),
        };

        #undef SONIK_BEATJUMP_ROW
        #undef SONIK_HOTCUE_ROW
        #undef SONIK_PER_DECK
        #undef SONIK_TARGET
        // clang-format on

        inline constexpr std::size_t kRegistrySize = sizeof (kRegistry) / sizeof (kRegistry[0]);

        static_assert (kRegistrySize <= static_cast<std::size_t> (InvalidTargetIndex),
                       "Registry exceeds TargetIndex range; bump TargetIndex to uint32_t.");

        // Compile-time sorted index over `kRegistry` for binary-search lookup.
        // Insertion sort: O(N^2) at compile time but N < 300 → cheap.
        inline constexpr std::array<TargetIndex, kRegistrySize> makeSortedIndex()
        {
            std::array<TargetIndex, kRegistrySize> idx {};
            for (std::size_t i = 0; i < kRegistrySize; ++i)
                idx[i] = static_cast<TargetIndex> (i);

            for (std::size_t i = 1; i < kRegistrySize; ++i)
            {
                TargetIndex value = idx[i];
                std::string_view valueId { kRegistry[value].id };
                std::size_t j = i;
                while (j > 0 && std::string_view (kRegistry[idx[j - 1]].id) > valueId)
                {
                    idx[j] = idx[j - 1];
                    --j;
                }
                idx[j] = value;
            }
            return idx;
        }

        inline constexpr std::array<TargetIndex, kRegistrySize> kSortedIndex = makeSortedIndex();

        // Verify uniqueness via the sorted index (adjacent equal ids → duplicate).
        inline constexpr bool registryHasUniqueIds()
        {
            for (std::size_t i = 1; i < kRegistrySize; ++i)
            {
                std::string_view a { kRegistry[kSortedIndex[i - 1]].id };
                std::string_view b { kRegistry[kSortedIndex[i]].id };
                if (a == b)
                    return false;
            }
            return true;
        }

        static_assert (registryHasUniqueIds(),
                       "Duplicate target id detected in ControlTargetRegistry. "
                       "Each id must be unique.");
    } // namespace detail

    //--------------------------------------------------------------------------
    /** Public, allocation-free accessor over the compile-time target table. */
    struct ControlTargetRegistry
    {
        ControlTargetRegistry()  = delete;
        ~ControlTargetRegistry() = delete;

        static constexpr std::size_t size() noexcept { return detail::kRegistrySize; }

        /** O(1) random access. Caller must pass a valid index (< size()). */
        static constexpr const ControlTarget& get (TargetIndex index) noexcept
        {
            return detail::kRegistry[index];
        }

        /** O(log N) binary search over the lex-sorted view. Returns nullopt
            for unknown ids. Performs no heap allocation. */
        static std::optional<TargetIndex> lookup (juce::StringRef id) noexcept
        {
            const auto* raw = id.text.getAddress();
            if (raw == nullptr)
                return std::nullopt;
            const std::string_view query { raw, std::strlen (raw) };

            std::size_t lo = 0;
            std::size_t hi = detail::kRegistrySize;
            while (lo < hi)
            {
                const std::size_t mid = lo + (hi - lo) / 2u;
                const std::string_view candidate { detail::kRegistry[detail::kSortedIndex[mid]].id };
                if (candidate < query)
                    lo = mid + 1u;
                else if (candidate > query)
                    hi = mid;
                else
                    return detail::kSortedIndex[mid];
            }
            return std::nullopt;
        }

        /** Linear scan for a (category, deckIndex) pair. Returns nullopt if
            no row matches. For per-deck rows `deckIndex` is 0..3; for global
            rows pass the row's stored `deckIndex` (or GlobalDeckIndex). Used
            by PRD-0045 to resolve a `MidiMessageEvent` back to a stable
            `TargetIndex` for soft-takeover state lookup. */
        static std::optional<TargetIndex> findByCategoryAndDeck (MidiTargetCategory category,
                                                                 std::uint8_t deckIndex) noexcept
        {
            for (std::size_t i = 0; i < detail::kRegistrySize; ++i)
            {
                const auto& row = detail::kRegistry[i];
                if (row.category == category && row.deckIndex == deckIndex)
                    return static_cast<TargetIndex> (i);
            }
            return std::nullopt;
        }
    };
} // namespace sonik::midi

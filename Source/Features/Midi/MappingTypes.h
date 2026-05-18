#pragma once
//==============================================================================
// PRD-0042: Mapping Data Model.
//
// In-memory shape of a parsed MIDI mapping file. All structs are PODs where
// possible so they can be copied, stored in the resolver hot path, and (for
// `ResolvedBinding`) handed to the bridge with zero allocation.
//
// Two-table design:
//   * `Mapping`        — immutable after parsing. Bindings + modifiers +
//                        precomputed lookup index.
//   * `ResolverState`  — mutable per-device state owned by the caller
//                        (e.g. 14-bit LSB cache). Passed by ref to resolve().
//==============================================================================

#include "MidiTargetCategory.h"
#include "ControlTargetRegistry.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace sonik::midi
{
    //--------------------------------------------------------------------------
    enum class Transform : std::uint8_t
    {
        Momentary,            // Note On vel>0 → 1.0f, anything else → 0.0f
        Toggle,               // Same encoding as Momentary; latch handled downstream
        Linear,               // 7-bit CC: value / 127.0f
        Linear14,             // 14-bit CC pair: (msb<<7 | lsb) / 16383.0f
        SignedBitDelta,       // (value - 64), clamped to [-63, 63]
        TwosComplementDelta,  // value < 64 ? value : value - 128
    };

    enum class SoftTakeoverPolicy : std::uint8_t
    {
        Pickup,    // Default: ignore until physical control crosses model.
        Always,    // Apply regardless of mismatch.
        Never,     // Soft-takeover disabled (alias of Always for symmetry).
    };

    enum class ModifierStyle : std::uint8_t
    {
        Momentary,  // Mask bit set while held, cleared on release.
        Latching,   // Each press XOR-toggles the bit; release is ignored.
    };

    //--------------------------------------------------------------------------
    /** Feedback style — what kind of source value the engine reads and how it
        translates it to an outbound MIDI value byte (PRD-0047). */
    enum class FeedbackStyle : std::uint8_t
    {
        None       = 0, // No feedback configured (midiKey == 0 also implies this).
        Binary     = 1, // Source bool → onVelocity / offVelocity.
        Colour     = 2, // Source int colour-index 0..15 → paletteVelocities[i].
        Continuous = 3, // Source float [0,1] → 0..127 via Linear / LinearInverse curve.
    };

    enum class FeedbackCurve : std::uint8_t
    {
        Linear        = 0, // out = round(value * 127)
        LinearInverse = 1, // out = round((1 - value) * 127)
    };

    //--------------------------------------------------------------------------
    /** Optional feedback wiring. Populated for bindings that can drive an LED
        or motor on the controller. Consumed by PRD-0047. */
    struct BindingFeedback
    {
        // Back-compat sentinel: 0 = no feedback configured (used by serializer +
        // PRD-0044 readers); PRD-0047's engine inspects `style` instead.
        std::uint32_t midiKey;

        // Binary style (back-compat fields).
        std::uint8_t  onValue;
        std::uint8_t  offValue;

        // PRD-0047 extensions.
        FeedbackStyle style;
        FeedbackCurve curve;
        float         blinkHz;                 // For disengaged feedback (0 = solid).
        std::uint8_t  paletteVelocities[16];   // Indexed by colour-index 0..15.
    };
    static_assert (std::is_trivially_copyable_v<BindingFeedback>,
                   "BindingFeedback must be POD; lives on the resolver hot path.");

    //--------------------------------------------------------------------------
    /** A single mapping row, fully resolved against the registry. */
    struct Binding
    {
        TargetIndex          target;               // Resolved at parse time.
        std::uint32_t        midiKey;              // (channel<<16)|(status<<8)|data1
        std::uint8_t         lsbData1;             // 255 = none (only set for Linear14)
        Transform            transform;
        std::uint32_t        requiredModifierMask; // 0 = no modifier required
        SoftTakeoverPolicy   softTakeover;
        BindingFeedback      feedback;
        BindingFeedback      disengagedFeedback;   // PRD-0047: shown while soft-takeover Disengaged.
    };
    static_assert (std::is_trivially_copyable_v<Binding>,
                   "Binding must be POD; lives in Mapping.bindings hot vector.");

    //--------------------------------------------------------------------------
    /** A SHIFT-style layer, identified by a single mask bit. */
    struct Modifier
    {
        std::uint32_t midiKey;
        std::uint8_t  bit;       // 0..31
        ModifierStyle style;
    };
    static_assert (std::is_trivially_copyable_v<Modifier>,
                   "Modifier must be POD; lives in Mapping.modifiers vector.");

    //--------------------------------------------------------------------------
    /** Device-fingerprint matchers. Compiled regexes are stored once on the
        owning `Mapping`; never reallocated on the resolver hot path. */
    struct DeviceMatch
    {
        juce::String manufacturerPattern;
        juce::String productPattern;
        juce::String midiNamePattern;

        // PRD-0051: optional per-physical-USB-port disambiguator matched
        // against `juce::MidiDeviceInfo::identifier`. Exactly one of the two
        // forms is populated when an `identifierHint` is configured:
        //   - `identifierHintLiteral` — exact string equality
        //   - `identifierHintRegexSrc` — ECMAScript regex source (case-insensitive)
        // When both are absent the mapping matches any port (legacy behaviour).
        std::optional<juce::String> identifierHintLiteral;
        std::optional<juce::String> identifierHintRegexSrc;

        // Lazy regex compile cache. shared_ptr so DeviceMatch remains copyable
        // (a Mapping is deep-copied by `Mapping copy = *src;` in createUserCopy).
        // `std::once_flag` guarantees one-shot compilation across all copies of
        // the same DeviceMatch.
        struct RegexCache
        {
            std::once_flag           once;
            std::optional<std::regex> compiled;
        };
        mutable std::shared_ptr<RegexCache> identifierHintRegexCache;

        bool hasIdentifierHint() const noexcept
        {
            return identifierHintLiteral.has_value() || identifierHintRegexSrc.has_value();
        }

        // Returns true iff this DeviceMatch's `identifierHint` (if any)
        // matches `candidate`. When no hint is configured returns `false`
        // (callers should check `hasIdentifierHint()` first to distinguish
        // "no constraint" from "constraint failed").
        bool identifierHintMatches (const juce::String& candidate) const noexcept
        {
            if (identifierHintLiteral.has_value())
                return *identifierHintLiteral == candidate;

            if (! identifierHintRegexSrc.has_value())
                return false;

            if (identifierHintRegexCache == nullptr)
                identifierHintRegexCache = std::make_shared<RegexCache>();

            std::call_once (identifierHintRegexCache->once, [this]() noexcept
            {
                try
                {
                    identifierHintRegexCache->compiled =
                        std::regex (identifierHintRegexSrc->toStdString(),
                                    std::regex::ECMAScript | std::regex::icase);
                }
                catch (const std::regex_error&)
                {
                    identifierHintRegexCache->compiled.reset();
                }
            });

            if (! identifierHintRegexCache->compiled.has_value())
                return false;

            try
            {
                return std::regex_search (candidate.toStdString(),
                                          *identifierHintRegexCache->compiled);
            }
            catch (const std::regex_error&)
            {
                return false;
            }
        }
    };

    //--------------------------------------------------------------------------
    using ModifierMask = std::uint32_t;

    /** Up to four modifier-layered overloads sharing the same MIDI key. */
    inline constexpr std::size_t MaxOverloadsPerMidiKey = 4;

    using BindingBucket = std::array<TargetIndex, MaxOverloadsPerMidiKey>;

    inline constexpr BindingBucket EmptyBindingBucket = {
        InvalidTargetIndex, InvalidTargetIndex, InvalidTargetIndex, InvalidTargetIndex
    };

    //--------------------------------------------------------------------------
    /** Immutable parsed mapping. Owned by PRD-0043's loader. */
    struct Mapping
    {
        int                     schemaVersion { 0 };
        DeviceMatch             deviceMatch;

        // Human-readable name for the Settings → MIDI panel (PRD-0048). Optional;
        // when empty the UI falls back to the mapping's stable id.
        juce::String            displayName;

        std::vector<Binding>    bindings;
        std::vector<Modifier>   modifiers;

        // Modifier id strings indexed by `bit`. `modifierNames[bit]` is the
        // declared id (e.g. "shift") when a modifier occupies that bit, or
        // an empty string otherwise. Filled by MappingParser and read by
        // MappingSerializer + MidiInboundRouter::getModifierBitName.
        std::vector<juce::String> modifierNames;

        // Hash map: midiKey → up to MaxOverloadsPerMidiKey indices into `bindings`.
        // Each bucket entry is the index of a Binding in `bindings`, NOT a TargetIndex
        // (despite reusing the same uint16 type — the resolver dereferences via bindings[]).
        std::unordered_map<std::uint32_t, BindingBucket> bindingIndex;

        // midiKey → modifier index in `modifiers` (modifiers cannot stack on the same key).
        std::unordered_map<std::uint32_t, std::uint16_t> modifierIndex;

        // Fast-path bitmap: data1 byte → "is this used as a Linear14 LSB by some binding?"
        // Used by the resolver to cache LSB values without doing a hashmap lookup
        // per inbound CC.
        std::array<bool, 128> isLsbDataByte {};
    };

    //--------------------------------------------------------------------------
    /** Per-device mutable resolver state. Caller owns; one per active device. */
    struct ResolverState
    {
        // Most-recent LSB CC values for 14-bit pair decoding, indexed by data1.
        // 255 means "no LSB seen yet"; resolver returns nullopt for Linear14 in
        // that case to avoid emitting a bogus value.
        std::array<std::uint8_t, 128> lsbCache;

        ResolverState() noexcept { lsbCache.fill (0xFFu); }

        void reset() noexcept { lsbCache.fill (0xFFu); }
    };

    //--------------------------------------------------------------------------
    /** POD result of `BindingResolver::resolve`. RT-safe to copy. */
    struct ResolvedBinding
    {
        TargetIndex         target;
        MidiTargetCategory  category;
        std::uint8_t        deckIndex;        // 255 for global targets
        float               normalisedValue;  // [0, 1] for continuous, {0, 1} for momentary
        std::int16_t        intDelta;         // For RelativeDelta transforms (and modifier bit)
        SoftTakeoverPolicy  softTakeover;
    };
    static_assert (std::is_trivially_copyable_v<ResolvedBinding>,
                   "ResolvedBinding must be POD; flows through the bridge.");

    //--------------------------------------------------------------------------
    /** Parser diagnostic. Aggregated into ParseResult.errors. */
    struct ValidationError
    {
        enum class Kind : std::uint8_t
        {
            UnsupportedSchemaVersion,
            MalformedRoot,
            UnknownTarget,
            MalformedMidi,
            UnknownTransform,
            UnknownModifierReference,
            DuplicateModifierId,
            ModifierBitOverflow,
            TooManyOverloads,
            UnknownSoftTakeover,
            ModifierTargetConflict,
        };

        Kind          kind;
        juce::String  detail;        // Free-form: the offending id, key, etc.
        juce::String  sourcePath;
        int           bindingIndex;  // -1 if not binding-scoped.
    };

    struct ParseResult
    {
        Mapping                       mapping;
        std::vector<ValidationError>  errors;

        bool ok() const noexcept { return errors.empty(); }
    };

    //--------------------------------------------------------------------------
    /** Pack (channel:1..16, status:0x80..0xF0 family, data1:0..127) into a
        single 32-bit key for hash-map lookups.
        - channel encoded as 1..16 (1-based, matching mapping JSON).
        - status encoded as the canonical status nibble: 0x90 = note,
          0xB0 = cc, 0xE0 = pitchBend (data1 is the LSB; data1 ignored for matching).
    */
    inline constexpr std::uint32_t packMidiKey (std::uint8_t channel1Based,
                                                std::uint8_t statusNibble,
                                                std::uint8_t data1) noexcept
    {
        return (static_cast<std::uint32_t> (channel1Based) << 16)
             | (static_cast<std::uint32_t> (statusNibble)  << 8)
             | (static_cast<std::uint32_t> (data1));
    }
} // namespace sonik::midi

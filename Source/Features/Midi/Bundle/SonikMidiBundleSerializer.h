#pragma once
//==============================================================================
// PRD-0050: bundle (de)serializer + deterministic-hash helper.
//
// Hash contract (MUST remain stable across releases):
//   `sha256OfSortedJson(v)` builds a fresh juce::var tree by:
//     * For every object encountered, copy its NamedValueSet into a NEW
//       DynamicObject, inserting the keys in lex-sorted order. Then recurse
//       into each value.
//     * For arrays, recurse element-wise preserving order.
//     * For primitives, copy as-is.
//   The sorted tree is then serialised via `juce::JSON::toString(v, /*allOnOneLine*/ true)`,
//   the resulting UTF-8 bytes are fed to `juce::SHA256(bytes, length)`, and the
//   64-char lower-case hex hash is returned via `.toHexString()`.
//
// Do NOT change this function's semantics; doing so invalidates every
// existing `.sonikmidi.json` bundle's integrity check.
//==============================================================================

#include "SonikMidiBundle.h"

#include <juce_core/juce_core.h>

#include <variant>

namespace sonik::midi
{
    template <typename T, typename E>
    struct BundleResult
    {
        bool ok { false };
        T    value {};
        E    error {};

        static BundleResult success (T v) { BundleResult r; r.ok = true;  r.value = std::move (v); return r; }
        static BundleResult failure (E e) { BundleResult r; r.ok = false; r.error = std::move (e); return r; }
    };

    struct SonikMidiBundleSerializer
    {
        SonikMidiBundleSerializer()  = delete;
        ~SonikMidiBundleSerializer() = delete;

        /** Assemble a manifest + mapping into a DynamicObject tree ready for
            `juce::JSON::toString`. */
        static juce::var toJson (const SonikMidiBundle&);

        /** Parse a top-level juce::var into a SonikMidiBundle. Validates that
            `manifest` and `mapping` objects exist and that the required
            manifest fields are present. Does NOT verify the sha256. */
        static BundleResult<SonikMidiBundle, BundleParseError> fromJson (const juce::var& root);

        /** Deterministic SHA-256 over the JSON value with recursive key
            sorting. See header comment for the exact contract. */
        static juce::String sha256OfSortedJson (const juce::var&);

        /** Helper: produce a fresh juce::var with object keys sorted
            recursively. Exposed for tests; not used outside serialisation. */
        static juce::var sortedClone (const juce::var&);

        /** Current bundle envelope format version. Bumped only on
            envelope-shape changes (not on mapping-schema changes). */
        static constexpr int kBundleEnvelopeVersion = 1;
    };
}

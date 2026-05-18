#pragma once
//==============================================================================
// PRD-0050: `.sonikmidi.json` portable bundle format.
//
// A self-contained JSON document with two top-level objects:
//   * `manifest` — provenance + integrity metadata for the bundle itself.
//   * `mapping`  — the exact same shape as a v1 user mapping file in
//                  `~/Library/Application Support/Sonik/MidiMappings/`.
//
// The manifest's `sha256` field covers ONLY the `mapping` block, serialised
// with a recursive lexical key sort (see `sha256OfSortedJson`). The manifest
// itself is therefore free to evolve in future versions without breaking the
// hash contract.
//==============================================================================

#include <juce_core/juce_core.h>

namespace sonik::midi
{
    struct BundleManifest
    {
        juce::String appVersion;                  // e.g. "0.1.0"
        int          sonikSchemaVersionAtExport { 0 };
        juce::String exportedAtIso8601;           // ISO 8601 UTC, e.g. "2026-05-12T14:30:00Z"
        juce::String sha256;                      // 64-char lower-case hex over sorted `mapping`
        juce::String exporterDeviceName;          // Optional; empty if not recorded.
    };

    struct SonikMidiBundle
    {
        BundleManifest manifest;
        juce::var      mappingJson;               // The raw `mapping` block (DynamicObject tree).
    };

    /** Stage-2 parse error: the file is JSON but does not match the bundle
        envelope. */
    struct BundleParseError
    {
        juce::String reason;
        juce::String missingField;                // populated when reason == "manifest field missing"
    };
}

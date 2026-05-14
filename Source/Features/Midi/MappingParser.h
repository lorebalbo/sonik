#pragma once
//==============================================================================
// PRD-0042: Mapping JSON parser.
//
// Pure function that materialises a `Mapping` from a parsed `juce::var`.
// Caller (PRD-0043) is responsible for reading the file and calling
// `juce::JSON::parse(...)` first. Partial loads are intentional: a single
// bad binding is recorded as a ValidationError but does NOT abort the parse.
//==============================================================================

#include "MappingTypes.h"

#include <juce_core/juce_core.h>

namespace sonik::midi
{
    struct MappingParser
    {
        MappingParser()  = delete;
        ~MappingParser() = delete;

        /** Parse a JSON tree into a Mapping plus a list of validation errors.
            On success, `errors` is empty. On any failure, the affected binding
            or modifier is excluded from the returned mapping; the rest still
            loads. */
        static ParseResult parse (const juce::var& root, juce::StringRef sourcePath);
    };
} // namespace sonik::midi

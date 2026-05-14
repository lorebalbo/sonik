#pragma once
//==============================================================================
// PRD-0043: Mapping → JSON serializer (inverse of MappingParser).
//==============================================================================

#include "MappingTypes.h"

#include <juce_core/juce_core.h>

namespace sonik::midi
{
    struct MappingSerializer
    {
        MappingSerializer()  = delete;
        ~MappingSerializer() = delete;

        /** Produces a juce::var (DynamicObject tree) that, when written via
            juce::JSON::toString and parsed back via MappingParser::parse,
            yields a Mapping semantically equivalent to the input. */
        static juce::var serialize (const Mapping&);
    };
}

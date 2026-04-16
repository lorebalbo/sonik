#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

/**
 * Canonical key format: 0-23
 *   0 = C major,   1 = C minor
 *   2 = C# major,  3 = C# minor
 *   ...
 *  22 = B major,  23 = B minor
 *
 * -1 = unknown / undetectable
 */
namespace KeyUtils
{
    /** Convert canonical key index (0-23) to Camelot notation (e.g. "8B", "5A"). */
    inline juce::String toCamelot (int canonicalKey)
    {
        if (canonicalKey < 0 || canonicalKey > 23)
            return "--";

        // Camelot major (B) mapping by pitch class: C=8B, C#=3B, D=10B, ...
        static constexpr const char* major[] = {
            "8B", "3B", "10B", "5B", "12B", "7B",
            "2B", "9B", "4B",  "11B", "6B", "1B"
        };

        // Camelot minor (A) mapping by pitch class: C=5A, C#=12A, D=7A, ...
        static constexpr const char* minor[] = {
            "5A", "12A", "7A", "2A", "9A", "4A",
            "11A", "6A", "1A", "8A", "3A", "10A"
        };

        int pitchClass = canonicalKey / 2;
        bool isMajor   = (canonicalKey % 2 == 0);

        return isMajor ? major[pitchClass] : minor[pitchClass];
    }

    /** Convert canonical key index to Open Key notation (e.g. "1d", "6m"). */
    inline juce::String toOpenKey (int canonicalKey)
    {
        if (canonicalKey < 0 || canonicalKey > 23)
            return "--";

        // Open Key major (d) mapping by pitch class
        static constexpr const char* major[] = {
            "1d", "8d", "3d", "10d", "5d", "12d",
            "7d", "2d", "9d", "4d",  "11d", "6d"
        };

        // Open Key minor (m) mapping by pitch class
        static constexpr const char* minor[] = {
            "1m", "8m", "3m", "10m", "5m", "12m",
            "7m", "2m", "9m", "4m",  "11m", "6m"
        };

        int pitchClass = canonicalKey / 2;
        bool isMajor   = (canonicalKey % 2 == 0);

        return isMajor ? major[pitchClass] : minor[pitchClass];
    }

    /** Convert canonical key index to standard musical notation (e.g. "C major", "A minor"). */
    inline juce::String toMusicalNotation (int canonicalKey)
    {
        if (canonicalKey < 0 || canonicalKey > 23)
            return "--";

        static constexpr const char* notes[] = {
            "C", "C#", "D", "D#", "E", "F",
            "F#", "G", "G#", "A", "A#", "B"
        };

        int pitchClass = canonicalKey / 2;
        bool isMajor   = (canonicalKey % 2 == 0);

        return juce::String (notes[pitchClass]) + (isMajor ? " major" : " minor");
    }

    /** Extract Camelot number (1-12) from canonical key index. Returns 0 if invalid. */
    inline int getCamelotNumber (int canonicalKey)
    {
        if (canonicalKey < 0 || canonicalKey > 23)
            return 0;

        int pitchClass = canonicalKey / 2;
        bool isMajor   = (canonicalKey % 2 == 0);

        static constexpr int pitchToCamelotMajor[] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
        static constexpr int pitchToCamelotMinor[] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };

        return isMajor ? pitchToCamelotMajor[pitchClass] : pitchToCamelotMinor[pitchClass];
    }

    /** Get a Camelot color for the canonical key index.
     *  12-hue wheel (30-degree increments), major at full saturation, minor at reduced. */
    inline juce::Colour getCamelotColour (int canonicalKey)
    {
        if (canonicalKey < 0 || canonicalKey > 23)
            return juce::Colours::transparentBlack;

        int camelotNum = getCamelotNumber (canonicalKey);
        bool isMajor   = (canonicalKey % 2 == 0);

        // Hue based on Camelot position (30-degree increments)
        float hue = static_cast<float> ((camelotNum - 1) * 30) / 360.0f;
        float saturation = isMajor ? 0.75f : 0.45f;
        float brightness = 0.85f;

        return juce::Colour::fromHSV (hue, saturation, brightness, 1.0f);
    }

    /** Parse a key string (e.g. "Am", "Cmaj", "C#m", "Gb", "8A", "1B") to canonical index.
     *  Returns -1 on failure. */
    inline int parseKeyString (const juce::String& keyStr)
    {
        if (keyStr.isEmpty())
            return -1;

        auto trimmed = keyStr.trim();

        // Try Camelot notation first (e.g. "8A", "12B")
        if (trimmed.endsWithChar ('A') || trimmed.endsWithChar ('B'))
        {
            auto numPart = trimmed.dropLastCharacters (1);
            if (numPart.containsOnly ("0123456789"))
            {
                int camelotNum = numPart.getIntValue();
                bool isMinor = trimmed.endsWithChar ('A');

                if (camelotNum >= 1 && camelotNum <= 12)
                {
                    // Reverse mapping: Camelot number → pitch class (different for major/minor)
                    static constexpr int camelotToPitchMajor[] = { -1, 11, 6, 1, 8, 3, 10, 5, 0, 7, 2, 9, 4 };
                    static constexpr int camelotToPitchMinor[] = { -1, 8, 3, 10, 5, 0, 7, 2, 9, 4, 11, 6, 1 };

                    int pitchClass = isMinor ? camelotToPitchMinor[camelotNum]
                                             : camelotToPitchMajor[camelotNum];
                    return pitchClass * 2 + (isMinor ? 1 : 0);
                }
            }
        }

        // Try standard notation (e.g. "Am", "C#m", "Cmaj", "Db")
        static constexpr const char* noteNames[] = {
            "C", "C#", "Db", "D", "D#", "Eb", "E", "F",
            "F#", "Gb", "G", "G#", "Ab", "A", "A#", "Bb", "B"
        };
        static constexpr int noteValues[] = {
            0, 1, 1, 2, 3, 3, 4, 5,
            6, 6, 7, 8, 8, 9, 10, 10, 11
        };

        auto upper = trimmed.toUpperCase();

        // Find longest matching note name
        int bestIdx = -1;
        int bestLen = 0;

        for (int i = 0; i < 17; ++i)
        {
            auto noteName = juce::String (noteNames[i]).toUpperCase();
            if (upper.startsWith (noteName) && noteName.length() > bestLen)
            {
                bestIdx = i;
                bestLen = noteName.length();
            }
        }

        if (bestIdx >= 0)
        {
            int pitchClass = noteValues[bestIdx];
            auto suffix = upper.substring (bestLen).trim();

            bool isMinor = false;
            if (suffix.startsWith ("MIN"))
                isMinor = true;
            else if (suffix.startsWith ("M") && ! suffix.startsWith ("MAJ"))
                isMinor = true;

            return pitchClass * 2 + (isMinor ? 1 : 0);
        }

        return -1;
    }

} // namespace KeyUtils

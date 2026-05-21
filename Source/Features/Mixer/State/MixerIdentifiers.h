#pragma once
//==============================================================================
// PRD-0052: Mixer State Schema — identifier constants and dotted-path
// resolver.
//
// Defines every juce::Identifier used in the mixer ValueTree hierarchy and
// the canonical translation between the externally-visible dotted form
// (e.g. "mixer.channel.A.eq.high", used by MIDI mappings, registry,
// logs) and either:
//   (a) a hierarchical ValueTree path  →  { ValueTree child, property Identifier }
//   (b) a MixerMeterSnapshot atomic slot pointer.
//
// Hierarchy (one juce::Identifier per level):
//   SonikState
//     └── Mixer                                ← type identifier "Mixer"
//           ├── channel                        ← type identifier "channel"
//           │     ├── A                        ← type identifier "A"
//           │     │     properties: gain, filter, fader, assignA, assignB
//           │     │     └── eq                 ← type identifier "eq"
//           │     │           properties: high, mid, low,
//           │     │                       killHigh, killMid, killLow
//           │     ├── B  (same shape)
//           │     ├── C  (same shape)
//           │     └── D  (same shape)
//           └── master                         ← type identifier "master"
//                 property: gain
//       Direct properties on Mixer: crossfader
//
// Dotted-path ↔ hierarchical-path examples:
//   "mixer.channel.A.gain"          ↔ Mixer→channel→A   property "gain"
//   "mixer.channel.B.eq.high"       ↔ Mixer→channel→B→eq property "high"
//   "mixer.channel.A.eq.killHigh"   ↔ Mixer→channel→A→eq property "killHigh"
//   "mixer.channel.B.filter"        ↔ Mixer→channel→B   property "filter"
//   "mixer.crossfader"              ↔ Mixer             property "crossfader"
//   "mixer.master.gain"             ↔ Mixer→master      property "gain"
//
// Metering identifiers (resolved to MixerMeterSnapshot, NOT ValueTree):
//   "mixer.channel.A.levelPeakL"    → MixerMeterSnapshot::channels[0].levelPeakL
//   "mixer.channel.A.levelPeakR"    → MixerMeterSnapshot::channels[0].levelPeakR
//   "mixer.channel.A.levelRmsL"     → MixerMeterSnapshot::channels[0].levelRmsL
//   "mixer.channel.A.levelRmsR"     → MixerMeterSnapshot::channels[0].levelRmsR
//   "mixer.channel.A.clip"          → MixerMeterSnapshot::channels[0].clip
//   "mixer.master.levelPeakL" etc.  → MixerMeterSnapshot::master.*
//
// THREADING NOTE
// --------------
// The resolver helpers below are intended for use ONLY on the message
// thread (e.g. by the MIDI inbound router or test code). They walk the
// juce::ValueTree, which is not real-time safe. The audio thread reads
// MixerAtomicSnapshot / MixerMeterSnapshot directly via relaxed atomic
// loads — it never calls the resolver.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>

struct MixerMeterSnapshot;   // forward declared (defined in MixerMeterSnapshot.h)

namespace MixerIDs
{
    #define DECLARE_MIX_ID(name) const juce::Identifier name (#name);

    // ---- Tree type identifiers -------------------------------------------
    DECLARE_MIX_ID (Mixer)
    DECLARE_MIX_ID (channel)
    DECLARE_MIX_ID (master)
    DECLARE_MIX_ID (eq)            // sub-tree under each channel

    // ---- Channel letter identifiers (used as child tree types) ----------
    DECLARE_MIX_ID (A)
    DECLARE_MIX_ID (B)
    DECLARE_MIX_ID (C)
    DECLARE_MIX_ID (D)

    // ---- Per-channel writable properties (top-level of channel) ---------
    DECLARE_MIX_ID (gain)          // float dB, range -inf…+12, default 0.0
    DECLARE_MIX_ID (filter)        // float bipolar [-1,+1], default 0.0
    DECLARE_MIX_ID (fader)         // float [0,1],           default 1.0
    DECLARE_MIX_ID (assignA)       // bool
    DECLARE_MIX_ID (assignB)       // bool
    DECLARE_MIX_ID (cue)           // bool, PFL toggle, default false

    // ---- Per-channel EQ sub-tree properties -----------------------------
    DECLARE_MIX_ID (high)          // float dB, range -inf…+6, default 0.0
    DECLARE_MIX_ID (mid)           // float dB, range -inf…+6, default 0.0
    DECLARE_MIX_ID (low)           // float dB, range -inf…+6, default 0.0
    DECLARE_MIX_ID (killHigh)      // bool, default false
    DECLARE_MIX_ID (killMid)       // bool, default false
    DECLARE_MIX_ID (killLow)       // bool, default false

    // ---- Mixer-level properties -----------------------------------------
    DECLARE_MIX_ID (crossfader)      // float [0,1], default 0.5
    DECLARE_MIX_ID (crossfaderCurve) // string enum "smooth"/"sharp" (PRD-0057),
                                     //   dotted path "mixer.crossfader.curve",
                                     //   default "smooth"

    #undef DECLARE_MIX_ID

    //--------------------------------------------------------------------------
    // Channel index ↔ letter conversions (0=A, 1=B, 2=C, 3=D).
    //--------------------------------------------------------------------------
    inline int letterToChannelIndex (const juce::Identifier& letter) noexcept
    {
        const auto s = letter.toString();
        if (s == "A") return 0;
        if (s == "B") return 1;
        if (s == "C") return 2;
        if (s == "D") return 3;
        return -1;
    }

    inline int letterToChannelIndex (juce::StringRef letter) noexcept
    {
        if (letter == juce::StringRef ("A")) return 0;
        if (letter == juce::StringRef ("B")) return 1;
        if (letter == juce::StringRef ("C")) return 2;
        if (letter == juce::StringRef ("D")) return 3;
        return -1;
    }

    inline const juce::Identifier& channelIndexToIdentifier (int idx) noexcept
    {
        static const juce::Identifier ids[] = { A, B, C, D };
        if (idx >= 0 && idx < 4) return ids[idx];
        return ids[0];
    }

    //==========================================================================
    // Resolver — dotted path → ValueTree property OR meter atomic slot.
    //
    // Message-thread only. See "THREADING NOTE" above.
    //==========================================================================

    /** Result of resolving a dotted path against the mixer ValueTree.
        Both fields are invalid if the path does not denote a writable
        ValueTree property. */
    struct ValueTreePropertyRef
    {
        juce::ValueTree   tree;        ///< Parent ValueTree holding the property.
        juce::Identifier  property {}; ///< Property identifier on that tree.

        bool isValid() const noexcept
        {
            return tree.isValid() && property.toString().isNotEmpty();
        }
    };

    /** Result of resolving a dotted path against the meter snapshot block.
        Exactly one of `floatSlot` / `boolSlot` is non-null when valid. */
    struct MeterSlotRef
    {
        std::atomic<float>* floatSlot { nullptr };
        std::atomic<bool>*  boolSlot  { nullptr };

        bool isValid() const noexcept { return floatSlot != nullptr || boolSlot != nullptr; }
    };

    namespace detail
    {
        /** Splits a dotted path into up to 5 components. Returns the number of
            tokens written; -1 if the path is malformed. Message-thread only
            (allocates). */
        inline int tokenise (juce::StringRef src, juce::String tokens[5]) noexcept
        {
            const juce::String full (src);
            int count = 0;
            int start = 0;
            const int len = full.length();
            for (int i = 0; i <= len; ++i)
            {
                const auto c = (i < len ? full[i] : juce::juce_wchar ('.'));
                if (c == juce::juce_wchar ('.'))
                {
                    if (i == start) return -1;
                    if (count >= 5) return -1;
                    tokens[count++] = full.substring (start, i);
                    start = i + 1;
                }
            }
            return count;
        }

        inline bool isChannelEqProperty (const juce::String& tok) noexcept
        {
            return tok == "high"
                || tok == "mid"
                || tok == "low"
                || tok == "killHigh"
                || tok == "killMid"
                || tok == "killLow";
        }

        inline bool isChannelTopLevelProperty (const juce::String& tok) noexcept
        {
            return tok == "gain"
                || tok == "filter"
                || tok == "fader"
                || tok == "assignA"
                || tok == "assignB"
                || tok == "cue";
        }
    }

    /** Resolve a dotted path (e.g. "mixer.channel.A.eq.high") against the
        supplied `mixerRoot` ValueTree (must be the "Mixer" sub-tree itself).
        Returns an invalid ValueTreePropertyRef if the path denotes a meter
        slot, a property that does not exist, or any unknown identifier.

        Message-thread only. */
    inline ValueTreePropertyRef resolveValueTreeProperty (const juce::ValueTree& mixerRoot,
                                                          juce::StringRef dottedPath) noexcept
    {
        if (! mixerRoot.isValid() || mixerRoot.getType() != Mixer)
            return {};

        juce::String toks[5];
        const int n = detail::tokenise (dottedPath, toks);
        if (n < 2 || toks[0] != "mixer")
            return {};

        // "mixer.crossfader"
        if (n == 2 && toks[1] == "crossfader")
            return { mixerRoot, crossfader };

        // PRD-0057: "mixer.crossfader.curve" — sibling string property on
        // the Mixer tree. The stored ValueTree property name is
        // `crossfaderCurve` (single token, no embedded dot) but the dotted
        // path is the natural "mixer.crossfader.curve" form.
        if (n == 3 && toks[1] == "crossfader" && toks[2] == "curve")
            return { mixerRoot, crossfaderCurve };

        // "mixer.master.gain"
        if (n == 3 && toks[1] == "master"
                   && toks[2] == "gain")
        {
            const auto m = mixerRoot.getChildWithName (master);
            return m.isValid() ? ValueTreePropertyRef { m, gain } : ValueTreePropertyRef {};
        }

        // "mixer.channel.X.*"
        if (toks[1] == "channel" && n >= 4)
        {
            const int chIdx = letterToChannelIndex (juce::StringRef (toks[2].toRawUTF8()));
            if (chIdx < 0) return {};

            const auto channelContainer = mixerRoot.getChildWithName (channel);
            if (! channelContainer.isValid()) return {};
            const auto chTree = channelContainer.getChildWithName (channelIndexToIdentifier (chIdx));
            if (! chTree.isValid()) return {};

            // "mixer.channel.X.<top-level-prop>"
            if (n == 4 && detail::isChannelTopLevelProperty (toks[3]))
            {
                if (toks[3] == "gain")    return { chTree, gain };
                if (toks[3] == "filter")  return { chTree, filter };
                if (toks[3] == "fader")   return { chTree, fader };
                if (toks[3] == "assignA") return { chTree, assignA };
                if (toks[3] == "assignB") return { chTree, assignB };
                if (toks[3] == "cue")     return { chTree, cue };
            }

            // "mixer.channel.X.eq.<band>"
            if (n == 5 && toks[3] == "eq"
                       && detail::isChannelEqProperty (toks[4]))
            {
                const auto eqTree = chTree.getChildWithName (eq);
                if (! eqTree.isValid()) return {};

                if (toks[4] == "high")     return { eqTree, high };
                if (toks[4] == "mid")      return { eqTree, mid };
                if (toks[4] == "low")      return { eqTree, low };
                if (toks[4] == "killHigh") return { eqTree, killHigh };
                if (toks[4] == "killMid")  return { eqTree, killMid };
                if (toks[4] == "killLow")  return { eqTree, killLow };
            }
        }

        return {};
    }

    /** Resolve a dotted path (e.g. "mixer.channel.A.levelPeakL",
        "mixer.master.clip") to a meter snapshot atomic slot.
        Returns an invalid MeterSlotRef if the path denotes a ValueTree
        property or any unknown identifier.

        Message-thread only (caller perspective). The returned pointer
        itself is safe to load/store atomically from any thread once held. */
    MeterSlotRef resolveMeterSlot (MixerMeterSnapshot& meters,
                                    juce::StringRef dottedPath) noexcept;

} // namespace MixerIDs

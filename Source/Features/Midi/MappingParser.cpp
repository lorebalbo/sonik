#include "MappingParser.h"

#include "ControlTargetRegistry.h"
#include "Migrations/MigrationRegistry.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace sonik::midi
{
    namespace
    {
        ValidationError makeError (ValidationError::Kind kind,
                                   const juce::String&   detail,
                                   juce::StringRef       sourcePath,
                                   int                   bindingIdx = -1)
        {
            return ValidationError { kind, detail, juce::String (sourcePath), bindingIdx };
        }

        // Map "note"/"cc"/"pitchBend" to canonical status nibble (0x90/0xB0/0xE0).
        // Returns 0 on unknown.
        std::uint8_t parseStatusNibble (const juce::String& s) noexcept
        {
            if (s == "note")      return 0x90;
            if (s == "cc")        return 0xB0;
            if (s == "pitchBend") return 0xE0;
            return 0;
        }

        bool parseTransform (const juce::String& s, Transform& out) noexcept
        {
            if (s == "momentary")           { out = Transform::Momentary;           return true; }
            if (s == "toggle")              { out = Transform::Toggle;              return true; }
            if (s == "linear")              { out = Transform::Linear;              return true; }
            if (s == "linear14")            { out = Transform::Linear14;            return true; }
            if (s == "signedBitDelta")      { out = Transform::SignedBitDelta;      return true; }
            if (s == "twosComplementDelta") { out = Transform::TwosComplementDelta; return true; }
            return false;
        }

        bool parseSoftTakeover (const juce::String& s, SoftTakeoverPolicy& out) noexcept
        {
            if (s == "pickup") { out = SoftTakeoverPolicy::Pickup; return true; }
            if (s == "always") { out = SoftTakeoverPolicy::Always; return true; }
            if (s == "never")  { out = SoftTakeoverPolicy::Never;  return true; }
            return false;
        }

        bool parseModifierStyle (const juce::String& s, ModifierStyle& out) noexcept
        {
            if (s == "momentary") { out = ModifierStyle::Momentary; return true; }
            if (s == "latching")  { out = ModifierStyle::Latching;  return true; }
            if (s == "toggle")    { out = ModifierStyle::Latching;  return true; } // legacy alias
            return false;
        }

        // Returns true on success; emits ValidationError(MalformedMidi) and returns false otherwise.
        bool parseMidiBlock (const juce::var&            midiVar,
                             std::uint8_t&               channelOut,
                             std::uint8_t&               statusOut,
                             std::uint8_t&               data1Out,
                             std::uint8_t&               lsbData1Out,
                             juce::String&               errorDetail)
        {
            if (! midiVar.isObject())
            {
                errorDetail = "midi block missing or not an object";
                return false;
            }

            const int channel = static_cast<int> (midiVar.getProperty ("channel", -1));
            if (channel < 1 || channel > 16)
            {
                errorDetail = "midi.channel out of range 1..16: " + juce::String (channel);
                return false;
            }

            const auto statusStr = midiVar.getProperty ("status", "").toString();
            const auto status    = parseStatusNibble (statusStr);
            if (status == 0)
            {
                errorDetail = "midi.status unknown: \"" + statusStr + "\"";
                return false;
            }

            // pitchBend has no semantically meaningful data1 in mapping; default to 0.
            int data1 = static_cast<int> (midiVar.getProperty ("data1",
                                                               status == 0xE0 ? 0 : -1));
            if (status != 0xE0 && (data1 < 0 || data1 > 127))
            {
                errorDetail = "midi.data1 out of range 0..127: " + juce::String (data1);
                return false;
            }

            int lsbData1 = static_cast<int> (midiVar.getProperty ("data1Lsb", 255));
            if (lsbData1 != 255 && (lsbData1 < 0 || lsbData1 > 127))
            {
                errorDetail = "midi.data1Lsb out of range 0..127: " + juce::String (lsbData1);
                return false;
            }

            channelOut  = static_cast<std::uint8_t> (channel);
            statusOut   = status;
            data1Out    = static_cast<std::uint8_t> (data1);
            lsbData1Out = static_cast<std::uint8_t> (lsbData1);
            return true;
        }
    } // namespace

    ParseResult MappingParser::parse (const juce::var& root, juce::StringRef sourcePath)
    {
        ParseResult result;
        Mapping&    mapping = result.mapping;

        if (! root.isObject())
        {
            result.errors.push_back (makeError (ValidationError::Kind::MalformedRoot,
                                                "root is not an object", sourcePath));
            return result;
        }

        // ---- schemaVersion ------------------------------------------------
        const int schemaVersion = static_cast<int> (root.getProperty ("schemaVersion", 0));
        if (schemaVersion != kCurrentSchemaVersion)
        {
            result.errors.push_back (makeError (ValidationError::Kind::UnsupportedSchemaVersion,
                                                "got " + juce::String (schemaVersion)
                                                    + ", supported = "
                                                    + juce::String (kCurrentSchemaVersion),
                                                sourcePath));
            return result;
        }
        mapping.schemaVersion = schemaVersion;

        // ---- displayName (PRD-0048, optional) -----------------------------
        mapping.displayName = root.getProperty ("displayName", "").toString();

        // ---- device.match -------------------------------------------------
        if (const auto deviceVar = root.getProperty ("device", juce::var()); deviceVar.isObject())
        {
            mapping.deviceMatch.manufacturerPattern = deviceVar.getProperty ("manufacturer", "").toString();
            mapping.deviceMatch.productPattern      = deviceVar.getProperty ("product",      "").toString();
            if (const auto matchVar = deviceVar.getProperty ("match", juce::var()); matchVar.isObject())
                mapping.deviceMatch.midiNamePattern = matchVar.getProperty ("midiName", "").toString();
        }

        // ---- modifiers ----------------------------------------------------
        std::unordered_map<std::string, std::uint8_t> modifierIdToBit;

        // Pre-scan: collect MIDI keys present in `bindings[]` so we can
        // detect ModifierTargetConflict (PRD-0046) without materialising
        // anything we'd then have to undo.
        std::unordered_set<std::uint32_t> bindingMidiKeys;
        if (const auto bindingsScanVar = root.getProperty ("bindings", juce::var()); bindingsScanVar.isArray())
        {
            const auto* arr = bindingsScanVar.getArray();
            for (int i = 0; i < arr->size(); ++i)
            {
                const auto& bVar = arr->getReference (i);
                if (! bVar.isObject())
                    continue;
                const auto midiVar = bVar.getProperty ("midi", juce::var());
                std::uint8_t ch = 0, st = 0, d1 = 0, lsb = 255;
                juce::String tmp;
                if (parseMidiBlock (midiVar, ch, st, d1, lsb, tmp))
                    bindingMidiKeys.insert (packMidiKey (ch, st, st == 0xE0 ? 0 : d1));
            }
        }

        // MIDI keys that are declared as BOTH a modifier and a binding (per
        // PRD-0046 we drop both sides; tracked here so binding parsing can
        // silently skip the binding too).
        std::unordered_set<std::uint32_t> conflictingMidiKeys;

        if (const auto modsVar = root.getProperty ("modifiers", juce::var()); modsVar.isArray())
        {
            const auto* arr = modsVar.getArray();
            std::uint8_t nextBit = 0;
            for (int i = 0; i < arr->size(); ++i)
            {
                const auto& modVar = arr->getReference (i);
                if (! modVar.isObject())
                    continue;

                const auto idStr = modVar.getProperty ("id", "").toString();
                if (idStr.isEmpty())
                    continue;
                const std::string idKey { idStr.toRawUTF8() };

                if (modifierIdToBit.contains (idKey))
                {
                    result.errors.push_back (makeError (ValidationError::Kind::DuplicateModifierId,
                                                        idStr, sourcePath, i));
                    continue;
                }

                if (nextBit >= 32)
                {
                    result.errors.push_back (makeError (ValidationError::Kind::ModifierBitOverflow,
                                                        "more than 32 modifiers declared",
                                                        sourcePath, i));
                    continue;
                }

                const auto bindingVar = modVar.getProperty ("binding", juce::var());
                std::uint8_t channel = 0, status = 0, data1 = 0, lsbData1 = 255;
                juce::String errDetail;
                if (! parseMidiBlock (bindingVar, channel, status, data1, lsbData1, errDetail))
                {
                    result.errors.push_back (makeError (ValidationError::Kind::MalformedMidi,
                                                        "modifier \"" + idStr + "\": " + errDetail,
                                                        sourcePath, i));
                    continue;
                }

                ModifierStyle style = ModifierStyle::Momentary;
                if (const auto styleStr = bindingVar.getProperty ("style", "momentary").toString();
                    ! parseModifierStyle (styleStr, style))
                {
                    style = ModifierStyle::Momentary;
                }

                const auto packed = packMidiKey (channel, status, status == 0xE0 ? 0 : data1);

                // ModifierTargetConflict: same MIDI key declared as both a
                // modifier and a regular binding → drop both (PRD-0046).
                if (bindingMidiKeys.contains (packed))
                {
                    result.errors.push_back (makeError (ValidationError::Kind::ModifierTargetConflict,
                                                        "midiKey 0x" + juce::String::toHexString ((int) packed)
                                                            + " (modifier \"" + idStr + "\")",
                                                        sourcePath, i));
                    conflictingMidiKeys.insert (packed);
                    continue;
                }

                Modifier mod {};
                mod.midiKey = packed;
                mod.bit     = nextBit;
                mod.style   = style;

                modifierIdToBit[idKey] = nextBit;
                if (mapping.modifierNames.size() <= nextBit)
                    mapping.modifierNames.resize (static_cast<std::size_t> (nextBit) + 1u);
                mapping.modifierNames[nextBit] = idStr;
                const auto modIndex = static_cast<std::uint16_t> (mapping.modifiers.size());
                mapping.modifiers.push_back (mod);
                mapping.modifierIndex.emplace (mod.midiKey, modIndex);
                ++nextBit;
            }
        }

        // ---- bindings -----------------------------------------------------
        if (const auto bindingsVar = root.getProperty ("bindings", juce::var()); bindingsVar.isArray())
        {
            const auto* arr = bindingsVar.getArray();
            for (int i = 0; i < arr->size(); ++i)
            {
                const auto& bVar = arr->getReference (i);
                if (! bVar.isObject())
                    continue;

                // target
                const auto targetStr = bVar.getProperty ("target", "").toString();
                const auto targetIdxOpt = ControlTargetRegistry::lookup (juce::StringRef (targetStr.toRawUTF8()));
                if (! targetIdxOpt.has_value())
                {
                    result.errors.push_back (makeError (ValidationError::Kind::UnknownTarget,
                                                        targetStr, sourcePath, i));
                    continue;
                }

                // midi
                const auto midiVar = bVar.getProperty ("midi", juce::var());
                std::uint8_t channel = 0, status = 0, data1 = 0, lsbData1 = 255;
                juce::String errDetail;
                if (! parseMidiBlock (midiVar, channel, status, data1, lsbData1, errDetail))
                {
                    result.errors.push_back (makeError (ValidationError::Kind::MalformedMidi,
                                                        errDetail, sourcePath, i));
                    continue;
                }

                // Silently skip bindings whose MIDI key collided with a
                // modifier declaration (the modifier side already emitted
                // ModifierTargetConflict for this key).
                {
                    const auto packed = packMidiKey (channel, status, status == 0xE0 ? 0 : data1);
                    if (conflictingMidiKeys.contains (packed))
                        continue;
                }

                // transform
                Transform transform = Transform::Momentary;
                const auto transformStr = bVar.getProperty ("transform", "").toString();
                if (! parseTransform (transformStr, transform))
                {
                    result.errors.push_back (makeError (ValidationError::Kind::UnknownTransform,
                                                        transformStr, sourcePath, i));
                    continue;
                }

                // modifier (optional) — accepts a string id OR an array of ids
                // (all must be held simultaneously; ANDed into the mask).
                std::uint32_t requiredMask = 0;
                bool          unknownModRef = false;
                if (bVar.hasProperty ("modifier"))
                {
                    const auto modVar = bVar.getProperty ("modifier", juce::var());

                    auto applyOne = [&] (const juce::String& ref) -> bool
                    {
                        if (ref.isEmpty())
                            return true;
                        const std::string key { ref.toRawUTF8() };
                        const auto it = modifierIdToBit.find (key);
                        if (it == modifierIdToBit.end())
                        {
                            result.errors.push_back (makeError (ValidationError::Kind::UnknownModifierReference,
                                                                ref, sourcePath, i));
                            return false;
                        }
                        requiredMask |= (1u << it->second);
                        return true;
                    };

                    if (modVar.isArray())
                    {
                        for (const auto& v : *modVar.getArray())
                        {
                            if (! applyOne (v.toString()))
                            {
                                unknownModRef = true;
                                break;
                            }
                        }
                    }
                    else
                    {
                        if (! applyOne (modVar.toString()))
                            unknownModRef = true;
                    }

                    if (unknownModRef)
                        continue;
                }

                // softTakeover (optional)
                SoftTakeoverPolicy soft = SoftTakeoverPolicy::Pickup;
                if (bVar.hasProperty ("softTakeover"))
                {
                    const auto softStr = bVar.getProperty ("softTakeover", "").toString();
                    if (! parseSoftTakeover (softStr, soft))
                    {
                        result.errors.push_back (makeError (ValidationError::Kind::UnknownSoftTakeover,
                                                            softStr, sourcePath, i));
                        continue;
                    }
                }

                // feedback (optional)
                auto parseFeedbackBlock = [&] (const juce::var& fbVar, BindingFeedback& outFb)
                {
                    if (! fbVar.isObject())
                        return;

                    std::uint8_t fbCh = 0, fbStat = 0, fbD1 = 0, fbLsb = 255;
                    juce::String fbErr;
                    if (! parseMidiBlock (fbVar, fbCh, fbStat, fbD1, fbLsb, fbErr))
                        return; // Bad feedback block is non-fatal.

                    // Style (default: binary for back-compat with PRD-0044 schema).
                    auto style = FeedbackStyle::Binary;
                    const auto styleStr = fbVar.getProperty ("style", "binary").toString();
                    if      (styleStr == "binary")     style = FeedbackStyle::Binary;
                    else if (styleStr == "colour"
                          || styleStr == "color")      style = FeedbackStyle::Colour;
                    else if (styleStr == "continuous") style = FeedbackStyle::Continuous;

                    auto curve = FeedbackCurve::Linear;
                    if (fbVar.getProperty ("curve", "linear").toString() == "linearInverse")
                        curve = FeedbackCurve::LinearInverse;

                    outFb.midiKey  = packMidiKey (fbCh, fbStat, fbStat == 0xE0 ? 0 : fbD1);
                    outFb.onValue  = static_cast<std::uint8_t> (
                        static_cast<int> (fbVar.getProperty ("onValue", 127)));
                    outFb.offValue = static_cast<std::uint8_t> (
                        static_cast<int> (fbVar.getProperty ("offValue", 0)));
                    outFb.style    = style;
                    outFb.curve    = curve;
                    outFb.blinkHz  = static_cast<float> (
                        static_cast<double> (fbVar.getProperty ("blinkHz", 0.0)));

                    // Palette: { "0": vel0, "1": vel1, ..., "15": vel15 }.
                    for (int p = 0; p < 16; ++p)
                        outFb.paletteVelocities[p] = 0;

                    if (const auto paletteVar = fbVar.getProperty ("palette", juce::var());
                        paletteVar.isObject())
                    {
                        if (auto* obj = paletteVar.getDynamicObject())
                        {
                            for (const auto& entry : obj->getProperties())
                            {
                                const auto idxStr = entry.name.toString();
                                const int idx = idxStr.getIntValue();
                                if (idx >= 0 && idx < 16)
                                    outFb.paletteVelocities[idx] = static_cast<std::uint8_t> (
                                        std::clamp (static_cast<int> (entry.value), 0, 127));
                            }
                        }
                    }
                };

                BindingFeedback feedback {};
                BindingFeedback disengagedFeedback {};
                parseFeedbackBlock (bVar.getProperty ("feedback", juce::var()),           feedback);
                parseFeedbackBlock (bVar.getProperty ("disengagedFeedback", juce::var()), disengagedFeedback);

                Binding binding {};
                binding.target               = static_cast<TargetIndex> (*targetIdxOpt);
                binding.midiKey              = packMidiKey (channel, status, status == 0xE0 ? 0 : data1);
                binding.lsbData1             = lsbData1;
                binding.transform            = transform;
                binding.requiredModifierMask = requiredMask;
                binding.softTakeover         = soft;
                binding.feedback             = feedback;
                binding.disengagedFeedback   = disengagedFeedback;

                const auto bindingIndex = static_cast<std::uint16_t> (mapping.bindings.size());

                // Insert into bindingIndex with overload chaining.
                BindingBucket bucket = EmptyBindingBucket;
                if (const auto existing = mapping.bindingIndex.find (binding.midiKey);
                    existing != mapping.bindingIndex.end())
                    bucket = existing->second;

                bool inserted = false;
                for (std::size_t slot = 0; slot < MaxOverloadsPerMidiKey; ++slot)
                {
                    if (bucket[slot] == InvalidTargetIndex)
                    {
                        bucket[slot] = bindingIndex;
                        inserted = true;
                        break;
                    }
                }

                if (! inserted)
                {
                    result.errors.push_back (makeError (ValidationError::Kind::TooManyOverloads,
                                                        "midiKey 0x" + juce::String::toHexString ((int) binding.midiKey),
                                                        sourcePath, i));
                    continue;
                }

                mapping.bindings.push_back (binding);
                mapping.bindingIndex[binding.midiKey] = bucket;

                if (transform == Transform::Linear14 && lsbData1 < 128)
                    mapping.isLsbDataByte[lsbData1] = true;
            }
        }

        return result;
    }
} // namespace sonik::midi

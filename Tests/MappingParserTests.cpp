// Tests for PRD-0042: MappingParser.

#include "Features/Midi/MappingParser.h"
#include "Features/Midi/ControlTargetRegistry.h"

#include <juce_core/juce_core.h>

namespace
{
    using namespace sonik::midi;

    juce::var parseJson (const char* json)
    {
        return juce::JSON::parse (juce::String::fromUTF8 (json));
    }

    class MappingParserTests final : public juce::UnitTest
    {
    public:
        MappingParserTests() : juce::UnitTest ("Mapping Parser (PRD-0042)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Reject root that is not an object");
            {
                // Pass a non-object var directly so we don't depend on JUCE's
                // top-level-array JSON acceptance behaviour.
                const juce::var notAnObject (42);
                const auto r = MappingParser::parse (notAnObject, "test");
                expect (! r.errors.empty());
                if (! r.errors.empty())
                    expect (r.errors[0].kind == ValidationError::Kind::MalformedRoot);
                expect (r.mapping.bindings.empty());
            }

            beginTest ("Reject unsupported schema version");
            {
                const auto v = parseJson (R"({"schemaVersion": 99})");
                const auto r = MappingParser::parse (v, "test");
                expectEquals ((int) r.errors.size(), 1);
                expect (r.errors[0].kind == ValidationError::Kind::UnsupportedSchemaVersion);
            }

            beginTest ("Parse a simple valid binding");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "device": { "manufacturer": "Reloop", "product": "Contour CE",
                                "match": { "midiName": "Reloop Contour CE" } },
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "test");
                expect (r.ok(), "expected ok parse");
                expectEquals ((int) r.mapping.bindings.size(), 1);
                expectEquals (r.mapping.schemaVersion, 1);
                expect (r.mapping.deviceMatch.midiNamePattern == "Reloop Contour CE");
                if (! r.mapping.bindings.empty())
                {
                    const auto& b = r.mapping.bindings[0];
                    expect (b.transform == Transform::Momentary);
                    expectEquals ((int) b.midiKey, (int) packMidiKey (1, 0x90, 14));
                }
            }

            beginTest ("Unknown target produces UnknownTarget but parser continues");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "bindings": [
                        { "target": "deck.A.does.not.exist",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" },
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 15 },
                          "transform": "momentary" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x.json");
                expectEquals ((int) r.errors.size(), 1);
                expect (r.errors[0].kind == ValidationError::Kind::UnknownTarget);
                expect (r.errors[0].detail == "deck.A.does.not.exist");
                expectEquals ((int) r.mapping.bindings.size(), 1);
            }

            beginTest ("Malformed midi block excludes binding");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 99, "status": "note", "data1": 14 },
                          "transform": "momentary" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x");
                expectEquals ((int) r.errors.size(), 1);
                expect (r.errors[0].kind == ValidationError::Kind::MalformedMidi);
                expectEquals ((int) r.mapping.bindings.size(), 0);
            }

            beginTest ("Unknown transform excludes binding");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "swirl" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x");
                expectEquals ((int) r.errors.size(), 1);
                expect (r.errors[0].kind == ValidationError::Kind::UnknownTransform);
            }

            beginTest ("Modifier declared and referenced");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift",
                          "binding": { "channel": 1, "status": "note", "data1": 24, "style": "momentary" } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" },
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "shift",
                          "transform": "momentary" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x");
                expect (r.ok());
                expectEquals ((int) r.mapping.modifiers.size(), 1);
                expectEquals ((int) r.mapping.bindings.size(), 2);
                expectEquals ((int) r.mapping.bindings[0].requiredModifierMask, 0);
                expectEquals ((int) r.mapping.bindings[1].requiredModifierMask, 1);
            }

            beginTest ("Unknown modifier reference is reported");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "ghost",
                          "transform": "momentary" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x");
                expectEquals ((int) r.errors.size(), 1);
                expect (r.errors[0].kind == ValidationError::Kind::UnknownModifierReference);
            }

            beginTest ("Five overloads on one MIDI key produce TooManyOverloads");
            {
                // 4 modifier bits → 5 unique masks (none + 4 single-bit)
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "m1", "binding": { "channel": 1, "status": "note", "data1": 100 } },
                        { "id": "m2", "binding": { "channel": 1, "status": "note", "data1": 101 } },
                        { "id": "m3", "binding": { "channel": 1, "status": "note", "data1": 102 } },
                        { "id": "m4", "binding": { "channel": 1, "status": "note", "data1": 103 } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" },
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "m1", "transform": "momentary" },
                        { "target": "deck.A.transport.sync",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "m2", "transform": "momentary" },
                        { "target": "deck.A.loop.in",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "m3", "transform": "momentary" },
                        { "target": "deck.A.loop.out",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "m4", "transform": "momentary" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x");
                bool sawOverflow = false;
                for (const auto& e : r.errors)
                    if (e.kind == ValidationError::Kind::TooManyOverloads)
                        sawOverflow = true;
                expect (sawOverflow);
                expectEquals ((int) r.mapping.bindings.size(), 4);
            }

            beginTest ("Linear14 marks LSB byte for fast-path resolver");
            {
                const auto v = parseJson (R"({
                    "schemaVersion": 1,
                    "bindings": [
                        { "target": "deck.A.pitchFader",
                          "midi": { "channel": 1, "status": "cc", "data1": 9, "data1Lsb": 41 },
                          "transform": "linear14" }
                    ]
                })");
                const auto r = MappingParser::parse (v, "x");
                expect (r.ok());
                expect (r.mapping.isLsbDataByte[41]);
                expect (! r.mapping.isLsbDataByte[42]);
            }
        }
    };

    static MappingParserTests mappingParserTests;
} // namespace

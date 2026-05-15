---
status: Implemented
epic: EPIC-0005
depends-on: [PRD-0040, PRD-0041]
---

# 1. PRD-0042: Control Target Registry and Mapping Data Model

## 1.1. Problem

PRD-0040 delivers raw MIDI bytes; PRD-0041 routes typed events to the right thread. But neither of them knows what a MIDI byte *means* in application terms. A `0x90 0x0E 0x7F` (Note On, note 14, full velocity) from the Behringer DDM4000 could mean "Play Deck A", "Hot Cue 1 on Deck B", or "Cycle pitch range" — the meaning depends on a binding declared by a mapping file.

Without a canonical, stable vocabulary of mappable application commands, every consumer of MIDI events would invent ad-hoc identifiers, mapping files would diverge in spelling and grammar across releases, and persisted bindings would silently break the moment a feature module was renamed or refactored. Worse, there would be no central place to verify that a target ID a mapping file references actually exists in the application — a typo in a community-shared profile could silently render a control inert with no error visible to the user.

The mapping schema itself faces the same risk. A free-form JSON schema with no shared parsing contract would produce inconsistent error handling across PRDs: PRD-0043 (storage) would parse one way, PRD-0048 (MIDI Learn UI) another, PRD-0044 (inbound routing) a third. Schema-version drift would become impossible to manage, and a single malformed binding could crash the loader or, worse, be silently ignored.

DJs who download community-built mapping files for their hardware are directly affected: without a stable target-ID contract and a strict, validating schema, a mapping built against today's Sonik may not work in next month's release, and there will be no visible diagnostic when it fails.

## 1.2. Objective

The system must provide two strictly-defined contracts that every later MIDI PRD consumes: (a) a canonical **Control Target Registry** enumerating every mappable application command with a stable string ID, and (b) a **Mapping Data Model** describing the in-memory shape of a parsed mapping plus a pure resolver function from a `MidiInboundEvent` to a typed `ResolvedBinding`. Specifically:

- The system ensures that every mappable application command is identified by a stable string ID of the form `<domain>.<scope>.<command>[.<index>]`, defined as a single source of truth in `Source/Features/Midi/ControlTargetRegistry.h`.
- The system ensures that each registered target is associated, at compile time, with exactly one `MidiTargetCategory` (defined in PRD-0041), one `TargetValueKind` (`Momentary`, `Toggle`, `Continuous`, `RelativeDelta`), and one `DeckScope` (`Global` or `PerDeck`).
- The system ensures that decks are addressed by absolute ID (`A`, `B`, `C`, `D`), never by focus, with the deck letter encoded in the target ID itself.
- The system ensures that an in-memory `Mapping` struct holds, per device, a list of `Binding` structs and a list of `Modifier` structs, each parsed from the JSON schema defined in EPIC-0005 §1.3.3, with **no `juce::String` or `std::string` allocations** in the resolver hot path.
- The system ensures that a pure function `BindingResolver::resolve(const Mapping&, ResolverState&, const MidiInboundEvent&, ModifierMask) -> std::optional<ResolvedBinding>` returns the matching binding (or none) without allocations, locks, or I/O, suitable for invocation from the MIDI callback thread. The `Mapping` is treated as immutable input; all mutable per-device state (e.g., the 14-bit LSB cache) lives in a separate `ResolverState` struct owned and supplied by the caller.
- The system ensures that the schema is versioned (`schemaVersion: 1`) and that a parser written here is reusable by both PRD-0043 (storage) and PRD-0048 (MIDI Learn UI) without modification.
- The system ensures that unknown target IDs in a parsed mapping produce a `ValidationError` carrying the offending ID, the file path, and the byte offset in the JSON source, returned to the caller (no exceptions, no silent drops).
- The system ensures that the registry lives entirely under `Source/Features/Midi/` and does not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`. The registry is a vocabulary, not a router.

This PRD defines **types, parsing, and resolution**. It does not load files (PRD-0043), does not dispatch resolved bindings to feature modules (PRD-0044), and does not render any UI (PRD-0048).

## 1.3. User Flow

This PRD has no end-user UI. Its consumers are PRD-0043 (which calls `parseMapping(const juce::var&)` to materialise a `Mapping` from JSON), PRD-0044 (which calls `BindingResolver::resolve(...)` on every inbound MIDI event), and PRD-0048 (which constructs `Binding` instances programmatically as the user MIDI-Learns new mappings).

### 1.3.1. Defining a New Target

1. A developer adds a new mappable command to the application (e.g., a "Filter Knob" on Deck A introduced by a future PRD).
2. The developer adds one entry to `ControlTargetRegistry.h`: `REGISTER_TARGET(filter, PerDeck, Continuous, MidiTargetCategory::FilterKnob)`.
3. The macro expands at compile time into a `constexpr ControlTarget` entry with id `"deck.A.filter"` … `"deck.A.D.filter"` (four entries, one per deck letter) plus a stable numeric `TargetIndex` for O(1) lookup. The total registry is a `constexpr std::array<ControlTarget, N>`.
4. The new target is immediately available to mapping files. A user can MIDI-Learn it in PRD-0048's UI on the day the registry entry lands.
5. The developer does **not** need to write a routing handler in PRD-0044 in the same patch; until they do, the resolver still returns a valid `ResolvedBinding`, but the inbound router (PRD-0044) silently no-ops the dispatch. A Debug-only `DBG` warning fires the first time the unhandled target is hit, naming the target for follow-up.

### 1.3.2. Parsing a Mapping File

1. PRD-0043 reads a JSON file from disk and calls `juce::JSON::parse(...)`.
2. PRD-0043 passes the resulting `juce::var` to `MappingParser::parse(const juce::var&, juce::StringRef sourcePath)`.
3. `MappingParser::parse` validates the top-level shape: `schemaVersion` is an integer, `device` is an object with `manufacturer`, `product`, and `match.midiName`, `modifiers` is an array, `bindings` is an array.
4. For each binding object: the parser reads the `target` string and looks it up in the registry via `ControlTargetRegistry::lookup(juce::StringRef) -> std::optional<TargetIndex>`. Unknown targets produce a `ValidationError { kind: UnknownTarget, target, sourcePath, offset }` accumulated in a `ParseResult::errors` vector.
5. For each binding object: the parser reads `midi.channel`, `midi.status` (one of `"note"`, `"cc"`, `"pitchBend"`), `midi.data1`, optionally `midi.data1Lsb` (for `linear14` transforms). It reads `transform` (one of `"momentary"`, `"toggle"`, `"linear"`, `"linear14"`, `"signedBitDelta"`, `"twosComplementDelta"`), `modifier` (optional string id referencing a modifier defined in the same file), `softTakeover` (optional, one of `"pickup"`, `"always"`, `"never"`), and `feedback` (optional object).
6. The parser materialises each valid binding as a `Binding` struct: small POD with no `std::string` — the target id is stored as a `TargetIndex` (16-bit integer), and the device-fingerprint regex is compiled once into a `juce::String` held by the parent `Mapping`.
7. `MappingParser::parse` returns a `ParseResult { Mapping mapping; std::vector<ValidationError> errors; }`. On success, `errors` is empty. On any error, `mapping` is still populated with the valid bindings — partial loads are intentional, so a single typo doesn't break the entire profile.

### 1.3.3. Resolving an Inbound Event

1. PRD-0044's inbound router receives `onMidiInbound(const MidiInboundEvent&)` on the MIDI callback thread (per PRD-0040).
2. The router calls `BindingResolver::resolve(const Mapping& activeMapping, const MidiInboundEvent& event, ModifierMask currentMask) -> std::optional<ResolvedBinding>`.
3. `resolve` performs a hash-table lookup keyed by `(channel, statusByte, data1)` (a 24-bit packed key) over a `juce::HashMap<uint32_t, BindingHandle>` precomputed when the mapping was parsed.
4. On a hit, `resolve` checks whether the binding's required modifier mask matches `currentMask`. If not, it skips to the next binding sharing the same MIDI key (chained via a small `std::array<BindingHandle, 4>` per hash bucket — supports up to 4 modifier-layered overloads per physical control).
5. On a full match, `resolve` applies the `transform`: `Momentary` maps Note On/value > 0 to `1.0f`, Note Off / value 0 to `0.0f`; `Linear` maps CC value to `value / 127.0f`; `Linear14` combines MSB and the most recent LSB into a 14-bit value scaled to `[0, 1]`; `SignedBitDelta` decodes the standard jog format (`value - 64`) into an `int16_t delta`; `TwosComplementDelta` decodes the alternative two's-complement jog format.
6. `resolve` returns a `ResolvedBinding { TargetIndex target; MidiTargetCategory category; uint8_t deckIndex; float normalisedValue; int16_t intDelta; SoftTakeoverPolicy softTakeover; }` — a POD, trivially copyable, no heap.
7. On miss (no binding matches the MIDI key, or all candidates fail the modifier check), `resolve` returns `std::nullopt`.
8. The router uses the returned `ResolvedBinding` to call `MidiMessageBridge::dispatch(...)` (PRD-0041). The bridge's routing table consults `category` and writes to the audio FIFO or schedules a `callAsync` accordingly.

### 1.3.4. Defining a Modifier (SHIFT Layer)

1. A mapping file declares `modifiers: [{ "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24, "type": "modifier" } }]`.
2. `MappingParser::parse` materialises the modifier as a `Modifier { ModifierBit bit; uint32_t midiKey; ModifierStyle style; }`, assigning a unique bit (0–31) per modifier id.
3. Inbound events matching a modifier's `midiKey` are resolved into a `ResolvedBinding` with `category = MidiTargetCategory::ModifierSet` and `intDelta = static_cast<int16_t>(bit)` (the bit index, not a value).
4. PRD-0044 consumes this special resolved binding and updates its per-device `ModifierMask` atomic (`std::atomic<uint32_t>`) accordingly. Subsequent `resolve` calls read the updated mask on the next inbound event.

### 1.3.5. Validation Error Surfacing

1. A user shares a community DDM4000 mapping that references `deck.A.filter` (which does not yet exist in this version of Sonik).
2. PRD-0043 calls `MappingParser::parse`, which returns `ParseResult` containing one `ValidationError { kind: UnknownTarget, target: "deck.A.filter", sourcePath: "...", offset: 1247 }`.
3. PRD-0043's loader logs the error via `DBG` on the Message thread, surfaces it in the future MIDI Learn UI (PRD-0048) as a per-binding warning row, and continues loading the rest of the file.
4. The application launches normally. The unknown binding is inert; every other binding works.

## 1.4. Acceptance Criteria

- [ ] The system defines `Source/Features/Midi/ControlTargetRegistry.h` declaring a `constexpr std::array<ControlTarget, N>` of every mappable application command.
- [ ] The system's `ControlTarget` struct contains: `const char* id`, `MidiTargetCategory category`, `TargetValueKind valueKind`, `DeckScope deckScope`, `uint8_t deckIndex` (255 = global).
- [ ] The system defines `TargetValueKind` as an enum class with values `Momentary`, `Toggle`, `Continuous`, `RelativeDelta`.
- [ ] The system defines `DeckScope` as an enum class with values `Global` and `PerDeck`.
- [ ] The system defines a `TargetIndex` type (16-bit unsigned integer) used as the stable in-memory handle for a target, suitable for storage in `Binding` POD structs.
- [ ] The system defines `ControlTargetRegistry::lookup(juce::StringRef id) -> std::optional<TargetIndex>` performing an O(log N) binary search over a sorted view of the registry (no heap allocation in the lookup).
- [ ] The system defines `ControlTargetRegistry::get(TargetIndex) -> const ControlTarget&` returning the target metadata in O(1).
- [ ] The system registers, at minimum, the following target IDs, each tagged with the appropriate `MidiTargetCategory`, `TargetValueKind`, and `DeckScope`:
  - Per-deck transport: `deck.{A,B,C,D}.transport.play`, `deck.{...}.transport.cue`, `deck.{...}.transport.sync` (Momentary)
  - Per-deck pitch/gain: `deck.{...}.pitchFader` (Continuous), `deck.{...}.pitchRange.cycle` (Momentary), `deck.{...}.gain` (Continuous)
  - Per-deck key/time: `deck.{...}.keyLock.toggle` (Toggle), `deck.{...}.masterTempo.toggle` (Toggle), `deck.{...}.keyShift.plus` (Momentary), `deck.{...}.keyShift.minus` (Momentary)
  - Per-deck jog: `deck.{...}.jog.scratch` (RelativeDelta), `deck.{...}.jog.bend` (RelativeDelta), `deck.{...}.jog.touch` (Momentary)
  - Per-deck loop: `deck.{...}.loop.in` (Momentary), `deck.{...}.loop.out` (Momentary), `deck.{...}.loop.toggle` (Toggle), `deck.{...}.loop.size.halve` (Momentary), `deck.{...}.loop.size.double` (Momentary)
  - Per-deck hot cue (8 per deck): `deck.{...}.hotcue.{1..8}.trigger` (Momentary), `deck.{...}.hotcue.{1..8}.delete` (Momentary)
  - Per-deck beat jump: `deck.{...}.beatjump.{minus,plus}.{1,2,4,8,16,32}` (Momentary), `deck.{...}.beatjump.size.cycle` (Momentary)
  - Per-deck quantize/slip: `deck.{...}.quantize.toggle` (Toggle), `deck.{...}.slip.toggle` (Toggle)
  - Per-deck position: `deck.{...}.position.seek` (Continuous, for needle-drop-style faders)
  - Global mixer (reserved namespace; backing handlers wired in PRD-0044): `mixer.crossfader` (Continuous), `mixer.master.gain` (Continuous), `mixer.headphones.gain` (Continuous), `mixer.deck.{A,B,C,D}.headphoneCue.toggle` (Toggle)
  - Library navigation: `library.scroll.up` (Momentary), `library.scroll.down` (Momentary), `library.load.deck.{A,B,C,D}` (Momentary), `library.focus.search` (Momentary)
- [ ] The system's registry includes a `MidiTargetCategory` value for every registered target that matches the categories defined in PRD-0041's enum.
- [ ] The system defines a `Binding` POD struct with members: `TargetIndex target`, `uint32_t midiKey` (packed `channel << 16 | status << 8 | data1`), `uint8_t lsbData1` (255 = none), `Transform transform`, `uint32_t requiredModifierMask`, `SoftTakeoverPolicy softTakeover`, `BindingFeedback feedback`. `static_assert(std::is_trivially_copyable_v<Binding>)`.
- [ ] The system defines a `Transform` enum class with values `Momentary`, `Toggle`, `Linear`, `Linear14`, `SignedBitDelta`, `TwosComplementDelta`.
- [ ] The system defines a `SoftTakeoverPolicy` enum class with values `Pickup`, `Always`, `Never`.
- [ ] The system defines a `Modifier` POD struct with members: `uint32_t midiKey`, `uint8_t bit` (0–31), `ModifierStyle style` (`Momentary` or `Toggle`). `static_assert(std::is_trivially_copyable_v<Modifier>)`.
- [ ] The system defines a `Mapping` struct holding: `int schemaVersion`, `DeviceMatch deviceMatch`, `std::vector<Binding> bindings`, `std::vector<Modifier> modifiers`, and a `juce::HashMap<uint32_t, std::array<TargetIndex, 4>> bindingIndex` for O(1) MIDI-key lookup with up to 4 modifier-layered overloads per physical control. `Mapping` is treated as immutable after parsing.
- [ ] The system defines a `ResolverState` struct holding mutable per-device resolver state, including a `std::array<uint8_t, 128> lsbCache` for 14-bit CC pairing. `ResolverState` is owned by the caller (one per active device) and passed by mutable reference to `resolve`.
- [ ] The system defines a `DeviceMatch` struct holding the manufacturer regex pattern, product-name regex pattern, and the compiled regexes, allocated once on parse and reused for matching.
- [ ] The system defines `MappingParser::parse(const juce::var& root, juce::StringRef sourcePath) -> ParseResult` where `ParseResult { Mapping mapping; std::vector<ValidationError> errors; }`.
- [ ] The system's parser accepts `schemaVersion: 1` and rejects any higher unknown version with a `ValidationError { kind: UnsupportedSchemaVersion, ... }`.
- [ ] The system's parser produces a `ValidationError { kind: UnknownTarget, target, sourcePath, offset }` for every binding whose `target` field does not resolve via `ControlTargetRegistry::lookup`, and excludes that binding from the returned `Mapping.bindings`.
- [ ] The system's parser produces a `ValidationError` for malformed `midi` blocks (missing channel, channel out of range 1–16, unknown status, data1 out of range 0–127) and excludes the binding.
- [ ] The system's parser produces a `ValidationError` for a `modifier` reference that does not match any declared modifier id in the same file.
- [ ] The system's parser produces a `ValidationError` for any `transform` value not in the defined enum.
- [ ] The system's parser, on encountering any validation error, continues parsing remaining bindings (partial-load behaviour) — never aborts the entire parse on a single bad binding.
- [ ] The system's parser populates `Mapping.bindingIndex` from valid bindings, packing up to 4 modifier-layered overloads per MIDI key into the bucket array. A 5th overload for the same MIDI key produces a `ValidationError { kind: TooManyOverloads, midiKey, ... }`.
- [ ] The system defines `BindingResolver::resolve(const Mapping&, ResolverState&, const MidiInboundEvent&, ModifierMask) -> std::optional<ResolvedBinding>` callable from the MIDI callback thread.
- [ ] The system's `resolve` performs zero heap allocations, zero locks, and zero I/O.
- [ ] The system's `resolve` performs O(1) hash-table lookup followed by O(1) modifier-mask comparison over at most 4 bucket entries.
- [ ] The system's `resolve` correctly applies each `Transform`: `Momentary` maps Note On with velocity > 0 to `1.0f`, all other inputs to `0.0f`; `Toggle` returns the same as `Momentary` (toggle state is handled downstream by PRD-0044); `Linear` returns `value / 127.0f`; `Linear14` requires a prior LSB CC to have been received on `lsbData1` (cached in `ResolverState::lsbCache`) and returns the combined 14-bit value scaled to `[0, 1]`; `SignedBitDelta` returns `intDelta = value - 64` clamped to `[-63, 63]`; `TwosComplementDelta` returns `intDelta = (value < 64) ? value : value - 128`.
- [ ] The system's `resolve` returns a `ResolvedBinding` POD with members: `TargetIndex target`, `MidiTargetCategory category`, `uint8_t deckIndex`, `float normalisedValue`, `int16_t intDelta`, `SoftTakeoverPolicy softTakeover`. `static_assert(std::is_trivially_copyable_v<ResolvedBinding>)`.
- [ ] The system handles modifier bindings as a special category: their `resolve` returns a `ResolvedBinding` with `category == MidiTargetCategory::ModifierSet` (or `ModifierClear` for momentary release / toggle off) and `intDelta = modifierBit`, leaving mask management to PRD-0044.
- [ ] The system defines a `ModifierMask` as a `uint32_t` value type (passed by value to `resolve`).
- [ ] The system lives entirely under `Source/Features/Midi/` and does not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`.
- [ ] The system depends on PRD-0040 (`MidiInboundEvent`) and PRD-0041 (`MidiTargetCategory`, `MidiAudioEvent`, `MidiMessageEvent`) only by `#include`-ing their public headers.
- [ ] The system is covered by `ControlTargetRegistryTests.cpp` and `MappingParserTests.cpp` in `Tests/` verifying: (a) every registered target has a unique, monotonically sortable id; (b) `lookup` returns `nullopt` for unknown ids and the correct index for every registered id; (c) the parser correctly materialises a hand-written DDM4000-style mapping fixture; (d) every defined `Transform` produces the documented output for representative input values; (e) `resolve` returns the correct overload when a SHIFT modifier is active; (f) `static_assert`s for POD triviality compile; (g) a stress test of 1,000,000 `resolve` calls produces zero allocations (validated by a custom allocator hook in the test build).

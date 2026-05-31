---
status: Not Implemented
epic: EPIC-0012
depends-on:
  - PRD-0099
---

# 1. PRD-0100: Audio Exporter — WAV / FLAC / MP3

## 1.1. Problem

PRD-0099's offline render driver advances the EPIC-0010 timeline engine and the
EPIC-0011 automation applier block-by-block, faster than real time, summing the
arrangement into a stream of output buffers. But that driver only produces
in-memory audio blocks; it does not know how to turn them into a file on disk.
The DJ who has finished building a set needs the summed mix written out as a
real, shareable audio file — and not just any file, but one whose **format**,
**sample rate**, **bit depth**, **bitrate/quality**, and **channel layout** they
can choose to match where the mix is going (a lossless archive, a streaming
upload, a USB stick for the club).

The encoding side is non-trivial and format-specific. JUCE ships
`juce::WavAudioFormat` and `juce::FlacAudioFormat` with full **write** support,
so WAV and FLAC are straightforward. MP3 is the problem: JUCE bundles an MP3
**decoder** but **no built-in MP3 encoder**. Exporting MP3 therefore requires a
decision about where the encoder comes from (bundled LAME, a platform encoder,
or dropping MP3 entirely) — a decision that must not leak into the rest of the
export pipeline. The encoder must also be fed by **streaming** the render blocks
through a `juce::AudioFormatWriter` rather than accumulating the whole render in
RAM, since a 90-minute stereo set at 96 kHz / 32-bit float is multiple gigabytes
of PCM.

This PRD owns the component that consumes PRD-0099's render blocks and **encodes
them to a user-chosen file** with all the format/quality/range options. It does
not own the render driver loop itself (PRD-0099) nor the export dialog and
progress UI (PRD-0101); it exposes a clean programmatic API that PRD-0101 will
drive.

## 1.2. Objective

The system provides an `AudioExporter` (in `Source/Features/Daw/Export/`) that
encodes the summed arrangement to a single audio file such that:

- The caller specifies, via an `ExportOptions` value type: target **format**
  (WAV, FLAC, MP3), **sample rate** (44.1 / 48 / 96 kHz), **bit depth**
  (16 / 24 / 32-bit float for WAV; 16 / 24 for FLAC; not applicable to MP3),
  **MP3 bitrate/quality** (e.g. 320 kbps CBR or a VBR quality level), **channel
  layout** (stereo), **export range** (whole arrangement vs a selected region),
  an optional **normalization / peak-limit** setting, and an output **file
  path**.
- The exporter constructs the correct `juce::AudioFormat` for the chosen format,
  obtains a `juce::AudioFormatWriter` for the chosen sample rate / bit depth /
  channel count, and **streams** PRD-0099's render blocks into the writer one
  buffer at a time, never buffering the entire render in RAM.
- The **export range** (whole arrangement or a selected region) is passed
  through to PRD-0099's driver as a start/end sample position pair so the driver
  renders only the requested span; the exporter does not itself seek the
  timeline.
- WAV export uses `juce::WavAudioFormat`; FLAC export uses
  `juce::FlacAudioFormat`; MP3 export goes through a **pluggable encoder
  interface** (`Mp3EncoderBackend`) so the concrete MP3 implementation (LAME,
  platform encoder, or none) is swappable without touching the exporter (see
  §1.5.1).
- When reducing bit depth below the render's internal float precision (e.g.
  32-bit float render → 16-bit WAV), the writer applies the format's standard
  dither; the exporter never silently truncates (see §1.5.4).
- An optional normalization / peak-limit pass is available; when off (the
  default), the export is bit-faithful to the summed render per the EPIC-0012
  determinism guarantee; when on, it applies the documented gain/limit strategy
  (see §1.5.4).
- The exporter runs entirely **off the real-time audio thread** (it is part of
  the offline export path) and reports completion / failure (path conflicts,
  encoder-init failure, disk-full, cancellation) back to its caller as a
  structured result.

## 1.3. Developer / Integration Flow

1. A new `ExportOptions` struct is defined in `Source/Features/Daw/Export/` with
   fields: `Format format` (`Wav`, `Flac`, `Mp3`), `double sampleRate`,
   `int bitDepth` (ignored for MP3), `int mp3BitrateKbps`, `bool mp3Vbr`,
   `ChannelLayout layout` (`Stereo`), `ExportRange range`
   (`{ int64 startSample; int64 endSample; }`, where the whole-arrangement case
   is `startSample = 0`, `endSample = arrangementLengthSamples`),
   `bool normalize`, `float peakCeilingDb`, and `juce::File outputFile`.
2. `AudioExporter` (`AudioExporter.h/.cpp`) exposes
   `ExportResult exportArrangement(const ExportOptions&, OfflineRenderDriver&,
   std::function<void(float)> progress, std::atomic<bool>& cancelRequested)`.
   It (a) validates the options against the per-format capability table
   (§1.5.2), (b) builds the matching `juce::AudioFormat`, (c) creates an
   `AudioFormatWriter` on a `juce::FileOutputStream` for `outputFile`, and (d)
   drives PRD-0099 to pull render blocks, writing each into the writer.
3. The exporter passes `options.range` to PRD-0099's driver so the driver
   advances the playhead only across the requested span. For each block PRD-0099
   yields, the exporter calls `writer->writeFromAudioSampleBuffer(...)` and
   forwards a normalized progress fraction (`renderedSamples / totalSamples`) to
   the `progress` callback.
4. MP3 export is routed through the `Mp3EncoderBackend` interface
   (`encodeBlock` / `finish`). At build-configuration time, exactly one concrete
   backend is registered; if none is available, `Format::Mp3` is reported as
   unsupported by the capability table and `exportArrangement` returns an
   `Unsupported` result without creating a file (see §1.5.1).
5. Bit-depth reduction (32-bit float render → 16/24-bit integer output) is
   delegated to the JUCE writer, which applies the format's standard dithering;
   the exporter sets `writeFromAudioSampleBuffer` up so JUCE performs the
   conversion rather than pre-quantising the float buffer itself (see §1.5.4).
6. If `options.normalize` is true, the exporter performs the documented
   normalization / peak-limit strategy before writing (§1.5.4). Otherwise the
   render blocks are written verbatim (subject only to the format's required
   dither on bit-depth reduction).
7. On `cancelRequested`, the exporter stops pulling blocks, closes and
   **deletes** the partially written file (a partial export is never left as a
   valid-looking file), and returns a `Cancelled` result. On any encoder /
   stream error it returns a `Failed` result with a diagnostic message and
   removes the partial file.
8. A new test file `Tests/AudioExporterTests.cpp` covers: WAV round-trip
   (export a known buffer, read it back, assert sample equality at each
   supported bit depth), FLAC round-trip (lossless equality), MP3 backend
   selection (capability table reflects whether a backend is registered), export
   range honouring (only the requested span is written), streaming (a render
   larger than a configured RAM budget completes without buffering the whole
   thing), and cancellation (partial file removed).

## 1.4. Acceptance Criteria

- [ ] An `ExportOptions` value type exists in `Source/Features/Daw/Export/` with
  fields for format, sample rate, bit depth, MP3 bitrate, MP3 VBR flag, channel
  layout, export range (start/end sample), normalize flag, peak ceiling, and
  output file.
- [ ] `AudioExporter::exportArrangement` encodes WAV output via
  `juce::WavAudioFormat`; a round-trip test exports a known buffer and reads it
  back with sample-exact equality at 16-bit, 24-bit, and 32-bit-float.
- [ ] `AudioExporter::exportArrangement` encodes FLAC output via
  `juce::FlacAudioFormat`; a round-trip test confirms lossless equality at
  16-bit and 24-bit.
- [ ] MP3 export is routed through a pluggable `Mp3EncoderBackend` interface; the
  concrete backend is selected at build-configuration time and the `AudioExporter`
  source contains no direct dependency on any specific MP3 encoder library.
- [ ] When no MP3 backend is registered, the per-format capability table reports
  `Format::Mp3` as unsupported and `exportArrangement` returns an `Unsupported`
  result **without** creating an output file.
- [ ] Render blocks from PRD-0099 are **streamed** into the
  `juce::AudioFormatWriter` one buffer at a time; a test renders a span larger
  than a fixed RAM budget and asserts the export completes without accumulating
  the full PCM in memory.
- [ ] The export range (`startSample` / `endSample`) is passed to PRD-0099's
  driver; a test exports a sub-region and asserts the output length equals
  `endSample - startSample` and the content matches that span of the
  arrangement.
- [ ] Supported sample rates are 44.1, 48, and 96 kHz; supported bit depths are
  16 / 24 / 32-bit-float for WAV and 16 / 24 for FLAC; MP3 ignores bit depth and
  accepts a bitrate (e.g. 320 kbps CBR) or a VBR quality level. Requesting an
  unsupported (format, bit depth) combination returns an `InvalidOptions` result
  before any file is created.
- [ ] Bit-depth reduction from the 32-bit-float render to a 16/24-bit integer
  output applies the JUCE writer's standard dither; the exporter never truncates
  the float buffer itself prior to writing.
- [ ] With `normalize = false` (default), the exported audio is bit-faithful to
  the summed render (subject only to required dither on bit-depth reduction),
  satisfying EPIC-0012 §1.3.3 determinism.
- [ ] With `normalize = true`, the exporter applies the documented normalization
  / peak-limit strategy (§1.5.4) and a test confirms the resulting peak does not
  exceed the configured ceiling.
- [ ] Channel layout is stereo; the writer is created with 2 channels and a
  mono-or-other-layout request returns an `InvalidOptions` result (see §1.5.5).
- [ ] On cancellation (`cancelRequested` set mid-export), the exporter stops,
  closes and deletes the partial output file, and returns a `Cancelled` result.
- [ ] On encoder-init / stream / disk error, the exporter returns a `Failed`
  result with a diagnostic message and removes any partially written file.
- [ ] The exporter runs entirely off the real-time audio thread; no
  `processBlock` path is added or modified by this PRD, and the offline export
  code may freely allocate, take locks, and perform file I/O (it is not
  real-time code).
- [ ] No export dialog or progress UI is added by this PRD; the exporter exposes
  only a programmatic API plus a `progress` callback and a `cancelRequested`
  flag for PRD-0101 to drive.

## 1.5. Grey Areas

### 1.5.1. MP3 Encoder Availability in JUCE

JUCE provides an MP3 **decoder** (`juce::MP3AudioFormat`, read-only) but **no
MP3 encoder**. Options: (a) bundle the LAME encoder and link it (introduces
LAME's LGPL licensing and a build dependency, plus the historical MP3 patent
situation — now expired, so patents are no longer a blocker); (b) use a
platform-native encoder (`AVAudioConverter` / AudioToolbox on macOS, Media
Foundation on Windows) — no third-party dependency but two code paths and
platform-specific quality differences; (c) drop MP3 and ship only WAV + FLAC.

**Resolution:** Define a **pluggable `Mp3EncoderBackend` interface**
(`encodeBlock(const float* const*, int numSamples)` / `finish()`), and keep
`AudioExporter` free of any concrete MP3 dependency. The default bundled backend
is **LAME** (mature, deterministic, identical output cross-platform, MP3 patents
expired, LGPL is compatible with dynamic linking), selected at build-config
time. The interface leaves the door open to a platform-encoder backend later
without touching the exporter, and to a "no MP3 backend" build (capability table
reports MP3 unsupported, WAV + FLAC still ship). This isolates the one genuinely
contentious dependency behind a single seam and keeps the lossless path
unconditional.

### 1.5.2. Supported Sample Rates & Bit Depths Per Format

Each format supports a different matrix: WAV PCM supports 16 / 24-bit integer
and 32-bit float; FLAC supports 16 / 24-bit integer only (no float); MP3 has no
bit depth (it is a perceptual codec parameterised by bitrate/quality). Allowing
the UI to request any combination risks invalid writer construction.

**Resolution:** A single **per-format capability table** is the source of truth:
sample rates `{44100, 48000, 96000}` for all formats; bit depths `{16, 24, 32f}`
for WAV, `{16, 24}` for FLAC, `{n/a}` for MP3; MP3 bitrate set
`{128, 192, 256, 320}` kbps plus a VBR quality scale. `exportArrangement`
validates `ExportOptions` against this table up front and returns
`InvalidOptions` (no file created) for any unsupported combination. PRD-0101's
dialog reads the same table to populate its controls, so the UI can only offer
valid combinations and the exporter's validation is a defensive backstop rather
than the primary gate.

### 1.5.3. Streaming-to-Writer vs Full-Buffer Render

The exporter could (a) ask PRD-0099 for the entire render as one giant buffer
then write it, or (b) stream block-by-block, writing each as it arrives. A
90-minute stereo set at 96 kHz / 32-bit float is several gigabytes of PCM —
infeasible to hold in RAM, and pointless given the writer consumes
incrementally.

**Resolution:** **Stream block-by-block.** The exporter pulls one render block
from PRD-0099, immediately writes it via
`AudioFormatWriter::writeFromAudioSampleBuffer`, updates progress, then pulls the
next — peak memory is one render block plus the writer's internal encode buffer,
independent of arrangement length. This matches EPIC-0012 §1.3.3 ("summing to an
output buffer written via `juce::AudioFormatWriter`") and is the only approach
that scales to long sets. The full-buffer approach is rejected outright.

### 1.5.4. Normalization / Peak-Limiting & Dithering on Bit-Depth Reduction

Two distinct concerns are easy to conflate. **Dithering** is required when
reducing bit depth (32-bit float render → 16/24-bit integer) to avoid truncation
distortion. **Normalization / peak-limiting** is an optional loudness/safety
treatment. EPIC-0012 §1.3.3 mandates "no added dither beyond what the chosen
format requires" and a render "bit-faithful to the sources."

**Resolution:** Dithering is **format-required, not optional**: bit-depth
reduction always goes through the JUCE writer's standard dither (the exporter
does not pre-quantise float buffers), and a 32-bit-float WAV export has no
bit-depth reduction so no dither — fully bit-faithful. Normalization / peak-limit
is **optional and off by default**: with it off, the export is bit-faithful to
the summed render (only required dither applies); with it on, the exporter
computes the render's true peak in a first analysis pass (or applies a
look-ahead brickwall limiter at `peakCeilingDb`) and scales/limits so the output
peak does not exceed the ceiling. Off-by-default preserves the EPIC-0012
determinism guarantee; on is an explicit, opt-in user choice surfaced by
PRD-0101.

### 1.5.5. Channel Layout (Stereo Only?)

The arrangement sums to a stereo bus today. The exporter could expose mono,
stereo, or multichannel layouts, but the engine produces stereo and EPIC-0012
§1.2.2 places stems/multitrack export out of scope.

**Resolution:** **Stereo only** for this PRD. The `ChannelLayout` enum is defined
with a single `Stereo` member so the type exists for future expansion (mono
fold-down, multichannel) without an API break, but the exporter creates a
2-channel writer and returns `InvalidOptions` for any non-stereo request. Mono
and multichannel are deferred to a future Epic alongside the out-of-scope
stems/multitrack export.

### 1.5.6. Export Range Plumbing to PRD-0099

The export range (whole arrangement vs a selected region/loop) must reach
PRD-0099's render driver, which owns playhead advancement. The exporter could
(a) seek the timeline itself before handing off, or (b) pass the range to
PRD-0099 and let the driver bound its own loop.

**Resolution:** **Pass the range to PRD-0099** as an explicit
`{ startSample, endSample }` pair; the driver bounds its block loop to that span
and the exporter never touches the timeline playhead directly. This keeps
playhead/seek ownership entirely in PRD-0099 (single responsibility), makes the
exporter a pure consumer of blocks, and means the whole-arrangement case is just
`{ 0, arrangementLengthSamples }` with no special path. The selected-region
source (loop bounds vs a UI selection) is PRD-0101's concern; the exporter only
receives the resolved sample pair.

### 1.5.7. Clipping / True-Peak Safety

A summed arrangement with automation can exceed 0 dBFS in the float domain. A
32-bit-float export preserves over-0 samples harmlessly, but a 16/24-bit integer
export will hard-clip them, and even a 0 dBFS sample-peak signal can exceed
0 dBTP (true peak) after D/A reconstruction.

**Resolution:** With normalization **off** (default), the exporter writes
verbatim and does **not** silently attenuate — honesty to the source mix takes
priority, and 32-bit-float export is the lossless escape hatch for over-0
content. Inter-sample / true-peak limiting is **only** applied when the user
opts into normalization with a peak ceiling (§1.5.4); a true-peak (oversampled)
limiter mode is a documented future enhancement, not required here. The exporter
does, however, surface a non-fatal **clip-warning flag** in `ExportResult` when
an integer-format export encountered samples that clipped, so PRD-0101 can
inform the DJ after the fact.

### 1.5.8. Metadata Tags in the Exported File

WAV (via BWF/INFO chunks), FLAC (Vorbis comments), and MP3 (ID3) all support
embedded metadata (title, artist, etc.). Whether the exporter writes any is
undecided.

**Resolution:** The exporter accepts an **optional `ExportMetadata`** sub-struct
(title, artist, comment) on `ExportOptions` and, when present, passes it to the
`juce::AudioFormat::createWriterFor` metadata map for WAV and FLAC, and to the
`Mp3EncoderBackend` for ID3 tags. Metadata is **optional and empty by default**
so the lossless/deterministic audio payload is unaffected by its presence or
absence (tags live in container chunks, not the PCM stream). Populating the
fields from project / library data is PRD-0101's responsibility; this PRD only
wires the values through to the encoder when provided.

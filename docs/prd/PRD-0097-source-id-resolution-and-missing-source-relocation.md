---
status: Not Implemented
epic: EPIC-0012
depends-on:
  - PRD-0039
  - PRD-0095
  - PRD-0096
---

# 1. PRD-0097: Source-Id Resolution & Missing-Source Relocation on Open

## 1.1. Problem

A `.soniksession` project (PRD-0095 schema, PRD-0096 open flow) stores clips by **stable source id**, never by embedded audio. Every `DawClip` (PRD-0063) carries a `sourceFileId` that, at save time, pointed at a concrete, readable audio source: a library track row in the EPIC-0004 database, an imported external file (PRD-0098), or a stem-cache artefact (EPIC-0002). Between save and reopen, any of those sources can move, be renamed, be deleted, live on a disconnected external drive, or — for a migrated project copied to another machine — never have existed locally at all. Without a resolution layer that runs **before** playback or render is allowed, the deserialized timeline would reference ids that point nowhere, and the engine would attempt to read audio from a null source the first time the playhead crossed an affected clip.

The DAW makes this worse than the deck case PRD-0039 already solves. A single source is frequently referenced by **many** clips: a DJ chops one track into a dozen timeline regions, each a separate `DawClip` sharing one `sourceFileId`. If that source is missing, naively reusing PRD-0039 per clip would prompt the DJ a dozen times to relocate the same file. The DAW also has source kinds the library has never seen — stem-cache artefacts derived from a parent track (EPIC-0002) — whose "relocation" is conceptually a re-derivation, not a file pick. And unlike a live deck load, a session open is a batch operation: the DJ wants to know up front how many sources are unresolved, fix them in one focused pass, and still be allowed to open and inspect the arrangement even if a few sources stay broken. The session must reconstruct fully (no clip silently dropped, per EPIC-0012 §1.3.2), flag unresolved clips visibly, and refuse to render until every referenced source resolves.

## 1.2. Objective

On opening a session (PRD-0096), the system resolves every clip's `sourceFileId` to a real, readable audio source before playback or offline render is permitted, such that:

- Each distinct `sourceFileId` in the deserialized `daw` model is resolved exactly once through an ordered strategy: library DB lookup by id (EPIC-0004), then the source's stored last-known path (PRD-0095 relocation hint), then content-hash match against the library. The first strategy that yields a readable file wins; the resolved path is bound to that source for the session.
- A source that resolves is marked `Resolved`; a source that no strategy can satisfy is marked `Missing`, and **every** clip sharing that `sourceFileId` is flagged together as missing — resolution operates per **source**, not per clip.
- Missing sources surface through a **session-scoped batch resolution** step that reuses PRD-0039's relocation machinery (FileChooser, dedup check, canonical-path write) but presents one entry per missing **source**, not one prompt per clip. Relocating a source once applies the new path to all clips that reference it (identical-source batch relocation).
- The three source kinds resolve through kind-appropriate paths: library-track sources resolve through the EPIC-0004 DB; imported-external-file sources (PRD-0098) resolve through their stored path then library hash; stem-cache sources (EPIC-0002) resolve by checking the stem cache for the parent id, offering **re-derivation** (re-run separation) rather than a file pick when the cache artefact is gone but the parent track still resolves.
- Session open is **non-blocking on missing sources** (partial open): the arrangement deserializes and displays in full, unresolved clips render with the DESIGN.md "Glitch" dithered treatment, and the DJ may inspect, edit, and relocate at leisure.
- Playback and **offline render/export (PRD-0099–0101) are gated**: any attempt to play or export while one or more referenced sources are `Missing` is blocked, with the affected clips highlighted and the batch resolution step offered.
- Resolution runs entirely off the real-time audio thread; the audio engine never sees a `Missing` source because the EPIC-0010 arrangement snapshot is only (re)compiled for clips whose sources are `Resolved`.

## 1.3. User Flow

### 1.3.1. Resolution Pass on Open

1. PRD-0096 deserializes the `.soniksession` into the `daw` `ValueTree` and, before triggering the EPIC-0010 arrangement-snapshot recompile, invokes the `SourceIdResolver`.
2. The resolver collects the **set of distinct `sourceFileId` values** referenced by all clips (deduplicating the many-clips-one-source case) and, for each, records the source kind (`LibraryTrack`, `ExternalFile`, `StemCache`) and the stored last-known path hint from the project.
3. For each distinct source, the resolver applies the ordered strategy (library DB id lookup → stored path `existsAsFile()` → content-hash library match), stopping at the first readable result, and records the resolved absolute path against the source.
4. Sources that resolve are marked `Resolved`; the resolver binds the path so the EPIC-0010 snapshot compile can read it. Sources that exhaust all strategies are marked `Missing`.
5. Every clip is tagged with its source's resolution state. Clips whose source is `Missing` are preserved in the model (never dropped) and flagged for the "Glitch" visual treatment (DESIGN.md).
6. The EPIC-0010 arrangement snapshot is compiled including only `Resolved`-source clips; `Missing`-source clips are excluded from the snapshot until relocated, so the engine never references an unreadable source.
7. The session window opens fully. If any source is `Missing`, a session-scoped "Unresolved Sources" banner shows the count and offers the batch resolution step.

### 1.3.2. Batch Resolution Step

1. The DJ opens the "Unresolved Sources" step (from the banner, or a menu item). It lists one entry per **missing source**, showing the source's display name, kind, broken last-known path, and the count of clips that reference it.
2. For a `LibraryTrack` or `ExternalFile` source, the DJ chooses "Relocate…", which opens PRD-0039's `juce::FileChooser` ("Choose replacement file", no format filter). On selection, PRD-0039's canonical-path dedup check runs; on success the new path is bound to the source and applied to **all** referencing clips at once.
3. For a `StemCache` source, the DJ is offered "Re-derive Stems" when the parent track resolves but the cached artefact is gone: this re-runs EPIC-0002 separation for the parent and rebinds the source to the regenerated cache. If the parent track itself is missing, the entry falls back to a parent-track relocate first.
4. On a successful relocation or re-derivation, the source flips to `Resolved`, all its clips lose the "Glitch" treatment, the affected clips are added to the EPIC-0010 snapshot, and the unresolved count decrements.
5. The DJ may resolve sources in any order and may close the step with sources still unresolved; the session stays open (partial-open), with playback and export gated until the count reaches zero.

### 1.3.3. Render / Playback Gating

1. The DJ presses Play, or initiates an export (PRD-0099–0101), while one or more sources are `Missing`.
2. The action is blocked. A modal explains that unresolved sources must be relocated first, highlights the affected clips in the timeline, and offers a shortcut to the batch resolution step.
3. Once every referenced source is `Resolved`, playback and export proceed normally with no further prompts.

## 1.4. Acceptance Criteria

- [ ] On opening a session, the `SourceIdResolver` collects the set of **distinct** `sourceFileId` values across all clips and resolves each source exactly once, regardless of how many clips reference it.
- [ ] Each source resolves through the ordered strategy: (1) library DB id lookup (EPIC-0004), (2) stored last-known path `juce::File(path).existsAsFile()`, (3) content-hash match against the library; the first readable result is bound, and the strategy stops there.
- [ ] A source for which every strategy fails is marked `Missing`, and **every** clip sharing that `sourceFileId` is flagged missing together; no clip is dropped from the model.
- [ ] `LibraryTrack` sources resolve through the EPIC-0004 database; `ExternalFile` sources (PRD-0098) resolve through stored path then library hash; `StemCache` sources (EPIC-0002) resolve by checking the stem cache for the parent id.
- [ ] The resolution pass runs before the EPIC-0010 arrangement-snapshot recompile; the snapshot is compiled including only clips whose source is `Resolved`, so `Missing`-source clips are excluded from the engine until relocated.
- [ ] Session open is non-blocking: the arrangement deserializes and displays fully even when sources are `Missing`, and unresolved clips render with the DESIGN.md "Glitch" dithered pattern (strictly monochrome, zero `border-radius`).
- [ ] The batch resolution step lists exactly one entry per **missing source** (not per clip), each showing display name, source kind, broken last-known path, and the count of referencing clips.
- [ ] Relocating a `LibraryTrack` or `ExternalFile` source reuses PRD-0039's `juce::FileChooser` and canonical-path dedup check; on success the new path is applied to **all** clips referencing that source in a single operation.
- [ ] A `StemCache` source whose cache artefact is gone but whose parent track resolves offers "Re-derive Stems", which re-runs EPIC-0002 separation for the parent and rebinds the source to the regenerated cache.
- [ ] On a successful relocate or re-derive, the source flips to `Resolved`, its clips lose the "Glitch" treatment, the clips are added to the EPIC-0010 snapshot, and the unresolved-source count decrements.
- [ ] Playback is blocked while any referenced source is `Missing`; the block highlights the affected clips and offers the batch resolution step.
- [ ] Offline render / export (PRD-0099–0101) is blocked while any referenced source is `Missing`; once all sources are `Resolved`, export proceeds with no further prompts.
- [ ] All resolution, relocation, and re-derivation work runs off the real-time audio thread; no resolver code path allocates, locks, or performs I/O on the audio thread, and the engine communicates cross-thread only via the existing EPIC-0010 snapshot mechanism.
- [ ] The resolver preserves all per-clip data (crop, timeline position, gain, automation) across relocation; only the source's bound path changes, exactly as PRD-0039 preserves analysis data on a library relocate.

## 1.5. Grey Areas

### 1.5.1. Id-to-Path Resolution Order

A source id could be resolved by library DB lookup, by the stored last-known path, or by a content-hash match — and these can disagree (the DB row may point at a stale path while the stored hint still exists on disk, or vice versa).

**Resolution:** Resolve in the order **library DB id lookup → stored path → content hash**, first readable result wins. The library DB is the authoritative, actively-maintained source of truth (PRD-0039's startup pass keeps `file_path` current and `is_missing` accurate), so it is tried first. The stored last-known path is a cheap fallback for `ExternalFile` sources that were never in the library and for projects opened before a library scan completes. Content-hash matching is last because it is the most expensive (it scans library rows) and is the right tool only when a file genuinely moved and the DB has not yet caught up. Stopping at the first readable hit avoids redundant work and guarantees deterministic resolution.

### 1.5.2. Reuse of PRD-0039 Prompt vs Session-Specific Batch Dialog

PRD-0039 ships a per-track Missing File Dialog tuned for the library and load-to-deck. The DAW could reuse that dialog verbatim or build a session-specific batch dialog.

**Resolution:** Reuse PRD-0039's **machinery** (FileChooser, canonical-path dedup check, path-write semantics, DESIGN.md styling) but present it through a **session-scoped batch step** that lists one entry per missing source. Reusing the underlying relocation primitives avoids duplicating dedup and path-canonicalisation logic and keeps a single code path for "find a replacement file." But the per-track modal is wrong for a batch open where one source feeds many clips and three source kinds exist; the batch step is the DAW-appropriate surface. This honours EPIC-0012's explicit "reuse PRD-0039's relocation flow" directive without inheriting its single-track UX.

### 1.5.3. Auto-Relocate by Searching Sibling Files

When a source's stored path is broken, the resolver could automatically search the same folder (or the folder of an already-resolved sibling source) for a file of the same name before prompting the DJ.

**Resolution:** Attempt automatic resolution **only** through the three deterministic strategies (DB, stored path, content hash); do **not** add a speculative same-folder filename search in this PRD. The content-hash strategy already catches the common "file moved but is still in the library" case correctly and safely (it matches on content, not on a guessable name). A filename-based sibling search risks binding the wrong file (two different tracks named `track01.wav`), which is a silent correctness hazard in a render pipeline. If a future PRD wants a "scan this folder for replacements" convenience, it can add it as an explicit, DJ-initiated action in the batch step, not as an automatic guess.

### 1.5.4. Partial-Open: Allow Opening With Missing Sources

The session could refuse to open until every source resolves (blocking open), or open fully and flag unresolved clips (partial open).

**Resolution:** **Partial open.** The arrangement always deserializes and displays in full, with unresolved clips preserved and flagged via the "Glitch" treatment. Refusing to open would strand a DJ whose external drive is merely disconnected, or who copied a project to a new machine to inspect it before relinking. Partial open matches EPIC-0012 §1.3.2 ("Unresolved clips are preserved, never silently dropped") and PRD-0039's philosophy of surfacing missing state non-destructively. The cost — that a partially-resolved session cannot play or export — is handled by the gating in §1.5.7, which is the correct place to enforce completeness.

### 1.5.5. Stem-Cache Source: Regeneration vs Relocate

A `StemCache` source (EPIC-0002) is a derived artefact, not a user-managed file. When its cache entry is gone, "relocating" to an arbitrary file is meaningless.

**Resolution:** For `StemCache` sources, offer **re-derivation** (re-run EPIC-0002 separation for the parent track) rather than a file pick, **provided the parent track resolves**. If the parent track is itself missing, the entry first requires relocating the parent (a normal `LibraryTrack`/`ExternalFile` relocate), after which re-derivation becomes available. This is the only coherent recovery for a derived source: the cache is reproducible from the parent, and pointing it at a hand-picked file would break the contract that the stem source is the separated output of a specific parent. Re-derivation runs on EPIC-0002's existing background separation thread with its progress/cancel UI.

### 1.5.6. Identical-Source Batch Relocation

One source is commonly referenced by many clips. Relocating could update only the clip the DJ clicked, or every clip sharing the source.

**Resolution:** Relocation operates on the **source**, so a single relocate updates **all** clips referencing that `sourceFileId` at once. Resolution and relocation are keyed by source id throughout — the resolver deduplicates to distinct sources, the batch step lists sources, and a relocate binds a path to a source. Updating per clip would force the DJ to repeat the same file pick once per chopped region, which is absurd for a track sliced into a dozen clips. Source-keyed relocation is both the correct mental model ("this audio file moved") and the only humane UX.

### 1.5.7. Blocking vs Non-Blocking Resolution & Render Gating

Resolution affects two independent decisions: whether **open** blocks (covered in §1.5.4) and whether **playback/render** is gated on full resolution.

**Resolution:** Open is non-blocking (§1.5.4); **playback and offline render/export are hard-gated** on zero `Missing` sources. The split is deliberate: opening to inspect and relink is a safe, read-only-ish activity that should never be obstructed, but **rendering an export with silently-missing audio would produce a corrupt deliverable** (gaps where unresolved clips should sound). Gating render guarantees the offline driver (PRD-0099) only ever runs against a fully-resolved arrangement, preserving EPIC-0012 §1.3.3's "bit-faithful to the sources" promise. Playback is gated for the same reason and for consistency: the DJ should not hear a misleading silent gap and assume the clip is empty. The gate is enforced at the play/export entry points, highlights the offending clips, and routes the DJ straight to the batch resolution step.

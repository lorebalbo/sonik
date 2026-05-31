---
status: Not Implemented
epic: EPIC-0008
depends-on:
  - PRD-0062
  - PRD-0063
  - PRD-0066
---

# 1. PRD-0067: Channel-Group & Three-Lane Layout

## 1.1. Problem

The DAW panel shell and horizontal time ruler exist (PRD-0066): there is a docked,
collapsible organism at the top of the application with a bars/beats ruler driven by
the master grid. But the panel's body is empty. Nothing in it yet represents the decks.
EPIC-0008 §1.1 and §1.3.3 are explicit that the DAW's vertical structure is a stack of
**per-deck channel groups**, each containing **three horizontal lanes** — `Original`,
`Instrumental`, `Vocal` — that map one-to-one onto the three playable sources EPIC-0002
exposes per deck (the original file plus the two stems). Until that vertical scaffold
exists, there is nowhere for clips (PRD-0068) or the live projection (PRD-0069) to be
drawn, and no spatial relationship between a deck and "its" region of the timeline.

The arrangement data model from PRD-0063 already publishes this structure: the `daw`
ValueTree branch holds `daw.tracks[]` (one per active deck channel group) and, beneath
each, `daw.tracks[i].lanes[]` (the three source lanes). PRD-0062 (the stem-source toggle,
EPIC-0002) defines the *source-mode → audible-lane* contract that decides which lanes
carry audio. What is missing is the **view layer** that observes that tree via JUCE
Listeners and renders, for each active deck, a group header plus three vertically stacked
lane rows with all-caps `Space Mono` lane headers, distinguished by tonal layering per
`DESIGN.md` — and that grows, shrinks, and reorders these groups as decks load and eject
tracks. When more decks/lanes exist than fit vertically, the panel must scroll and/or
allow group/lane collapse (EPIC-0008 §1.2.1 "Vertical scroll / lane collapse").

This PRD builds *only* the lane-and-header scaffold. It deliberately draws no clip
content (PRD-0068) and performs no live projection growth (PRD-0069). It reads — but does
not itself act on, beyond lane labelling/visibility — the source-mode→lane mapping
contract from PRD-0062.

## 1.2. Objective

The system renders, inside the DAW panel body (PRD-0066), a vertical stack of per-deck
**channel groups** such that:

- For each active deck (1–4) that has a corresponding `daw.tracks[i]` node, exactly one
  channel group is rendered: a **group header** (the deck label, all-caps `Space Mono`)
  followed by **three stacked horizontal lanes** in fixed order — `Original`,
  `Instrumental`, `Vocal`.
- Each lane is a `LaneView` molecule (EPIC-0008 §1.3.6) consisting of a **lane header**
  (all-caps `Space Mono` label: `ORIGINAL` / `INSTRUMENTAL` / `VOCAL`) and an empty lane
  **content row** whose horizontal extent aligns with the PRD-0066 ruler's time axis. The
  content row draws no clips in this PRD — it is the surface PRD-0068 paints onto.
- The three lanes are visually distinguished by **tonal layering** (dither density /
  tonal step), never by colour, per `DESIGN.md` §1.3.7.
- The view observes `daw.tracks[]` and `daw.tracks[i].lanes[]` (PRD-0063) via JUCE
  Listeners (Observer pattern). A channel group **appears** when a deck loads a track (its
  `daw.tracks[i]` node is added) and **disappears** when the deck ejects (the node is
  removed). No polling of deck or audio-thread state occurs.
- Lane visibility/labelling reflects PRD-0062's source-mode→lane mapping read from deck
  state (original → `Original`; both stems audible → `Instrumental` + `Vocal`; one stem
  muted → only the audible stem lane). The exact "hide vs dim unaudible lanes" policy is
  resolved in §1.5.1.
- The stack supports **vertical scroll** when the total height of all groups × lanes
  exceeds the panel body height, and supports **collapse** of a group (and/or individual
  lanes) to reclaim vertical space (granularity resolved in §1.5.2).
- Channel groups are ordered by **deck id** (Deck 1 top → Deck 4 bottom), stable across
  load/eject churn (§1.5.4).
- The horizontal lane content surface shares the PRD-0066 ruler's coordinate transform
  (PRD-0065) so that a future clip at timeline sample `S` lands at the same x as the
  ruler tick for `S`; vertical layout (lane heights, header widths) is owned by this PRD.

This PRD adds **no DSP and no audio-thread code**. All rendering and model observation
happen on the message/UI thread (EPIC-0008 §1.3.8); the only cross-thread contact is the
existing read of deck source-mode state already surfaced into the ValueTree by PRD-0062.

## 1.3. User Flow

1. The DJ expands the DAW panel (PRD-0066). With no tracks loaded, the panel body is
   empty below the ruler — no channel groups exist because no `daw.tracks[]` nodes exist.
2. The DJ loads a track onto Deck 1. PRD-0063's model adds a `daw.tracks[0]` node with its
   three `lanes[]`. This PRD's Listener fires and a channel group materialises: a group
   header reading `DECK 1` (all-caps `Space Mono`), then three stacked lanes labelled
   `ORIGINAL`, `INSTRUMENTAL`, `VOCAL`, each an empty row spanning the timeline width.
3. The three lanes are tonally layered — e.g. `Original` at the base tone, `Instrumental`
   one dither step deeper, `Vocal` a further step — so the eye separates them without any
   colour. No clips are drawn (that is PRD-0068).
4. The DJ loads Deck 2. A second channel group (`DECK 2`) appears directly below Deck 1's,
   maintaining deck-id order. Loading Decks 3 and 4 stacks two more groups.
5. With all four decks loaded (4 groups × 3 lanes = 12 lanes) the stack exceeds the panel
   body height. A vertical scrollbar appears (DESIGN.md-compliant) and the DJ scrolls to
   reach lower groups; the ruler stays pinned at top, the horizontal axis unaffected.
6. The DJ collapses Deck 1's group via its header affordance. The group's three lanes
   fold away leaving just the `DECK 1` header row; the groups below slide up to reclaim
   the space. Expanding restores the three lanes.
7. The DJ toggles Deck 2 from original to stems mode (PRD-0062). The group's lane set
   updates per the resolved §1.5.1 policy: the `Original` lane becomes inactive (hidden or
   dimmed) and `Instrumental` + `Vocal` become the active lanes. Muting one stem leaves
   only the audible stem lane active. No clip is moved or drawn — only lane
   activeness/visibility changes.
8. The DJ ejects Deck 1's track. PRD-0063 removes `daw.tracks[0]`; the Listener fires and
   the `DECK 1` group disappears; remaining groups re-pack upward in deck-id order.

## 1.4. Acceptance Criteria

- [ ] A `ChannelGroup` organism (or `ChannelGroupView`) renders one channel group per
  `daw.tracks[i]` node, composed of a `ChannelGroupHeader` molecule plus three `LaneView`
  molecules in fixed order `Original`, `Instrumental`, `Vocal`.
- [ ] Channel groups are created/destroyed strictly in response to `daw.tracks[]` child
  add/remove notifications observed via a JUCE `ValueTree::Listener`; no polling of deck
  or audio-thread state occurs anywhere in this PRD's code.
- [ ] Each lane renders a `LaneView` with a lane header showing the all-caps `Space Mono`
  label `ORIGINAL`, `INSTRUMENTAL`, or `VOCAL`, and an empty content row that draws no
  clip content (clip rendering is PRD-0068).
- [ ] The three lanes within a group are distinguished by **tonal layering** (dither
  density / tonal step per `DESIGN.md`), not colour; no hue is introduced.
- [ ] The group header shows the deck label in all-caps `Space Mono` (e.g. `DECK 1`).
- [ ] Channel groups are ordered top-to-bottom by deck id (Deck 1 → Deck 4) and this
  order is stable across load/eject churn regardless of `daw.tracks[]` insertion order.
- [ ] Loading a track on a deck makes its channel group appear; ejecting makes it
  disappear; the remaining groups re-pack vertically without gaps.
- [ ] Lane activeness/visibility reflects PRD-0062's source-mode→lane mapping read from
  deck state: original → `Original` active; both stems → `Instrumental` + `Vocal` active;
  one stem muted → only the audible stem lane active (presentation per §1.5.1).
- [ ] When total content height (groups × visible lanes) exceeds the DAW panel body
  height, a DESIGN.md-compliant vertical scrollbar appears and scrolls the group stack;
  the PRD-0066 ruler stays pinned and the horizontal time axis is unaffected by vertical
  scroll.
- [ ] A group (and/or individual lane, per §1.5.2) can be collapsed and expanded; collapse
  reclaims vertical space and the stack re-packs; the collapse state is per-group UI state
  (persistence is out of scope here).
- [ ] Each lane's content surface uses the PRD-0065 coordinate transform shared with the
  PRD-0066 ruler so its horizontal origin and scale match the ruler exactly (a sample `S`
  maps to the same x in lane and ruler); lane vertical geometry is owned by this PRD.
- [ ] Lane and group vertical heights honour `DESIGN.md` (2-px solid borders, zero
  border-radius, monochrome, fixed-width `Space Mono`); inter-lane and inter-group
  separation uses borders/tonal steps, not colour.
- [ ] No clip block, waveform, live-projection growth, playhead, or horizontal ruler logic
  is added by this PRD (owned by PRD-0068, PRD-0069, PRD-0070, PRD-0066 respectively).
- [ ] No DSP and no audio-thread code is added; all model observation and rendering run on
  the message/UI thread, perform no allocation on the audio thread, take no locks, and do
  no I/O on the audio thread (none of this PRD's code runs on the audio thread).
- [ ] At least one test under `Tests/` (e.g. `ChannelGroupLayoutTests.cpp`) drives a
  synthetic `daw` ValueTree (add/remove `tracks[]`, toggle source mode) and asserts the
  resulting group count, lane order, lane activeness, and deck-id ordering — without
  instantiating real audio decks.

## 1.5. Grey Areas

### 1.5.1. Always Show Three Lanes vs Only the Audible Ones

When a deck is in original mode, the `Instrumental` and `Vocal` lanes carry no audio; in
stems mode the `Original` lane carries no audio; muting a stem silences its lane. The
layout could (a) always render all three lanes regardless of source mode, (b) render only
the currently-audible lane(s), or (c) always render all three but visually dim the
inactive ones (tonal de-emphasis).

**Resolution:** Approach (c) — always render all three lanes, dim the inactive ones via a
tonal step (a deeper/sparser dither), keeping the active lane(s) at full tonal weight. This
keeps the group's vertical footprint **stable** as the DJ flips source modes (no jarring
relayout, no groups jumping up/down underneath the cursor), preserves a consistent mental
model ("every deck has exactly three lanes"), and still communicates audibility honestly
through tonal weight rather than presence/absence. Fully hiding inactive lanes (b) would
churn the layout on every toggle and break vertical alignment between groups; this PRD
rejects it. The dim/active distinction is purely tonal (no colour), satisfying DESIGN.md.

### 1.5.2. Collapse Granularity: Group vs Individual Lane

EPIC-0008 §1.2.1 lists "lane collapse" but does not specify whether the unit of collapse
is the whole channel group, an individual lane, or both.

**Resolution:** Provide **group-level collapse** as the primary affordance (one toggle on
the `ChannelGroupHeader` folds all three lanes to just the header row), and treat
individual-lane collapse as optional/deferred. Group collapse is the high-value action — a
DJ wanting to focus on Deck 1 while four decks are loaded collapses the other three groups
to reclaim most of the vertical space in one click each. Per-lane collapse is finer-grained
but lower-value (a single lane is already short) and risks a fiddly, inconsistent layout.
The `LaneView` molecule is structured so a per-lane collapse can be added later without
reworking the group, but this PRD ships only group-level collapse to keep scope tight.

### 1.5.3. Lane Vertical Height & Interaction with Panel Height (PRD-0066)

Lane height interacts with the PRD-0066 panel: a tall fixed lane height plus four groups
overflows quickly; a short height crams waveforms (PRD-0068) into illegibility. The panel
itself is resizable/collapsible (PRD-0066).

**Resolution:** Lanes use a **fixed default height** (a DESIGN.md-grid-aligned constant,
e.g. a multiple of the base spacing unit) chosen so a single fully-expanded channel group
(header + three lanes) fits comfortably within the default expanded panel height, and so
two groups are partially visible before scrolling. The lane height is **not** auto-stretched
to fill the panel (that would make one deck's lanes huge and break cross-group alignment);
instead, overflow is handled by vertical scroll (§1.5.5) and collapse (§1.5.2). If the
panel is resized taller (PRD-0066), more groups become visible but lane height stays fixed;
the panel does not redistribute its extra height into taller lanes. This keeps every lane
the same height across all groups, which is essential for a readable arrangement and for
the future playhead (PRD-0070) crossing all lanes at a consistent scale.

### 1.5.4. Ordering of Decks / Groups

`daw.tracks[]` children may be inserted in any order (a deck loaded later could, depending
on PRD-0063's implementation, append its node), but the visual stack must be predictable.

**Resolution:** Order channel groups by **deck id ascending** (Deck 1 top → Deck 4 bottom),
independent of `daw.tracks[]` child insertion order. Each `daw.tracks[i]` node carries its
owning deck id (PRD-0063); this PRD's view sorts by that id when laying out the stack, so
Deck 3 loading before Deck 2 still places Deck 2's group above Deck 3's. Deck-id order is
the DJ's stable spatial mental model (it matches the physical/UI deck arrangement) and
avoids groups reshuffling as tracks are loaded/ejected in arbitrary sequence.

### 1.5.5. Empty Deck (No Track): Show Empty Group or Hide

A deck with no track loaded has no `daw.tracks[]` node per PRD-0063 (groups are created on
load). But one could argue for always showing four empty placeholder groups so the DJ sees
the full deck layout up front.

**Resolution:** **Hide** groups for decks with no loaded track — render a channel group
only when its `daw.tracks[i]` node exists. The timeline is an *arrangement* of what is
actually loaded and (later) playing; four permanent empty groups would waste vertical space
(the scarcest dimension here) and add visual noise with no content. This also keeps the
view a pure projection of the `daw` ValueTree (group ⇔ track node), which is the cleanest
Observer contract and the easiest to test. When a deck loads, its group appears (§1.3
flow); when it ejects, the group disappears. An empty-state hint (e.g. a faint "load a
track" affordance) in the panel body when zero groups exist is a PRD-0066 panel concern,
not this PRD's.

### 1.5.6. Instrumental Lane: Summed Stem vs Three Underlying Stems

EPIC-0002 may expose stems at finer granularity (e.g. drums / bass / other) than the single
`Instrumental` lane implies. The three-lane model (§1.3.3) names exactly one `Instrumental`
lane, but the underlying separation could be three or four stems.

**Resolution:** The `Instrumental` lane is a **single lane representing the summed
instrumental** (drums + bass + other combined), not three separate stem lanes. EPIC-0008
§1.3.3 fixes the channel-group model at exactly three lanes (`Original`, `Instrumental`,
`Vocal`), mirroring the two-stem (vocal / instrumental) playback contract from EPIC-0002
that PRD-0062's source toggle drives. If a future Epic exposes per-stem lanes
(drums/bass/other as distinct rows), that is a model and layout extension — a new lane kind
in `daw.tracks[i].lanes[]` — and does not belong in this PRD. Here, `Instrumental` is one
row mapping to the summed instrumental source id; the layout does not subdivide it.

### 1.5.7. Vertical Scroll vs Fixed When 4 Decks × 3 Lanes Overflow

Twelve lanes plus four group headers will not fit the default panel body. The panel could
(a) scroll vertically, (b) compress lane heights to force-fit, or (c) rely solely on
collapse to fit.

**Resolution:** **Vertical scroll** (a) is the primary mechanism, with collapse (§1.5.2) as
a complementary affordance — never height compression (b). Force-fitting by shrinking lanes
would violate the fixed-height invariant (§1.5.3), make waveforms (PRD-0068) illegible, and
break cross-group alignment. A DESIGN.md-compliant vertical scrollbar lets the DJ reach all
groups at full, consistent lane height; the PRD-0066 ruler stays pinned at the top of the
panel and is unaffected by the vertical scroll position. Collapse remains available to
reduce how much scrolling is needed, but the canonical overflow answer is to scroll, not to
shrink.

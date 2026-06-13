# Design System Document: The 1-Bit Deck

## 1. Overview & Creative North Star

### Creative North Star: "The Orthographic Sound-System"

This design system rejects the "smoothness" and neon glows typical of modern DJ software in favor of a meticulous, high-contrast 1-bit aesthetic. It is a celebration of vintage hardware samplers and 90s trackers, reimagined through a high-end editorial lens. We are not simply creating a "retro" interface; we are building a precision instrument where every pixel is an intentional choice.

The system utilizes **Orthographic Layering**. Elements (Decks, Mixer, Library, FX) are stacked with rigid, structural precision, creating a dense, information-rich environment that feels like a professional workstation from a lost era of 8-bit computing.

---

## 2. Colors & Tonal Texture

The palette is strictly monochrome. The illusion of gray and sonic depth is created solely through pixel density (dithering).

* **Primary Palette:**
  * `primary`: `#2d2d2d` (The ink, the data, the sound)
  * `surface`: `#fdfdfd` (The chassis of the interface)
* **The "No-Line" Rule for Modules:** Avoid using 1px solid dividers to separate Decks from the Mixer or Library. Instead, use background shifts. A `surface-container-low` (`#f3f3f4`) panel should sit on a `surface` background.
* **Dithered Gradients (Audio Levels & VU Meters):** For VU meters and volume faders, do not use green/yellow/red gradients. Use dithering patterns (checkerboard pixels) that become progressively denser and darker as the volume rises toward the peak (clipping).
* **Surface Hierarchy:**
  * `surface-container-highest` (`#e2e2e2`): Deck headers, transport bar (Play/Cue).
  * `surface-container-lowest` (`#fdfdfd`): Text input areas (track search) or high-priority visual canvases (the Waveform display).

---

## 3. Typography

We utilize a sharp, pixel-aligned bitmap approach to maintain editorial clarity, which is essential when mixing live in low-light environments.

* **Display (BPM & Time elapsed/remaining):** `Space Mono Regular` at massive scales (Display-LG: 3.5rem or higher). Numbers must be massive and brutalist. The contrast between giant BPMs and tiny track metadata labels creates the "Editorial" feel.
* **Body (Track Library & Metadata):** `Space Mono Regular` (Body-MD: 0.875rem). The monospaced, fixed-width nature of this font mimics system terminals while ensuring maximum legibility in dense lists.
* **Visual Hierarchy for Controls:** Bold, all-caps labels (`label-md`) for EQ (HI, MID, LOW), Filters, and FX, to make them look like printed labels on hardware equipment.

---

## 4. Elevation & Depth

Depth is achieved through **Tonal Layering** and **Pixel Offset**, not simulated light sources.

* **The Layering Principle:** The central Mixer should appear "lifted" relative to the side Decks using the surface scale.
* **Ambient Shadows (The Dithered Drop):** Effect tooltips and floating panels (e.g., the Export dialog) use a **Dithered Shadow**. Apply a 2px or 4px offset with a 50% checkerboard pattern of `#2d2d2d`. Zero blur.
* **Popup & Dropdown Menus:** Flat, no shadow of any kind — just the surface fill and the 2px solid `#2d2d2d` border. A dropdown opened from a button matches the width of that button.
* **Zero Roundedness:** All `border-radius` tokens are strictly `0px`. Sharp corners are mandatory to maintain orthographic integrity.

---

## 5. DJ Specific Components

### Waveforms

* **The Problem:** Traditional software uses colors (red, blue, green) to indicate frequencies (lows, mids, highs).
* **The 1-Bit Solution:** Use **pattern density**. "Kicks" (Lows) are represented by solid `primary` blocks. "Hi-hats" and highs are represented by sparse dithering (stippling). The final waveform will look like a lithographic print of the sound.

### Decks & Transport (Play, Cue, Sync)

* **Tactile Buttons:** Massive squares. All buttons carry a mandatory `2px solid #2d2d2d` border at all times.
  * **Active state:** `#2d2d2d` fill, `#fdfdfd` text, `2px solid #2d2d2d` border.
  * **Inactive state:** `#fdfdfd` fill, `#2d2d2d` text, `2px solid #2d2d2d` border.
  * No transition animations or fades — state changes are instant.

Hover on an inactive button steps the fill to `surface-container-highest` (`#e2e2e2`) instantly; pressing previews the full inversion. Disabled controls render their ink at 35% alpha. Every clickable element shows the pointing-hand cursor while it can actually respond to a click.
* **Jog Wheels / Phase Meters:** Instead of simulating a round vinyl record, use 1-bit pixel arcs or a brutal, square linear phase indicator. If using a circular shape, apply a dithered shadow in the bottom-right quadrant.

### Mixer: Knobs (EQ/Filter) & Faders (Line/Crossfader)

* **Knobs:** Constructed in pure pixel-art style, built from four monochrome layers (outside-in): 11 small square ticks placed every 30 degrees across the 300-degree sweep (each square rotated so a flat side faces the center; the bottom 60-degree dead zone stays empty), a thick solid `primary` outer ring, a `surface` white face, and an inner `primary` arc broken by a gap that travels with the needle. The needle is a straight thick line from the center pointing out through that gap. Ring, inner arc, and needle all share a single stroke weight (~10% of the ring radius, minimum 2px).
* **Faders:** Vertical and horizontal (crossfader) tracks using `surface-container-highest`. The fader "cap" (handle) is a massive, solid black rectangular block.

### Track Library (The Browser)

* **Visual Rigor:** No solid divider lines between tracks.
* **Zebra-Striping:** Use an alternating background shift between `surface` and `surface-container-low` for song rows.
* **"Now Playing" State:** The track currently loaded on a deck is not highlighted with a bright color, but with inverted colors (white text on a black background) or a blinking block indicator (`[>]`).

### The "Glitch" / Warning States

* **Audio Clipping / Sync Loss:** If audio clips or Sync is lost, use a background with a random dithering pattern (visual noise). This communicates a system emergency much better than classic red, staying true to the monochrome restriction.

---

## 6. Do's and Don'ts

### Do:

* **Do** align every element to a strict pixel grid. If a waveform appears blurry, it is a system failure.
* **Do** use an asymmetrical layout to capture attention on vital information. Remaining track time should hierarchically dominate the EQ.
* **Do** use `spacing-4` (0.9rem) to clearly space apart Decks, Browser, and Mixer. Breathing room in the interface prevents visual fatigue during long sets.
* **Do** design for instant hover/active states. DJs need zero-latency visual feedback.

### Don't:

* **Don't** use `border-radius`. Ever. Music may be fluid, the interface must be rigid.
* **Don't** use soft shadows or blur to highlight an active Deck.
* **Don't** use anti-aliased vector icons. Use hand-crafted "pixel-perfect" icons on the grid for functions like Headphones (Cue), Loops, or FX.
* **Don't** add gradients to beautify waveforms. Beauty must arise from the mathematical precision of the raw audio data.


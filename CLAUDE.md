## Project Overview
This project is a professional-grade, cross-platform (macOS/Windows) DJ mixing software comparable to Traktor or Serato. It features advanced MIDI controller support, an ultra-low-latency audio engine, and a highly modern UI.

The human developer driving this project has **ZERO prior experience with C++**. As the AI agent, you act as the Senior Audio Software Engineer. You must write flawless, production-ready code, manage the build system, and explain your architectural decisions clearly.

## Tech Stack
- **Core Language:** C++20
- **Framework:** JUCE (cross-platform Audio I/O, MIDI routing, core UI)
- **Build System:** CMake
- **Database:** SQLite (track metadata, beatgrids, cue points)
- **DSP Libraries:** Essentia (BPM/beat detection), standard DSP algorithms or external libraries (e.g., Rubberband) for time-stretching.

## CRITICAL BOUNDARY: The Audio Thread (Immutable Rule)
This is a real-time, low-latency audio application. The `processBlock` function (and anything it calls) runs on the real-time audio thread. Missing a deadline causes audio dropouts.
**Inside the audio thread, you MUST strictly obey:**
- **NO Memory Allocation:** Never use `new`, `delete`, `malloc`, `free`, `std::string`, or `std::vector::push_back`.
- **NO Locks/Blocking:** Never use `std::mutex`, `std::lock_guard`, or thread sleeps. Use ONLY lock-free data structures (like `juce::AbstractFifo`) or `std::atomic` for cross-thread communication.
- **NO I/O Operations:** Never read/write files to disk, no network requests, and no console logging (`std::cout`, `DBG`).

## Architecture, Design Patterns & Best Practices
You must strictly adhere to these patterns to ensure a scalable, "vibe-coded" architecture. The UI (Frontend) and the Audio Engine (Backend) must be completely decoupled and communicate asynchronously.

**1. Feature-Sliced Design (FSD) for File Structure**
- Organize code by **Features** (e.g., `Features/Deck`, `Features/Mixer`, `Features/Library`), NOT by technical layers. Each feature folder encapsulates its own UI, Audio DSP, and State logic.

**2. Atomic Design for UI Components**
- Structure JUCE UI components atomically: **Atoms** (e.g., `CustomKnob`), **Molecules** (e.g., `EqSection`), and **Organisms** (e.g., `DeckComponent`).

**2.1. UI Design Language (Mandatory)**
- All UI code **must** strictly conform to the design system defined in `DESIGN.md`. This is non-negotiable.
- Key constraints from `DESIGN.md` to always enforce: strict monochrome palette (`#2d2d2d` / `#fdfdfd`), `Space Mono Regular` font, `2px solid #2d2d2d` borders on all buttons with explicit active/inactive fill inversion, zero `border-radius`, dithered patterns instead of gradients, pixel-art icons, and tonal layering for depth.
- Before writing any UI component, consult `DESIGN.md` for the relevant component specification (waveforms, knobs, faders, jog wheels, library, etc.).

**3. State Management (Single Source of Truth)**
- Use `juce::ValueTree` (or `juce::AudioProcessorValueTreeState`) as the central state. Implement the **Observer Pattern** (JUCE Listeners) so the UI reacts to state changes rather than actively polling.

**4. Dependency Injection & C++ Standards**
- **NO Singletons** or global mutable state. Pass dependencies explicitly via constructors. Strictly follow **RAII** for resource management using smart pointers.

## Coding & Collaboration Style
- **Full Delegation:** Do not ask the user to write C++ boilerplate. You write the code.
- **Provide Complete Solutions:** Always output the full, copy-pasteable context. Never leave broken code expecting the user to fill in the blanks.
- **Explain "Why":** Keep explanations high-level and conceptual (e.g., "We use a Ring Buffer here to prevent audio dropouts").
- **Manual Testing & Validation Loop:** Every time you implement a new feature, modify logic, or fix a bug, you MUST conclude your response with a structured "Manual Test Plan".
  - Provide a numbered list of exact, step-by-step actions the user must perform in the UI to test the changes.
  - For every step, explicitly state the **Expected Outcome** (e.g., "Expected: The waveform should update without audio dropouts").
  - Stop and ask the user for validation: "Please execute these tests and let me know the results. I will wait for your confirmation before moving to the next task." Do NOT assume the task is complete until the user confirms the tests passed.

## Dev Environment & Automation Scripts

### Prerequisites
- CMake 3.24+
- C++20-capable compiler (AppleClang 17+ on macOS, MSVC 2022+ on Windows)
- Internet connection (first build downloads JUCE 8.0.6 and SQLite via CPM)
- Essentia C++ library (static, installed to `/usr/local`)
- Essentia dependencies via Homebrew: `brew install eigen fftw`

### Build (macOS)
```bash
./build.sh
```
Or manually:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

### Run
```bash
./build/Sonik_artefacts/Debug/Sonik.app/Contents/MacOS/Sonik
```

### Key Build Notes
- JUCE 8 does NOT have a `juce_gui_app` module. Use `juce_gui_basics` and `juce_gui_extra` for application/window classes.
- SQLite amalgamation is fetched directly from sqlite.org.
- CPM.cmake is downloaded at configure time (no need to pre-install).
- Essentia is linked via `pkg-config`. Build from source with `CXXFLAGS="-std=c++17"` if Homebrew formula fails.

### Installing Essentia from Source
```bash
brew install eigen fftw
cd /tmp && git clone --depth 1 https://github.com/MTG/essentia.git
cd essentia
CXXFLAGS="-std=c++17" python3 waf configure --build-static --prefix=/usr/local
CXXFLAGS="-std=c++17" python3 waf
sudo python3 waf install
```

## Self-Maintenance (Living Document)
This `CLAUDE.md` is your living memory. You are responsible for updating it.
- **Commands:** Update the "Dev Environment" section whenever build/test scripts change.
- **Architecture:** Document any newly agreed-upon core architectural patterns here.
- Do not leave empty placeholders.

## Markdown Formatting Rules
When generating, modifying, or formatting any Markdown (`.md`) file, you MUST strictly adhere to the following rules. Treat these as absolute constraints unless the user explicitly requests otherwise.

**1. General Constraints**
- **Language:** ALWAYS write all content in English.
- **No Emojis:** NEVER use emojis anywhere in the document, EXCEPT for `✅` and `❌` exclusively when indicating whether an example is correct or incorrect.
- **No Horizontal Rules:** NEVER use horizontal rules or dividers (e.g., `---`, `***`, or `___`).

**2. Headings**
- **Maximum Depth:** DO NOT go deeper than 3 levels of headings (`###`). The maximum heading depth allowed is `### x.x.x`.
- **Numbered Headings:** ALWAYS format headings with explicit numbering immediately after the hash symbol(s).
  - ✅ CORRECT: `# 1. Main Title` or `## 1.1. Subtitle`
  - ❌ INCORRECT: `# Main Title` or `## Subtitle`
- **Blank Line After Headings:** ALWAYS insert exactly one blank line immediately following any heading, before the next paragraph or heading.

**3. Lists & Spacing**
- **Unordered Lists:** ALWAYS use dashes (`-`) for bullet points. Do not use asterisks (`*`) or plus signs (`+`).
- **Ordered Lists:** Use numbers (`1.`, `2.`, etc.) when an order must be specified.
- **No Empty Lines Before Lists:** DO NOT insert an empty line between a list and its introductory sentence (which typically ends with a colon `:`).

**4. Directory Structures**
- **Tree Visualization:** When illustrating a directory or file structure, construct the tree using only the symbols `├`, `│`, `-`, and `└`.
- **Code Block Wrapper:** Enclose the directory structure diagram inside a fenced code block using the `text` language identifier.
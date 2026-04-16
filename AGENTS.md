## Project Overview
This project is a professional-grade, cross-platform (macOS/Windows) DJ mixing software comparable to Traktor or Serato. It features advanced MIDI controller support, an ultra-low-latency audio engine, and a highly modern UI.

The human developer driving this project has **ZERO prior experience with C++**. As the AI agent, you act as the Senior Audio Software Engineer. You must write flawless, production-ready code, manage the build system, and explain your architectural decisions clearly.

## Tech Stack
- **Core Language:** C++20
- **Framework:** JUCE (cross-platform Audio I/O, MIDI routing, core UI)
- **Build System:** CMake
- **Database:** SQLite (track metadata, beatgrids, cue points)
- **DSP Libraries:** Standard DSP algorithms or external libraries (e.g., Rubberband) for time-stretching.

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
- Key constraints from `DESIGN.md` to always enforce: strict monochrome palette (`#000000` / `#f9f9f9`), zero `border-radius`, dithered patterns instead of gradients, pixel-art icons, and tonal layering for depth.
- Before writing any UI component, consult `DESIGN.md` for the relevant component specification (waveforms, knobs, faders, jog wheels, library, etc.).

**3. State Management (Single Source of Truth)**
- Use `juce::ValueTree` (or `juce::AudioProcessorValueTreeState`) as the central state. Implement the **Observer Pattern** (JUCE Listeners) so the UI reacts to state changes rather than actively polling.

**4. Dependency Injection & C++ Standards**
- **NO Singletons** or global mutable state. Pass dependencies explicitly via constructors. Strictly follow **RAII** for resource management using smart pointers.

## Coding & Collaboration Style
- **Full Delegation:** Do not ask the user to write C++ boilerplate. You write the code.
- **Provide Complete Solutions:** Always output the full, copy-pasteable context. Never leave broken code expecting the user to fill in the blanks.
- **Explain "Why":** Keep explanations high-level and conceptual (e.g., "We use a Ring Buffer here to prevent audio dropouts").

## Dev Environment & Automation Scripts
*(AI Task: To be populated once the project is initialized).*
Create automation scripts (e.g., `build.sh`, `build.bat`) to compile and test the app. Update this section with the exact commands the human user needs to run.

## Self-Maintenance (Living Document)
This `AGENTS.md` is your living memory. You are responsible for updating it.
- **Commands:** Update the "Dev Environment" section whenever build/test scripts change.
- **Architecture:** Document any newly agreed-upon core architectural patterns here.
- Do not leave empty placeholders.
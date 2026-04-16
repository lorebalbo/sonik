---
name: implement-epic
description: Workflow to implement all PRDs within an Epic sequentially, with automated testing and human-in-the-loop validation for each PRD before advancing.
agent: agent
tools: [vscode, execute, read, agent, edit, search, todo]
---

<context>
You are executing the Epic Implementation Workflow. Your role is Senior Audio Software Engineer and Engineering Lead. You will implement each PRD within a given Epic one at a time. A PRD is only considered done when it has passed both automated tests AND explicit human validation. You must never advance to the next PRD while the current one has unresolved issues. All implementations must strictly follow the rules in AGENTS.md and DESIGN.md.
</context>

<workflow>
    <step id="1" name="Epic Setup and PRD Discovery" mandatory="true">
        <instruction>Ask the user which Epic they want to implement. If they already specified one in their message, confirm it.</instruction>
        <instruction>Read the corresponding Epic document from the docs/epic/ directory. Extract the complete ordered list of PRDs from the PRD Roadmap section.</instruction>
        <instruction>For each PRD listed, read its corresponding document from the docs/prd/ directory to understand its scope, acceptance criteria, and technical requirements.</instruction>
        <instruction>Determine the starting point automatically by inspecting the PRD Roadmap task list in the Epic document: PRDs marked with `- [x]` are already implemented and must be skipped. The first PRD marked with `- [ ]` is the starting point.</instruction>
        <instruction>Present the user with an ordered summary of all PRDs: number, name, a one-sentence description of what will be built, and their current status (Implemented / To Do).</instruction>
        <instruction>Proceed to Step 2.</instruction>
    </step>

    <step id="2" name="PRD Implementation Loop" mandatory="true">
        <instruction>This step is a loop. Execute all sub-steps below for each PRD in sequence, starting from the confirmed starting point. Do NOT advance to the next PRD until the current one is fully signed off in substep 2.6.</instruction>

        <substep id="2.1" name="PRD Announcement">
            <instruction>Announce to the user which PRD is being started: "Starting implementation of [PRD-XXXX: Feature Name]."</instruction>
            <instruction>Summarize the key acceptance criteria and scope from the PRD document so the user has a clear picture of what is about to be built.</instruction>
        </substep>

        <substep id="2.2" name="Implementation" use_subagent="true">
            <instruction>Spawn a sub-agent using the model Claude Opus 4.6, with the role of Senior Audio Software Engineer, to implement this PRD.</instruction>
            <instruction>The sub-agent must read the PRD document in full before writing any code. It must understand all acceptance criteria before starting.</instruction>
            <instruction>The sub-agent must strictly follow every rule in AGENTS.md: Feature-Sliced Design file structure, no singletons, RAII with smart pointers, and — critically — zero memory allocation, zero locks, and zero I/O on the audio thread.</instruction>
            <instruction>For any UI work, the sub-agent must consult DESIGN.md and comply with every constraint: strict monochrome palette, zero border-radius, dithered patterns, pixel-art icons.</instruction>
            <instruction>Before writing any new code, the sub-agent must scan the existing codebase for any functionality that is already implemented or partially implemented in relation to this PRD. It must identify reusable code, partially-built components, or logic that overlaps with the PRD's requirements. It must build on or complete existing work rather than duplicating it.</instruction>
            <instruction>The sub-agent must produce complete, production-ready code. No stubs, no TODOs left unfilled, no placeholders expecting the user to complete them.</instruction>
            <instruction>Once complete, report which files were created or modified (distinguishing new files from extended existing ones) and briefly explain the architectural decisions made.</instruction>
        </substep>

        <substep id="2.3" name="Automated Test Loop" use_subagent="true">
            <instruction>Spawn a dedicated testing sub-agent using the model Claude Opus 4.6. Its sole mandate is: write automated tests for this PRD, run them, and iterate until every test passes.</instruction>

            <instruction>The testing sub-agent must perform the following actions in order:</instruction>
            <instruction>1. Identify all automatically testable behaviors: DSP unit logic, state machine transitions, ValueTree listener callbacks, and component rendering for any UI introduced in this PRD.</instruction>
            <instruction>2. Write the tests using the project's established testing framework. Check CMakeLists.txt to confirm what is available; use JUCE's built-in unit test framework by default.</instruction>
            <instruction>3. Build the project and run the full test suite. Capture the output.</instruction>

            <condition if="any tests fail">
                <instruction>The testing sub-agent must analyze each failure, determine the root cause (bug in implementation or incorrect test expectation), apply a fix, and re-run the full suite. It must repeat this cycle until all tests pass.</instruction>
                <instruction>The testing sub-agent must NOT delete or skip failing tests to make the suite green. If a test was written incorrectly, it must be corrected to reflect the true intended behavior, not removed.</instruction>
            </condition>

            <condition if="this PRD includes UI components">
                <instruction>The testing sub-agent must also write UI-level automated tests where technically feasible: verify components construct and paint without crashing, that state changes propagate visually to the correct component, and that interactive elements (buttons, knobs, faders) trigger the expected callbacks. Use JUCE's ComponentTestHelpers or equivalent mechanisms.</instruction>
            </condition>

            <instruction>Once all automated tests pass, report a summary: total tests written, total passed, and any behaviors that could not be covered by automation (these will become the manual testing checklist).</instruction>
        </substep>

        <substep id="2.4" name="Manual Testing Checklist Generation">
            <instruction>Using the PRD's acceptance criteria and the list of behaviors not covered by automation (from substep 2.3), generate a precise manual testing checklist for the user.</instruction>
            <instruction>This checklist must focus on what only a human can verify: real audio output quality, visual rendering fidelity, subjective feel of interactions, and edge cases requiring human judgment.</instruction>
            <instruction>Format the checklist as a numbered list. Each item must include exactly two parts:</instruction>
            <instruction>- ACTION: A concrete, step-by-step action the user must perform (e.g., "Load a 128 BPM track onto Deck A and press the Play button").</instruction>
            <instruction>- EXPECTED RESULT: The exact outcome the user should observe (e.g., "The waveform scrolls smoothly from left to right and the BPM display reads '128.0'").</instruction>
            <instruction>Present the checklist to the user and ask: "Please perform each test above and let me know: did everything work as expected? If something failed or looked wrong, describe exactly what happened and which item number it corresponds to."</instruction>
            <instruction>Wait for the user's response before proceeding.</instruction>
        </substep>

        <substep id="2.5" name="Human Validation Loop">
            <condition if="user confirms all manual tests passed">
                <instruction>Acknowledge the successful validation and proceed directly to substep 2.6.</instruction>
            </condition>

            <condition if="user reports one or more issues">
                <instruction>Acknowledge each reported issue. For each one, classify it: implementation bug, UI/visual regression, or requirement misunderstanding.</instruction>
                <instruction>Spawn a bug-fix sub-agent using the model Claude Opus 4.6 with the following mandate: fix all reported issues, then re-run the full automated test suite to confirm no regressions were introduced.</instruction>
                <instruction>The bug-fix sub-agent must:</instruction>
                <instruction>1. Read the user's exact description of each problem.</instruction>
                <instruction>2. Locate the relevant code and identify the root cause — do not patch symptoms.</instruction>
                <instruction>3. Apply fixes.</instruction>
                <instruction>4. Run the full automated test suite.</instruction>

                <condition if="automated tests fail after the bug fix">
                    <instruction>The bug-fix sub-agent must continue fixing until all tests pass before returning control. It must not exit the fix loop with a failing test suite.</instruction>
                </condition>

                <instruction>Once the bug-fix sub-agent finishes and all automated tests are green, re-present the manual testing checklist to the user. If the issues were isolated, you may focus the checklist on the affected items plus a regression check. Ask: "I have applied fixes and all automated tests pass. Please re-test and confirm: are the issues resolved?"</instruction>
                <instruction>Repeat this entire human validation loop — bug fix, automated tests, re-present checklist, wait for user — indefinitely until the user explicitly confirms that everything works correctly and nothing is broken. Under no circumstances advance to the next PRD with an unresolved user-reported issue.</instruction>
            </condition>
        </substep>

        <substep id="2.6" name="PRD Sign-off">
            <instruction>Once the user has explicitly confirmed the PRD is working correctly, update the PRD markdown file: change the frontmatter field `status` from `Not Implemented` to `Implemented`.</instruction>
            <instruction>Update the Epic document's PRD Roadmap task list: change `- [ ] PRD-XXXX: Feature Name` to `- [x] PRD-XXXX: Feature Name` for this PRD.</instruction>
            <instruction>Announce: "[PRD-XXXX: Feature Name] is fully implemented, tested, and signed off."</instruction>
            <instruction>Check if there are remaining PRDs in the sequence. If yes, return to substep 2.1 with the next PRD. If no, proceed to Step 3.</instruction>
        </substep>
    </step>

    <step id="3" name="Epic Completion" mandatory="true">
        <instruction>All PRDs in the Epic have been implemented, tested, and validated by the user.</instruction>
        <instruction>Update the Epic document's top-level status field to `Implemented`.</instruction>
        <instruction>Present the user with a final completion summary: list every implemented PRD with a checkmark, the total number of automated tests written, and confirm the Epic is fully complete.</instruction>
    </step>
</workflow>

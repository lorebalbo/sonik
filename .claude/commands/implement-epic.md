---
description: Workflow to implement all PRDs within an Epic sequentially, with automated testing and human-in-the-loop validation for each PRD before advancing.
---

<context>
You are executing the Epic Implementation Workflow. Your role is Senior Audio Software Engineer and Engineering Lead. You will implement each PRD within a given Epic one at a time. A PRD is only considered done when it has passed both automated tests AND explicit human validation. You must never advance to the next PRD while the current one has unresolved issues. All implementations must strictly follow the rules in CLAUDE.md and DESIGN.md.
</context>

<options>
    <option name="--batch-prds N">
        Limit the run to the next N unimplemented PRDs in the Epic. After N PRDs have been signed off (or after the deferred manual test validation in --go-all-the-way mode), the workflow stops and reports a batch-complete summary instead of proceeding further.
    </option>
    <option name="--go-all-the-way">
        Skip the per-PRD manual testing checkup (substeps 2.5 and 2.6) during implementation. Instead, accumulate all manual test checklists across every PRD in the run and present them together in a single consolidated validation session at the very end, after all automated tests have passed. The human validation loop in Step 3 then covers all accumulated checklists before any PRD is officially signed off.
    </option>
</options>

<manual-test-policy mandatory="true">
    The manual testing checklist given to the user must contain ONLY items that are genuinely impossible for Claude to verify by itself. Before adding any item to the checklist, Claude must first attempt to verify it through one of these self-verification methods:
    - Automated test (logic, state, callbacks, DSP output values)
    - Build output / compiler warnings
    - Log or console output
    - Screenshot analysis (for all visual/UI assertions)

    An item may only appear in the manual checklist if it requires one of the following forms of human judgment that Claude cannot replicate:
    - Subjective audio quality (does the mix sound clean, natural, musical)
    - Subjective tactile feel (does the knob response feel smooth and natural)
    - Perceptual latency judgment (does the timing feel tight under real performance conditions)
    - Aesthetic taste beyond spec compliance (does the overall design feel right)

    The following are BANNED from the manual checklist because Claude can verify them itself:
    - Whether a UI element is visible or present (use screenshot)
    - Whether a color, border, or layout matches the spec (use screenshot + DESIGN.md comparison)
    - Whether a value or label displays correctly (use screenshot or automated test)
    - Whether a component renders without crashing (use automated test or screenshot)
    - Whether a callback or state transition fires (use automated test)
    - Whether the build succeeds (use the build output)

    Violating this policy by offloading self-verifiable checks onto the user is a workflow error.
</manual-test-policy>

<workflow>
    <step id="1" name="Epic Setup and PRD Discovery" mandatory="true">
        <instruction>Parse the arguments passed to this command. Detect the presence of --batch-prds N (extract N as an integer) and --go-all-the-way (boolean flag). Store both values for use throughout the workflow.</instruction>
        <instruction>Ask the user which Epic they want to implement. If they already specified one in their message, confirm it.</instruction>
        <instruction>Read the corresponding Epic document from the docs/epic/ directory. Extract the complete ordered list of PRDs from the PRD Roadmap section.</instruction>
        <instruction>For each PRD listed, read its corresponding document from the docs/prd/ directory to understand its scope, acceptance criteria, and technical requirements.</instruction>
        <instruction>Determine the starting point automatically by inspecting the PRD Roadmap task list in the Epic document: PRDs marked with `- [x]` are already implemented and must be skipped. The first PRD marked with `- [ ]` is the starting point.</instruction>
        <instruction>If --batch-prds N was provided, the active PRD set for this run is the first N unimplemented PRDs starting from the determined starting point. PRDs beyond the batch limit are out of scope for this run and must not be implemented.</instruction>
        <instruction>Present the user with an ordered summary of all PRDs: number, name, a one-sentence description of what will be built, and their current status (Implemented / In Batch / Out of Scope / Already Done). Clearly mark which PRDs will be worked on in this run.</instruction>
        <instruction>If --go-all-the-way is active, notify the user: "Running in --go-all-the-way mode. Manual tests for all PRDs will be consolidated and presented at the end after all automated tests have passed."</instruction>
        <instruction>Initialise an empty ordered list called DEFERRED_MANUAL_TESTS. This list will accumulate the manual checklist entries from every PRD when --go-all-the-way is active.</instruction>
        <instruction>Proceed to Step 2.</instruction>
    </step>

    <step id="2" name="PRD Implementation Loop" mandatory="true">
        <instruction>This step is a loop. Execute all sub-steps below for each PRD in the active PRD set (respecting the --batch-prds limit), starting from the confirmed starting point. Do NOT advance to the next PRD until the current one is fully signed off in substep 2.7 (or, in --go-all-the-way mode, until automated tests and UI verification pass and the checklist has been appended to DEFERRED_MANUAL_TESTS).</instruction>

        <substep id="2.1" name="PRD Announcement">
            <instruction>Announce to the user which PRD is being started: "Starting implementation of [PRD-XXXX: Feature Name]."</instruction>
            <instruction>Summarize the key acceptance criteria and scope from the PRD document so the user has a clear picture of what is about to be built.</instruction>
        </substep>

        <substep id="2.2" name="Implementation" use_subagent="true">
            <instruction>Spawn a sub-agent using the model Claude Opus 4.6, with the role of Senior Audio Software Engineer, to implement this PRD.</instruction>
            <instruction>The sub-agent must read the PRD document in full before writing any code. It must understand all acceptance criteria before starting.</instruction>
            <instruction>The sub-agent must strictly follow every rule in CLAUDE.md: Feature-Sliced Design file structure, no singletons, RAII with smart pointers, and — critically — zero memory allocation, zero locks, and zero I/O on the audio thread.</instruction>
            <instruction>For any UI work, the sub-agent must consult DESIGN.md and comply with every constraint: strict monochrome palette, zero border-radius, dithered patterns, pixel-art icons.</instruction>
            <instruction>Before writing any new code, the sub-agent must scan the existing codebase for any functionality that is already implemented or partially implemented in relation to this PRD. It must identify reusable code, partially-built components, or logic that overlaps with the PRD's requirements. It must build on or complete existing work rather than duplicating it.</instruction>
            <instruction>The sub-agent must produce complete, production-ready code. No stubs, no TODOs left unfilled, no placeholders expecting the user to complete them.</instruction>
            <instruction>Once complete, report which files were created or modified (distinguishing new files from extended existing ones), whether this PRD includes any UI components, and briefly explain the architectural decisions made.</instruction>
        </substep>

        <substep id="2.3" name="UI Visual Verification">
            <condition if="this PRD includes NO UI components">
                <instruction>Skip this substep entirely and proceed to substep 2.4.</instruction>
            </condition>

            <condition if="this PRD includes UI components">
                <instruction>This substep is a visual verification loop. Its purpose is to confirm that every UI element introduced by this PRD fully complies with DESIGN.md before any manual checklist is generated. Claude must complete this loop without user involvement.</instruction>

                <instruction>Execute the following sequence:</instruction>
                <instruction>1. Build the project by running: cmake --build build --parallel. If the build fails, fix all compilation errors and rebuild before continuing.</instruction>
                <instruction>2. Launch the application in the background: ./build/Sonik_artefacts/Debug/Sonik.app/Contents/MacOS/Sonik &amp;</instruction>
                <instruction>3. Wait 3 seconds for the app to fully render, then take a screenshot: screencapture -x /tmp/sonik_ui_verify.png</instruction>
                <instruction>4. Read the screenshot file at /tmp/sonik_ui_verify.png using the Read tool to load it as an image.</instruction>
                <instruction>5. Carefully analyze the screenshot against every relevant DESIGN.md constraint for the UI components introduced in this PRD. Check in particular: correct monochrome palette (#2d2d2d / #fdfdfd only), 2px solid borders, zero border-radius on all elements, correct font (Space Mono Regular), absence of gradients, pixel-art icons, and correct tonal layering.</instruction>
                <instruction>6. Kill the app after capturing the screenshot: pkill -f Sonik</instruction>

                <condition if="screenshot analysis reveals any DESIGN.md non-compliance">
                    <instruction>Do NOT proceed to substep 2.4. Instead, fix every identified visual issue in the source code, rebuild the project, relaunch the app, take a new screenshot, and re-analyze it. Repeat this fix-rebuild-screenshot-analyze cycle until the screenshot confirms full DESIGN.md compliance.</instruction>
                    <instruction>Report each visual issue found and the fix applied before repeating the cycle.</instruction>
                </condition>

                <condition if="screenshot confirms full DESIGN.md compliance">
                    <instruction>Report to the user: "UI visual verification passed. Screenshot analysis confirms DESIGN.md compliance." Attach or describe the key observations from the screenshot. Then proceed to substep 2.4.</instruction>
                </condition>
            </condition>
        </substep>

        <substep id="2.4" name="Automated Test Loop" use_subagent="true">
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

            <instruction>Once all automated tests pass, report a summary: total tests written, total passed, and — strictly applying the manual-test-policy defined above — the list of behaviors that genuinely cannot be verified by code or screenshot and must go to the human. Do NOT include in this list anything that was already verified by automated tests or by the UI visual verification screenshot in substep 2.3.</instruction>
        </substep>

        <substep id="2.5" name="Manual Testing Checklist Generation">
            <instruction>Before writing any checklist item, re-read the manual-test-policy block at the top of this document and enforce it strictly. Every candidate item must pass this gate: "Is there ANY way Claude can verify this through code, a test, a log, or a screenshot?" If yes, Claude must perform that verification now and discard the item from the checklist. Only items that survive this filter — those requiring genuine human audio, tactile, or perceptual judgment — may appear in the checklist.</instruction>

            <condition if="--go-all-the-way is NOT active">
                <instruction>Generate the final manual testing checklist using only the items that passed the policy gate above.</instruction>
                <instruction>Format the checklist as a numbered list. Each item must include exactly two parts:</instruction>
                <instruction>- ACTION: A concrete, step-by-step action the user must perform (e.g., "Load a 128 BPM track onto Deck A and press the Play button then listen through headphones").</instruction>
                <instruction>- EXPECTED RESULT: The exact outcome the user should observe or hear (e.g., "The beat transition sounds clean and rhythmically locked with no audible artifacts").</instruction>
                <instruction>If the filtered checklist is empty (everything was self-verifiable), state explicitly: "No manual tests required for this PRD — all acceptance criteria were verified automatically." Then proceed directly to substep 2.7 without waiting for user input.</instruction>
                <instruction>If the checklist is non-empty, present it to the user and ask: "Please perform each test above and let me know: did everything work as expected? If something failed or sounded wrong, describe exactly what happened and which item number it corresponds to." Wait for the user's response before proceeding.</instruction>
            </condition>

            <condition if="--go-all-the-way IS active">
                <instruction>Generate the filtered manual testing checklist applying the same policy gate.</instruction>
                <instruction>Prefix every item with the PRD identifier (e.g., "[PRD-XXXX]") so the user can distinguish which feature each test belongs to when the consolidated list is presented at the end.</instruction>
                <instruction>Append this labeled checklist to DEFERRED_MANUAL_TESTS (even if the checklist is empty — record it as "[PRD-XXXX]: No manual tests required").</instruction>
                <instruction>Notify the user: "[PRD-XXXX] automated tests and UI verification passed. Manual tests (if any) deferred to end-of-run validation." Then proceed directly to substep 2.7.</instruction>
            </condition>
        </substep>

        <substep id="2.6" name="Human Validation Loop">
            <condition if="--go-all-the-way IS active">
                <instruction>Skip this substep entirely. Proceed directly to substep 2.7.</instruction>
            </condition>

            <condition if="--go-all-the-way is NOT active">
                <condition if="the checklist was empty and no user response is needed">
                    <instruction>Proceed directly to substep 2.7.</instruction>
                </condition>

                <condition if="user confirms all manual tests passed">
                    <instruction>Acknowledge the successful validation and proceed directly to substep 2.7.</instruction>
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

                    <condition if="the reported issues involved UI visual problems">
                        <instruction>After fixes are applied and automated tests are green, re-run the UI visual verification loop from substep 2.3 to confirm the visual regression is resolved before re-presenting the manual checklist.</instruction>
                    </condition>

                    <instruction>Once the bug-fix sub-agent finishes and all automated tests are green, re-present the manual testing checklist to the user. If the issues were isolated, focus the checklist on the affected items plus a regression check. Ask: "I have applied fixes and all automated tests pass. Please re-test and confirm: are the issues resolved?"</instruction>
                    <instruction>Repeat this entire human validation loop — bug fix, automated tests, UI re-verification if needed, re-present checklist, wait for user — indefinitely until the user explicitly confirms that everything works correctly and nothing is broken. Under no circumstances advance to the next PRD with an unresolved user-reported issue.</instruction>
                </condition>
            </condition>
        </substep>

        <substep id="2.7" name="PRD Sign-off">
            <condition if="--go-all-the-way is NOT active">
                <instruction>Once the user has explicitly confirmed the PRD is working correctly (or the manual checklist was empty and all automated checks passed), update the PRD markdown file: change the frontmatter field `status` from `Not Implemented` to `Implemented`.</instruction>
                <instruction>Update the Epic document's PRD Roadmap task list: change `- [ ] PRD-XXXX: Feature Name` to `- [x] PRD-XXXX: Feature Name` for this PRD.</instruction>
                <instruction>Announce: "[PRD-XXXX: Feature Name] is fully implemented, tested, and signed off."</instruction>
            </condition>

            <condition if="--go-all-the-way IS active">
                <instruction>Mark this PRD internally as "automated-tests-passed, pending-manual-validation". Do NOT update the PRD markdown file or Epic document yet — sign-off is deferred to Step 3 after the consolidated manual validation.</instruction>
                <instruction>Announce: "[PRD-XXXX: Feature Name] implementation, UI verification, and automated tests complete. Sign-off deferred to end-of-run manual validation."</instruction>
            </condition>

            <instruction>Check if there are remaining PRDs in the active PRD set for this run. If yes, return to substep 2.1 with the next PRD. If no (either all PRDs in the Epic are done, or the --batch-prds limit has been reached), proceed to Step 3.</instruction>
        </substep>
    </step>

    <step id="3" name="Completion and Final Validation" mandatory="true">
        <condition if="--go-all-the-way IS active">
            <instruction>Review DEFERRED_MANUAL_TESTS. Filter out any PRD entries marked "No manual tests required". If the entire list is empty after filtering, skip straight to sign-off without asking the user for manual validation.</instruction>
            <instruction>If there are items remaining, present the consolidated list to the user grouped by PRD. Open with: "All PRDs in this run have passed automated tests and UI verification. Below is the complete manual validation checklist. Please work through the full list and report any failures with the PRD identifier and item number."</instruction>
            <instruction>Wait for the user's response.</instruction>

            <condition if="user confirms all consolidated manual tests passed (or list was empty)">
                <instruction>For every PRD in the run, update its markdown file: change the frontmatter field `status` from `Not Implemented` to `Implemented`.</instruction>
                <instruction>Update the Epic document's PRD Roadmap task list: change `- [ ]` to `- [x]` for every PRD in the run.</instruction>
                <instruction>Acknowledge: "All manual tests passed. All PRDs are now fully signed off."</instruction>
            </condition>

            <condition if="user reports one or more issues">
                <instruction>Acknowledge each reported issue and identify which PRD it belongs to using the "[PRD-XXXX]" prefix.</instruction>
                <instruction>Spawn a bug-fix sub-agent using the model Claude Opus 4.6 to fix all reported issues across any affected PRDs, then re-run the full automated test suite to confirm no regressions.</instruction>
                <instruction>If any reported issue is visual in nature, re-run the UI visual verification loop (build, launch, screenshot, analyze) for the affected PRD before re-presenting the checklist.</instruction>
                <instruction>Once fixes are applied and all automated tests are green, re-present only the affected checklist items (plus a regression check for neighboring features) and ask the user to re-test.</instruction>
                <instruction>Repeat this loop — bug fix, automated tests, UI re-verification if needed, targeted re-test, wait for user — until the user explicitly confirms everything is correct.</instruction>
                <instruction>Once fully confirmed, update all PRD markdown files and the Epic document as described above.</instruction>
            </condition>
        </condition>

        <condition if="--batch-prds was provided and the batch is now complete (but the Epic still has remaining PRDs)">
            <instruction>Do NOT mark the Epic itself as Implemented. Instead, present a batch-complete summary: list every PRD that was implemented and signed off in this run, the total automated tests written, and the number of PRDs still remaining in the Epic.</instruction>
            <instruction>Inform the user: "Batch of N PRDs complete. Run /implement-epic again to continue with the next batch."</instruction>
        </condition>

        <condition if="all PRDs in the Epic are now implemented (no --batch-prds limit, or the final batch covered the last PRDs)">
            <instruction>Update the Epic document's top-level status field to `Implemented`.</instruction>
            <instruction>Present the user with a final completion summary: list every implemented PRD with a checkmark, the total number of automated tests written, and confirm the Epic is fully complete.</instruction>
        </condition>
    </step>
</workflow>

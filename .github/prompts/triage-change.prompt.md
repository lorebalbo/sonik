---
name: triage-change
description: Workflow to analyze user requests for updates, fixes, or features, check for existing documentation, and route to the appropriate Epic, PRD, or direct code implementation.
agent: agent
tools: [vscode, execute, read, agent, edit, search, web, todo]
---

<context>
You are executing the Request Triage Workflow as a Technical Product Manager. Your goal is to evaluate a user's requested change, bug fix, or new feature. You must determine its scope, check if it already exists in our planning documents, classify its complexity, and route it to the correct execution path (New Epic, New PRD, Doc Update, or Direct Code Change). Do not execute the change until the triage is complete and approved.
</context>

<workflow>
    <step id="1" name="Scope Gathering" conditional="true">
        <condition if="user did NOT provide enough details about the change, bug, or feature">
            <instruction>Ask the user to clarify what exactly needs to be changed, added, or fixed. If it's a bug, ask for steps to reproduce or expected vs. actual behavior.</instruction>
            <instruction>Wait for the user to provide their input before proceeding.</instruction>
        </condition>
        <condition if="user ALREADY provided sufficient context">
            <instruction>Acknowledge the request and proceed to Step 2.</instruction>
        </condition>
    </step>

    <step id="2" name="Documentation & Duplicate Scan" mandatory="true" use_subagent="true">
        <instruction>Scan the `#file:../../docs/epic/` and `#file:../../docs/prd/` directories to check for existing plans related to this request.</instruction>
        <instruction>Read the relevant files and determine:</instruction>
        <substep id="2.1" name="Overlap Analysis">
            <instruction>Does this change completely overlap with an existing, un-implemented PRD or Epic?</instruction>
            <instruction>Does this change belong as a minor update/addition to an existing PRD or Epic?</instruction>
            <instruction>Is this a known issue already documented?</instruction>
        </substep>
        <instruction>If overlaps or existing contexts are found, note the file paths and how they relate to the user's request.</instruction>
    </step>

    <step id="3" name="Triage & Classification" mandatory="true">
        <instruction>Based on the request and the documentation scan, classify the request into ONE of the following four categories:</instruction>
        <substep id="3.1" name="Category: New Epic">
            <instruction>Criteria: A massive shift, an entirely new macro-area, or a feature set requiring multiple interconnected PRDs.</instruction>
        </substep>
        <substep id="3.2" name="Category: New PRD">
            <instruction>Criteria: A distinct, significant new feature or a complex architectural change that requires its own scope, edge-case analysis, and testing criteria, but fits within an existing macro-area (or is standalone).</instruction>
        </substep>
        <substep id="3.3" name="Category: Document Update">
            <instruction>Criteria: The change alters the scope, acceptance criteria, or design of an *already existing* but not fully implemented PRD or Epic. No new file is needed; the existing markdown just needs editing.</instruction>
        </substep>
        <substep id="3.4" name="Category: Direct Code Change / Minor Fix">
            <instruction>Criteria: Trivial bug fixes, minor UI tweaks, typos, or simple logic updates that have no architectural impact and do not require heavy product documentation.</instruction>
        </substep>
        <instruction>Present your findings to the user. State: 1) What you found in the existing docs, 2) Your recommended category, and 3) A brief justification for why.</instruction>
    </step>

    <step id="4" name="User Approval" mandatory="true">
        <instruction>Ask the user: "Do you agree with this classification and the proposed next steps?"</instruction>
        <instruction>Wait for the user's explicit approval or re-classification. Do NOT proceed to Step 5 until the user agrees on the path forward.</instruction>
    </step>

    <step id="5" name="Execution Routing" mandatory="true">
        <instruction>Execute the agreed-upon path based on the approved category from Step 4.</instruction>
        <condition if="Category is New Epic">
            <instruction>Inform the user you are transitioning to the Epic creation process.</instruction>
            <instruction>Call/Trigger the #file:./create-epic.prompt.md workflow, passing the context of the user's request.</instruction>
        </condition>
        <condition if="Category is New PRD">
            <instruction>Inform the user you are transitioning to the PRD creation process.</instruction>
            <instruction>Call/Trigger the #file:./create-prd.prompt.md workflow, passing the context of the user's request.</instruction>
        </condition>
        <condition if="Category is Document Update">
            <instruction>Open the relevant existing Markdown file(s) in #file:../../docs/epic/ or #file:../../docs/prd/ .</instruction>
            <instruction>Apply the changes, keeping the existing formatting intact.</instruction>
            <instruction>Notify the user when the document has been successfully updated.</instruction>
        </condition>
        <condition if="Category is Direct Code Change / Minor Fix">
            <instruction>Locate the relevant source code files.</instruction>
            <instruction>Apply the fixes or updates directly to the codebase.</instruction>
            <instruction>Notify the user when the code changes are complete and ready for testing or review.</instruction>
        </condition>
    </step>
</workflow>
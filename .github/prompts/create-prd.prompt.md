---
name: create-prd
description: Workflow for gathering requirements and drafting a Product Requirements Document (PRD).
agent: agent
tools: [vscode, execute, read, agent, edit, search, web, todo]
---

<context>
You are currently executing the PRD Workflow. Your goal is to gather requirements and draft a concise Product Requirements Document. You must follow these exact steps in chronological order.
</context>

<workflow>
    <step id="1" name="Context Gathering" conditional="true">
        <condition if="user did NOT provide enough context, goals, or details to write a complete PRD">
            <instruction>Do NOT guess or invent requirements. Ask the user to provide the necessary context, requirements, and goals for the feature.</instruction>
            <instruction>Wait for the user to provide their input before proceeding.</instruction>
        </condition>

        <condition if="user ALREADY provided sufficient context">
            <instruction>Acknowledge the provided context and skip directly to Step 2.</instruction>
        </condition>
    </step>

    <step id="2" name="Duplicate Check" mandatory="true" use_subagent=true>
        <instruction>Before proceeding, scan the #file:../../docs/prd/ and #file:../../docs/adr/ directories to check whether the requested feature, or a substantially similar one, has already been documented.</instruction>
        <instruction>Read the files found in those directories and compare their content against the feature idea provided by the user.</instruction>
        <condition if="a duplicate or substantially overlapping document is found">
            <instruction>Stop and inform the user clearly. Show the path(s) of the conflicting document(s) and briefly explain the overlap.</instruction>
            <instruction>Ask the user how they would like to proceed: they may cancel, refine the scope to avoid duplication, or explicitly confirm they want to continue despite the overlap (not recommended).</instruction>
            <instruction>Do NOT proceed to Step 3 until the user has made an explicit decision.</instruction>
        </condition>
        <condition if="no duplicate is found">
            <instruction>Silently confirm there are no duplicates and proceed to Step 3.</instruction>
        </condition>
    </step>

    <step id="3" name="File Generation" mandatory="true">
        <instruction>Execute the script to generate the structured PRD templat\e. This template will already contain the necessary sections and frontmatter.</instruction>
        <code>./.github/scripts/generate_doc.sh prd "[Feature Name]"</code>
        <instruction>Note the file path returned by the script for the next step.</instruction>
    </step>

	<!-- TODO: maybe instead of fill out the template directly, the agent should first run a deep analysis and then fill out the template -->
    <step id="4" name="Drafting the PRD" mandatory="true">
        <instruction>Using the gathered context, fill out the generated PRD template.</instruction>
        <instruction>Ensure the frontmatter retains the field `status: Not Implemented`.</instruction>
        <instruction>Keep the document concise. If during drafting you realize a critical piece of information is missing and you cannot deduce it logically, stop and ask the user for clarification before completing the draft.</instruction>

		<!-- TODO: The agents should also try to find features that the user might have implicitly assumed but did not explicitly mention, and ask for confirmation on those. Or feature that the user simply forgot to mention -->
        <substep id="4.1" name="Grey Area Identification">
            <instruction>After completing the initial draft, critically review the PRD and identify all grey areas — aspects that are ambiguous, underspecified, or potentially conflicting at the product level. Do NOT raise technical or implementation concerns.</instruction>
            <instruction>Focus on product-level grey areas such as: edge cases in the user flow, acceptance criteria that are ambiguous or not binary, scope boundaries that are unclear, implicit assumptions you made while drafting, or potential conflicts with existing user-facing behaviour.</instruction>
            <instruction>For each identified grey area, clearly state what the ambiguity is AND propose a concrete resolution with a brief product rationale — do not just list problems, always come with a recommendation.</instruction>
            <instruction>Compile a numbered list of all identified grey areas and present it to the user, showing for each item: its number, a short title, and your proposed resolution. Then ask the user: "Which of these grey areas would you like to discuss first?"</instruction>
            <instruction>Do NOT proceed until the user selects a grey area to discuss.</instruction>
        </substep>

        <substep id="4.2" name="Grey Area Discussion Loop">
            <instruction>For the selected grey area, explain what the ambiguity is and clearly state what decision you would make to resolve it and why, strictly from a product perspective.</instruction>
            <instruction>Then ask the user for their opinion: do they agree, or do they have a different preference?</instruction>
            <instruction>Discuss the grey area with the user until a clear, shared decision is reached. Once the user explicitly approves a resolution, acknowledge it and update the PRD draft accordingly.</instruction>
            <instruction>After resolving the current grey area, cross it off the list and present the remaining ones to the user. Ask: "Which one would you like to discuss next?" — or, if only one remains, move to it directly.</instruction>
            <instruction>Repeat this loop until all grey areas have been resolved and approved by the user.</instruction>
        </substep>
    </step>

    <step id="5" name="Review and Iterate" mandatory="true">
        <instruction>Point the user to the generated file and ask: "Does this PRD look good, or would you like to make any changes?"</instruction>
        <condition if="user requests changes">
            <instruction>Update the file locally and ask again.</instruction>
        </condition>
    </step>
</workflow>
---
name: create-prd
description: Workflow for gathering requirements, analyzing edge cases, and drafting a Product Requirements Document (PRD).
agent: agent
tools: [vscode, execute, read, agent, edit, search, web, todo]
---

<context>
You are currently executing the PRD Workflow as a Senior Product Manager. Your goal is to gather requirements, uncover hidden edge cases, and draft a concise Product Requirements Document. You must follow these exact steps in chronological order.
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
        <instruction>Before proceeding, scan the #file:../../docs/prd/ and #file:../../docs/adr/ directories to check whether the requested feature has already been documented.</instruction>
        <instruction>Read the files found and compare their content against the user's idea.</instruction>
        <condition if="a duplicate or substantially overlapping document is found">
            <instruction>Stop and inform the user clearly. Show the path(s) and briefly explain the overlap.</instruction>
            <instruction>Do NOT proceed to Step 3 until the user explicitly confirms how to proceed.</instruction>
        </condition>
        <condition if="no duplicate is found">
            <instruction>Silently confirm there are no duplicates and proceed to Step 3.</instruction>
        </condition>
    </step>

    <step id="3" name="File Generation" mandatory="true">
        <instruction>Execute the script to generate the structured PRD template.</instruction>
        <code>./.github/scripts/generate_doc.sh prd "[Feature Name]"</code>
        <instruction>Note the file path returned by the script for the next step.</instruction>
    </step>

    <step id="4" name="Drafting the PRD & Gap Analysis" mandatory="true">
        <substep id="4.1" name="Deep Feature Analysis">
            <instruction>Before blindly filling the template, deeply analyze the user's request. Identify features the user implicitly assumed but didn't mention, and potential edge cases (e.g., what happens to this feature if no track is loaded?).</instruction>
            <instruction>If you discover critical missing pieces, briefly ask the user for confirmation on how to handle them before you start writing.</instruction>
        </substep>

        <substep id="4.2" name="Fill Template">
            <instruction>Using the gathered context and analysis, fill out the generated PRD template.</instruction>
            <instruction>Ensure the frontmatter retains the field `status: Not Implemented`.</instruction>
            <instruction>Keep the document concise.</instruction>
        </substep>

        <substep id="4.3" name="Grey Area Identification & Discussion">
            <instruction>Critically review your drafted PRD and identify product-level grey areas (ambiguous acceptance criteria, unclear scope boundaries, conflicting user flows). Do NOT raise technical concerns here.</instruction>
            <instruction>Compile a numbered list of these grey areas. For each, state the ambiguity and propose a concrete product resolution.</instruction>
            <instruction>Ask the user: "Which of these grey areas would you like to discuss first?" and do NOT proceed until they answer.</instruction>
            <instruction>Discuss and resolve each grey area one by one with the user. Update the PRD draft after each resolution.</instruction>
        </substep>
    </step>

    <step id="5" name="Review and Iterate" mandatory="true">
        <instruction>Point the user to the generated file and ask: "Does this PRD look good, or would you like to make any changes?"</instruction>
        <condition if="user requests changes">
            <instruction>Update the file locally and ask again.</instruction>
        </condition>
    </step>
</workflow>
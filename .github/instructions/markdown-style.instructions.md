---
description: "Load these Markdown formatting rules when working in a Markdown file (*.md)."
applyTo: "**/*.md"
---
# Markdown Formatting Rules

When generating, modifying, or formatting Markdown (`.md`) files, you MUST STRICTLY adhere to the following rules. Treat these as absolute constraints unless the user explicitly requests otherwise.

## 1. General Constraints
- **Language:** ALWAYS write all content in English.
- **No Emojis:** NEVER use emojis anywhere in the document, **EXCEPT** for `✅` and `❌` exclusively when indicating whether an example is correct or incorrect.
- **No Horizontal Rules:** NEVER use horizontal rules or dividers (e.g., `---`, `***`, or `___`).

## 2. Headings
- **Maximum Depth:** DO NOT go deeper than 3 levels of headings (`###`). The maximum heading depth allowed is `### x.x.x`.
- **Numbered Headings:** ALWAYS format headings with explicit numbering immediately after the hash symbol(s).
  - ✅ **CORRECT:** `# 1. Main Title` or `## 1.1. Subtitle`
  - ❌ **INCORRECT:** `# Main Title` or `## Subtitle`
- **Blank Line After Headings:** ALWAYS insert exactly one blank (empty) line immediately following any heading, before starting the next text paragraph or even another heading separator.
  - ✅ **CORRECT:**
    ```markdown
    # 1. Main Title

    ## 1.1. Subtitle

    This is the text paragraph.
    ```
  - ❌ **INCORRECT:**
    ```markdown
    # 1. Main Title
    ## 1.1. Subtitle
    This is the text paragraph.
    ```

## 3. Lists & Spacing
- **Unordered Lists:** ALWAYS use dashes (`-`) for bullet points. Do not use asterisks (`*`) or plus signs (`+`).
- **Ordered Lists:** Use numbers (`1.`, `2.`, etc.) when an order must be specified.
- **No Empty Lines Before Lists:** DO NOT insert an empty line between a list and its introductory sentence (which typically ends with a colon `:`).
  - ✅ **CORRECT:**
    ```markdown
    The following items are required:
    - First item
    - Second item
    ```
  - ❌ **INCORRECT:**
    ```markdown
    The following items are required:

    - First item
    - Second item
    ```

## 4. Directory Structures
- **Tree Visualization:** When illustrating a directory or file structure, you MUST construct the tree using only the following symbols: `├`, `│`, `-`, and `└`.
- **Code Block Wrapper:** You MUST enclose the directory structure diagram inside a fenced code block using the `text` language identifier (i.e., ` ```text `).
  - ✅ **CORRECT:**
    ```text
    project-root/
    ├- src/
    │  ├- index.js
    │  └- utils.js
    └- README.md
    ```

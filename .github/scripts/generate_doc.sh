#!/bin/bash

# ==============================================================================
# Document Generator Script
# ==============================================================================
# This script automates the creation of Product Requirements Documents (PRD)
# and Architecture Decision Records (ADR).
#
# It automatically identifies the highest existing document number for the chosen
# type, increments it, formats the user-provided title into kebab-case, and
# generates a new markdown file using the predefined template.
#
# Usage:
#   ./.agents/scripts/generate_doc.sh [--verbose] <prd|adr|epic> <"Title of the Document">
#
# Examples:
#   ./.agents/scripts/generate_doc.sh prd "User Login Feature"    -> Creates docs/prd/PRD-0001-user-login-feature.md
#   ./.agents/scripts/generate_doc.sh adr "Switch to PostgreSQL"  -> Creates docs/adr/ADR-0001-switch-to-postgresql.md
#   ./.agents/scripts/generate_doc.sh epic "New Epic Feature"     -> Creates docs/epic/EPIC-0001-new-epic-feature.md
#   ./.agents/scripts/generate_doc.sh --verbose prd "Feature"     -> Outputs process information and success messages
# ==============================================================================

VERBOSE=0
POSITIONAL_ARGS=()

while [[ $# -gt 0 ]]; do
  case $1 in
    --verbose)
      VERBOSE=1
      shift
      ;;
    *)
      POSITIONAL_ARGS+=("$1")
      shift
      ;;
  esac
done

set -- "${POSITIONAL_ARGS[@]}"

log() {
    if [[ $VERBOSE -eq 1 ]]; then
        echo "$@"
    fi
}

# Check argument count
if [[ $# -ne 2 ]]; then
    log "Error: Incorrect number of arguments provided."
    log "Usage: $0 [--verbose] <prd|adr> <Document_Name_String>"
    exit 1
fi

DOC_TYPE="$1"
DOC_NAME="$2"

# Validate the first argument
if [[ "$DOC_TYPE" != "prd" && "$DOC_TYPE" != "adr" && "$DOC_TYPE" != "epic" ]]; then
    log "Error: The first argument must be strictly 'prd', 'adr', or 'epic'."
    log "Usage: $0 [--verbose] <prd|adr|epic> <Document_Name_String>"
    exit 1
fi

# Convert DOC_NAME to kebab-case
DOC_NAME_KEBAB=$(echo "$DOC_NAME" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g' | sed -E 's/^-+|-+$//g')

# Set uppercase prefixes for output filename
if [[ "$DOC_TYPE" == "prd" ]]; then
    PREFIX="PRD"
elif [[ "$DOC_TYPE" == "adr" ]]; then
    PREFIX="ADR"
elif [[ "$DOC_TYPE" == "epic" ]]; then
    PREFIX="EPIC"
fi

DOCS_DIR="docs/${DOC_TYPE}"
TEMPLATE_DIR="./.github/templates"
TEMPLATE_FILE="${TEMPLATE_DIR}/${DOC_TYPE}-template.md"

MAX_NUM=0

# Determine the highest existing document number
if [[ -d "$DOCS_DIR" ]]; then
    for FILE in "$DOCS_DIR"/*.md; do
        if [[ -f "$FILE" ]]; then
            BASENAME=$(basename "$FILE")
            # Extract the 4-digit number if it matches the PREFIX-XXXX pattern
            if [[ $BASENAME =~ ^${PREFIX}-([0-9]{4})[-_].*\.md$ ]]; then
                NUM_STR="${BASH_REMATCH[1]}"
                # Convert to integer removing leading zeros using base 10 interpretation
                NUM=$((10#$NUM_STR))
                if [[ $NUM -gt $MAX_NUM ]]; then
                    MAX_NUM=$NUM
                fi
            fi
        fi
    done
else
    # Create the target directory if missing
    log "Information: Directory '$DOCS_DIR' not found. It will be created."
    mkdir -p "$DOCS_DIR"
fi

# Calculate the next number and format to 4 digits
NEXT_NUM=$((MAX_NUM + 1))
FORMATTED_NUM=$(printf "%04d" "$NEXT_NUM")

NEW_FILE_NAME="${PREFIX}-${FORMATTED_NUM}-${DOC_NAME_KEBAB}.md"
NEW_FILE_PATH="${DOCS_DIR}/${NEW_FILE_NAME}"

# Copy the template over or create an empty file if no template exists
if [[ -n "$TEMPLATE_FILE" && -f "$TEMPLATE_FILE" ]]; then
    cp "$TEMPLATE_FILE" "$NEW_FILE_PATH"
    log "Success: Created document from template at '$NEW_FILE_PATH'."
else
    log "Warning: No template file found at '$TEMPLATE_FILE'."
    log "Creating an empty file instead."
    # Ensure template dir exists for future use
    mkdir -p "$TEMPLATE_DIR"
    touch "$NEW_FILE_PATH"
    log "Success: Created empty document at '$NEW_FILE_PATH'."
fi

if [[ $VERBOSE -eq 0 ]]; then
    echo "$NEW_FILE_PATH"
fi

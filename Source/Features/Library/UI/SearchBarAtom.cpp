#include "SearchBarAtom.h"
#include "LibraryPalette.h"

// =============================================================================
// Internal: zero-border-radius clear button
// =============================================================================

class ClearBtn final : public juce::Component
{
public:
    std::function<void()> onClick;

    ClearBtn()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LibraryPalette::containerLowest());
        g.setColour (LibraryPalette::primary());
        g.setFont (juce::FontOptions ("Space Grotesk", 13.0f, juce::Font::plain));
        g.drawText (juce::CharPointer_UTF8 ("\xc3\x97"), // UTF-8 ×
                    getLocalBounds(), juce::Justification::centred);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.getNumberOfClicks() == 1 && onClick)
            onClick();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClearBtn)
};

// =============================================================================
// SearchBarAtom
// =============================================================================

SearchBarAtom::SearchBarAtom()
{
    editor.setMultiLine (false);
    editor.setReturnKeyStartsNewLine (false);
    editor.setScrollbarsShown (false);
    editor.setPopupMenuEnabled (false);
    editor.setTextToShowWhenEmpty ("Search library...",
                                   LibraryPalette::primary().withAlpha (0.5f));
    editor.applyFontToAllText (juce::Font (LibraryPalette::bodyFont()));
    editor.setColour (juce::TextEditor::backgroundColourId,
                      LibraryPalette::containerLowest());
    editor.setColour (juce::TextEditor::textColourId,          LibraryPalette::primary());
    editor.setColour (juce::TextEditor::outlineColourId,       juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId,juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::highlightColourId,
                      LibraryPalette::primary().withAlpha (0.2f));
    editor.setColour (juce::TextEditor::highlightedTextColourId,
                      LibraryPalette::containerLowest());
    editor.addListener (this);
    addAndMakeVisible (editor);

    auto* btn = new ClearBtn();
    clearBtn.reset (btn);
    btn->onClick = [this] { clear(); };
    btn->setVisible (false);
    addAndMakeVisible (*btn);
}

SearchBarAtom::~SearchBarAtom()
{
    stopTimer();
    editor.removeListener (this);
}

void SearchBarAtom::paint (juce::Graphics& g)
{
    g.fillAll (LibraryPalette::containerLowest());
    g.setColour (LibraryPalette::primary());
    g.drawRect (getLocalBounds(), 1);
}

void SearchBarAtom::resized()
{
    auto b = getLocalBounds().reduced (1);
    const int btnW = b.getHeight();

    if (clearBtn != nullptr && clearBtn->isVisible())
        clearBtn->setBounds (b.removeFromRight (btnW));
    else if (clearBtn != nullptr)
        clearBtn->setBounds ({});

    editor.setBounds (b.withTrimmedLeft (3));
}

void SearchBarAtom::grabFocus()
{
    editor.grabKeyboardFocus();
}

void SearchBarAtom::clear()
{
    editor.clear();
    if (clearBtn) clearBtn->setVisible (false);
    resized();
    stopTimer();
    if (onTextChanged)
        onTextChanged ({});
}

juce::String SearchBarAtom::getText() const
{
    return editor.getText();
}

// ---- TextEditor::Listener ---------------------------------------------------

void SearchBarAtom::textEditorTextChanged (juce::TextEditor& e)
{
    const bool hasText = e.getText().isNotEmpty();
    if (clearBtn) clearBtn->setVisible (hasText);
    resized();
    stopTimer();
    startTimer (150);
}

void SearchBarAtom::textEditorFocusLost (juce::TextEditor&)
{
    editor.setTextToShowWhenEmpty ("Search library...",
                                   LibraryPalette::primary().withAlpha (0.5f));
}

void SearchBarAtom::textEditorEscapeKeyPressed (juce::TextEditor& e)
{
    if (e.getText().isNotEmpty())
        clear();
}

// ---- Timer ------------------------------------------------------------------

void SearchBarAtom::timerCallback()
{
    stopTimer();
    if (onTextChanged)
        onTextChanged (editor.getText());
}

// ---- focusOfChildComponentChanged (Component override) --------------------

void SearchBarAtom::focusOfChildComponentChanged (FocusChangeType)
{
    if (editor.hasKeyboardFocus (false) && editor.getText().isEmpty())
    {
        editor.setTextToShowWhenEmpty (
            "bpm:  key:  rating:  title:  artist:  album:",
            LibraryPalette::primary().withAlpha (0.4f));
    }
    else if (!editor.hasKeyboardFocus (false))
    {
        editor.setTextToShowWhenEmpty ("Search library...",
                                       LibraryPalette::primary().withAlpha (0.5f));
    }
}

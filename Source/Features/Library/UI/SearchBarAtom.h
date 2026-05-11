#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

class ClearBtn;

/// Atom: single-line search field with 150 ms debounce, a × clear button that
/// appears only when text is non-empty, and a focus-triggered scope-operator
/// hint. Zero border-radius. 1 px #000000 border. #ffffff background.
class SearchBarAtom : public juce::Component,
                      private juce::TextEditor::Listener,
                      private juce::Timer
{
public:
    /// Fired 150 ms after the last keystroke with the current text value.
    std::function<void (const juce::String&)> onTextChanged;

    SearchBarAtom();
    ~SearchBarAtom() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;

    void         grabFocus ();
    void         clear     ();
    juce::String getText   () const;

protected:
    // Called when the editor child gains/loses keyboard focus
    void focusOfChildComponentChanged (FocusChangeType) override;

private:
    // juce::TextEditor::Listener
    void textEditorTextChanged      (juce::TextEditor&) override;
    void textEditorFocusLost        (juce::TextEditor&) override;
    void textEditorReturnKeyPressed (juce::TextEditor&) override {}
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;

    // juce::Timer — debounce
    void timerCallback () override;

    juce::TextEditor          editor;
    std::unique_ptr<ClearBtn> clearBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SearchBarAtom)
};

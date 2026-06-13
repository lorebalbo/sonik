#include "MasterButton.h"
#include "Features/Shared/Ui/SonikDraw.h"

MasterButton::MasterButton (juce::ValueTree deckState,
                             MasterClockManager& manager,
                             int deckIndex)
    : juce::Button ("MasterButton"),
      deckState_ (deckState),
      manager_ (manager),
      deckIndex_ (deckIndex)
{
    setClickingTogglesState (false); // ValueTree / MasterClockManager is the source of truth
    isMaster_ = static_cast<bool> (deckState_.getProperty (IDs::isMaster, false));
    deckState_.addListener (this);

    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Master: make this deck the master clock");
}

MasterButton::~MasterButton()
{
    deckState_.removeListener (this);
}

void MasterButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    sonik::ui::draw::paintLatchButton (g, getLocalBounds(), "MASTER",
                                       { .active  = isMaster_,
                                         .hover   = isMouseOver,
                                         .pressed = isButtonDown });
}

void MasterButton::clicked()
{
    // No-op if this deck is already the master clock source
    if (isMaster_)
        return;

    // Delegate to MasterClockManager — it handles mutual exclusion and isSynced clearing
    manager_.setMaster (deckIndex_);
}

void MasterButton::valueTreePropertyChanged (juce::ValueTree& tree,
                                              const juce::Identifier& property)
{
    if (tree == deckState_ && property == IDs::isMaster)
    {
        const bool newVal = static_cast<bool> (tree[property]);
        if (newVal != isMaster_)
        {
            isMaster_ = newVal;
            juce::MessageManager::callAsync (
                [safeThis = juce::Component::SafePointer<juce::Component> (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
        }
    }
}

#include "DeckLayoutManager.h"

DeckLayoutManager::DeckLayoutManager (DeckStateManager& deckState,
                                      AudioEngine& engine,
                                      AudioFileLoader& loader)
    : deckStateManager (deckState),
      audioEngine (engine),
      audioFileLoader (loader),
      decksNode (deckState.getStateTree().getChildWithName (IDs::Decks))
{
    decksNode.addListener (this);
    rebuildDeckShells();
}

DeckLayoutManager::~DeckLayoutManager()
{
    decksNode.removeListener (this);
}

// --- Rebuild shells from state ---

void DeckLayoutManager::rebuildDeckShells()
{
    // Remove old shells
    for (auto& shell : deckShells)
        removeChildComponent (shell.get());
    deckShells.clear();

    // Create shells for each deck
    auto ids = deckStateManager.getDeckIds();
    for (const auto& id : ids)
    {
        auto shell = std::make_unique<DeckShellComponent> (
            deckStateManager, audioEngine, audioFileLoader, id);

        shell->onRemoveRequested = [this] (const juce::String& deckId)
        {
            handleRemoveRequest (deckId);
        };

        addAndMakeVisible (*shell);
        deckShells.push_back (std::move (shell));
    }

    if (getWidth() > 0 && getHeight() > 0)
        applyLayout();
}

// --- Layout ---

void DeckLayoutManager::resized()
{
    applyLayout();
}

void DeckLayoutManager::applyLayout()
{
    auto area = getLocalBounds();
    auto n = static_cast<int> (deckShells.size());
    if (n == 0 || area.isEmpty())
        return;

    const int halfGap = deckGap / 2;

    switch (n)
    {
        case 1:
            deckShells[0]->setBounds (area);
            break;

        case 2:
        {
            int halfW = area.getWidth() / 2;
            deckShells[0]->setBounds (area.withWidth (halfW - halfGap));
            deckShells[1]->setBounds (area.withTrimmedLeft (halfW + halfGap));
            break;
        }

        case 3:
        {
            int halfW = area.getWidth() / 2;
            int halfH = area.getHeight() / 2;

            deckShells[0]->setBounds (area.getX(), area.getY(),
                                      halfW - halfGap, halfH - halfGap);
            deckShells[1]->setBounds (area.getX() + halfW + halfGap, area.getY(),
                                      area.getWidth() - halfW - halfGap, halfH - halfGap);
            deckShells[2]->setBounds (area.getX(), area.getY() + halfH + halfGap,
                                      area.getWidth(), area.getHeight() - halfH - halfGap);
            break;
        }

        case 4:
        default:
        {
            int halfW = area.getWidth() / 2;
            int halfH = area.getHeight() / 2;

            deckShells[0]->setBounds (area.getX(), area.getY(),
                                      halfW - halfGap, halfH - halfGap);
            deckShells[1]->setBounds (area.getX() + halfW + halfGap, area.getY(),
                                      area.getWidth() - halfW - halfGap, halfH - halfGap);
            deckShells[2]->setBounds (area.getX(), area.getY() + halfH + halfGap,
                                      halfW - halfGap, area.getHeight() - halfH - halfGap);
            deckShells[3]->setBounds (area.getX() + halfW + halfGap,
                                      area.getY() + halfH + halfGap,
                                      area.getWidth() - halfW - halfGap,
                                      area.getHeight() - halfH - halfGap);
            break;
        }
    }
}

// --- Remove handling ---

void DeckLayoutManager::handleRemoveRequest (const juce::String& deckId)
{
    if (! deckStateManager.canRemoveDeck (deckId))
        return;

    // If track is loaded (not empty), show confirmation
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (deckTree.isValid())
    {
        auto status = deckTree.getProperty (IDs::playbackStatus).toString();
        if (status != "empty")
        {
            auto options = juce::MessageBoxOptions()
                .withTitle ("Remove Deck " + deckId)
                .withMessage ("This deck has a track loaded. Are you sure you want to remove it?")
                .withButton ("Remove")
                .withButton ("Cancel")
                .withIconType (juce::MessageBoxIconType::QuestionIcon);

            juce::AlertWindow::showAsync (options, [this, deckId] (int result)
            {
                if (result == 1) // "Remove" chosen
                {
                    audioEngine.unregisterDeck (deckId);
                    audioEngine.clearDeckBuffer (deckId);
                    deckStateManager.removeDeck (deckId);
                }
            });
            return;
        }
    }

    // Empty deck — remove immediately
    audioEngine.unregisterDeck (deckId);
    audioEngine.clearDeckBuffer (deckId);
    deckStateManager.removeDeck (deckId);
}

// --- ValueTree::Listener ---

void DeckLayoutManager::valueTreeChildAdded (juce::ValueTree& parent,
                                              juce::ValueTree& child)
{
    if (parent == decksNode && child.hasType (IDs::Deck))
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
                safeThis->rebuildDeckShells();
        });
    }
}

void DeckLayoutManager::valueTreeChildRemoved (juce::ValueTree& parent,
                                                juce::ValueTree& child,
                                                int /*index*/)
{
    if (parent == decksNode && child.hasType (IDs::Deck))
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
                safeThis->rebuildDeckShells();
        });
    }
}

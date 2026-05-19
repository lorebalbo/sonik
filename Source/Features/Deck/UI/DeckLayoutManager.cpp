#include "DeckLayoutManager.h"
#include "../../../MidiHandlers/DeckMidiHandler.h"

DeckLayoutManager::DeckLayoutManager (DeckStateManager& deckState,
                                      AudioEngine& engine,
                                      AudioFileLoader& loader,
                                      WaveformManager& waveformMgr,
                                      BeatGridManager& beatGridMgr,
                                      StemSeparationManager& stemMgr,
                                      MasterClockManager& clockMgr)
    : deckStateManager (deckState),
      audioEngine (engine),
      audioFileLoader (loader),
      waveformManager (waveformMgr),
      beatGridManager (beatGridMgr),
      stemSeparationManager (stemMgr),
      masterClockManager (clockMgr),
      decksNode (deckState.getStateTree().getChildWithName (IDs::Decks))
{
    decksNode.addListener (this);
    rebuildDeckShells();
}

DeckLayoutManager::~DeckLayoutManager()
{
    decksNode.removeListener (this);
}

// --- MIDI handler wiring ---

void DeckLayoutManager::setDeckMidiHandler (DeckMidiHandler* handler)
{
    deckMidiHandler = handler;
    if (deckMidiHandler == nullptr)
        return;

    // Register all currently-alive shells with the handler.
    auto ids = deckStateManager.getDeckIds();
    for (int i = 0; i < static_cast<int> (deckShells.size()) && i < 4; ++i)
    {
        deckMidiHandler->registerDeckEngines (
            static_cast<std::uint8_t> (i),
            deckShells[static_cast<std::size_t> (i)]->getBeatJumpEngine(),
            deckShells[static_cast<std::size_t> (i)]->getLoopEngine(),
            deckShells[static_cast<std::size_t> (i)]->getHotCueManager());
    }
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
            deckStateManager, audioEngine, audioFileLoader, waveformManager, beatGridManager, stemSeparationManager, masterClockManager, id);

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

// --- Incremental deck shell management ---

void DeckLayoutManager::addDeckShell (const juce::String& deckId)
{
    // Check if a shell for this deck already exists
    for (const auto& shell : deckShells)
    {
        if (shell->getDeckId() == deckId)
            return;
    }

    auto shell = std::make_unique<DeckShellComponent> (
        deckStateManager, audioEngine, audioFileLoader, waveformManager, beatGridManager, stemSeparationManager, masterClockManager, deckId);

    shell->onRemoveRequested = [this] (const juce::String& id)
    {
        handleRemoveRequest (id);
    };

    addAndMakeVisible (*shell);
    deckShells.push_back (std::move (shell));

    // Register engines with the MIDI handler now that the shell exists.
    if (deckMidiHandler != nullptr)
    {
        const auto newIndex = static_cast<std::uint8_t> (deckShells.size() - 1);
        if (newIndex < 4)
        {
            auto& newShell = *deckShells.back();
            deckMidiHandler->registerDeckEngines (
                newIndex,
                newShell.getBeatJumpEngine(),
                newShell.getLoopEngine(),
                newShell.getHotCueManager());
        }
    }

    if (getWidth() > 0 && getHeight() > 0)
        applyLayout();
}

void DeckLayoutManager::removeDeckShell (const juce::String& deckId)
{
    for (auto it = deckShells.begin(); it != deckShells.end(); ++it)
    {
        if ((*it)->getDeckId() == deckId)
        {
            // Deregister engines before destroying the shell.
            if (deckMidiHandler != nullptr)
            {
                const auto idx = static_cast<std::uint8_t> (std::distance (deckShells.begin(), it));
                if (idx < 4)
                    deckMidiHandler->deregisterDeckEngines (idx);
            }

            removeChildComponent (it->get());
            deckShells.erase (it);
            break;
        }
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
        {
            // Deck height is fixed — min equals max so the deck is never compressed.
            deckShells[0]->setBounds (area.withHeight (kPreferredDeckH));
            break;
        }

        case 2:
        {
            int halfW = area.getWidth() / 2;
            int deckH = kPreferredDeckH;
            deckShells[0]->setBounds (area.getX(), area.getY(), halfW - halfGap, deckH);
            deckShells[1]->setBounds (area.getX() + halfW + halfGap, area.getY(),
                                      area.getWidth() - halfW - halfGap, deckH);
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
        auto id = child.getProperty (IDs::id).toString();
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this), id]()
        {
            if (safeThis != nullptr)
                safeThis->addDeckShell (id);
        });
    }
}

void DeckLayoutManager::valueTreeChildRemoved (juce::ValueTree& parent,
                                                juce::ValueTree& child,
                                                int /*index*/)
{
    if (parent == decksNode && child.hasType (IDs::Deck))
    {
        auto id = child.getProperty (IDs::id).toString();
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this), id]()
        {
            if (safeThis != nullptr)
                safeThis->removeDeckShell (id);
        });
    }
}

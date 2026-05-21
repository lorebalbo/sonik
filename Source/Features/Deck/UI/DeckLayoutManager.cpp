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
    mixerColumnArea = {};
    if (n == 0 || area.isEmpty())
        return;

    // Centre column reserved for the mixer organism. Width is clamped so it
    // never starves the decks: at most a third of the area, capped at the
    // declared maximum. The column always lives in row 1; rows 2+ span the
    // full width because the single mixer in row 1 already governs all
    // strips for the active deck set.
    const int mixerColW = juce::jmin (mixerColumnMax,
                                      juce::jmax (0, area.getWidth() / 4));
    const int gap       = (mixerColW > 0) ? mixerGap : 0;

    auto rowFor = [this, mixerColW, gap, n] (juce::Rectangle<int> row,
                                             juce::Rectangle<int>& outLeft,
                                             juce::Rectangle<int>& outRight,
                                             juce::Rectangle<int>* outMixerCol)
    {
        // Carve a centred mixer column out of the row. With <= 1 deck the
        // mixer simply takes the right side; with >= 2 decks we split the
        // residual width evenly between the left and right deck slots.
        if (mixerColW <= 0)
        {
            outLeft  = row;
            outRight = {};
            if (outMixerCol != nullptr) *outMixerCol = {};
            return;
        }

        if (n <= 1)
        {
            const auto deckW = juce::jmax (0, row.getWidth() - mixerColW - gap);
            outLeft  = row.withWidth (deckW);
            outRight = {};
            if (outMixerCol != nullptr)
                *outMixerCol = juce::Rectangle<int> (row.getRight() - mixerColW,
                                                     row.getY(),
                                                     mixerColW,
                                                     row.getHeight());
            return;
        }

        const auto sideW = juce::jmax (0, (row.getWidth() - mixerColW - 2 * gap) / 2);
        outLeft  = juce::Rectangle<int> (row.getX(), row.getY(),
                                          sideW, row.getHeight());
        const auto mxX = row.getX() + sideW + gap;
        if (outMixerCol != nullptr)
            *outMixerCol = juce::Rectangle<int> (mxX, row.getY(),
                                                  mixerColW, row.getHeight());
        const auto rightX = mxX + mixerColW + gap;
        outRight = juce::Rectangle<int> (rightX, row.getY(),
                                          juce::jmax (0, row.getRight() - rightX),
                                          row.getHeight());
    };

    switch (n)
    {
        case 1:
        {
            auto row = area.withHeight (kPreferredDeckH);
            juce::Rectangle<int> left, right, mix;
            rowFor (row, left, right, &mix);
            deckShells[0]->setBounds (left);
            mixerColumnArea = mix;
            break;
        }

        case 2:
        {
            auto row = area.withHeight (kPreferredDeckH);
            juce::Rectangle<int> left, right, mix;
            rowFor (row, left, right, &mix);
            deckShells[0]->setBounds (left);
            deckShells[1]->setBounds (right);
            mixerColumnArea = mix;
            break;
        }

        case 3:
        {
            auto row1 = area.withHeight (kPreferredDeckH);
            juce::Rectangle<int> left, right, mix;
            rowFor (row1, left, right, &mix);
            deckShells[0]->setBounds (left);
            deckShells[1]->setBounds (right);
            mixerColumnArea = mix;

            const auto row2 = juce::Rectangle<int> (area.getX(),
                                                     row1.getBottom() + deckGap,
                                                     area.getWidth(),
                                                     kPreferredDeckH);
            deckShells[2]->setBounds (row2);
            break;
        }

        case 4:
        default:
        {
            auto row1 = area.withHeight (kPreferredDeckH);
            juce::Rectangle<int> left, right, mix;
            rowFor (row1, left, right, &mix);
            deckShells[0]->setBounds (left);
            deckShells[1]->setBounds (right);
            mixerColumnArea = mix;

            // Row 2: deck C and deck D sit symmetrically with a centre gap
            // matching the mixer column width above so the four decks line
            // up vertically.
            const int halfGap = deckGap / 2;
            const int row2W   = area.getWidth();
            const int gapW    = juce::jmax (0, mixerColW + 2 * gap);
            const int sideW   = juce::jmax (0, (row2W - gapW) / 2);
            const int row2Y   = row1.getBottom() + deckGap;

            deckShells[2]->setBounds (area.getX(), row2Y,
                                       sideW, kPreferredDeckH);
            deckShells[3]->setBounds (area.getX() + sideW + gapW, row2Y,
                                       juce::jmax (0, row2W - sideW - gapW),
                                       kPreferredDeckH);

            (void) halfGap;
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

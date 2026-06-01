#pragma once
//==============================================================================
// PRD-0079: ArrangementRecompileTrigger — single, coalesced message-thread
// entry point that drives compile + publish whenever the `daw` ValueTree changes.
//
// All subsystems that mutate the `daw` tree (edit commands PRD-0083, the
// recorder EPIC-0009, future automation EPIC-0011) call requestRecompile() after
// their mutation.  A burst of rapid calls within the coalescing window produces
// exactly one compile + publish, bounding CPU cost without starving the audio
// thread of timely schedule updates.
//
// MESSAGE THREAD ONLY.
//==============================================================================

#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ArrangementCompiler.h"
#include "ArrangementPublisher.h"

namespace Daw
{

class ArrangementRecompileTrigger final : private juce::AsyncUpdater
{
public:
    /// @param daw       The "Daw" root branch to compile (not owned).
    /// @param compiler  Compiler (may carry a handle-resolver lambda).
    /// @param publisher Shared publisher (injected, not owned).
    ArrangementRecompileTrigger (const juce::ValueTree&  daw,
                                 ArrangementCompiler     compiler,
                                 ArrangementPublisher&   publisher)
        : daw_       (daw)
        , compiler_  (std::move (compiler))
        , publisher_ (publisher)
    {}

    ~ArrangementRecompileTrigger() override
    {
        cancelPendingUpdate();
    }

    //--------------------------------------------------------------------------
    /// Request a recompile.  May be called many times; the actual compile +
    /// publish is coalesced to one async dispatch per coalescing window.
    /// Safe to call on the message thread from any subsystem.
    //--------------------------------------------------------------------------
    void requestRecompile()
    {
        triggerAsyncUpdate();
    }

    //--------------------------------------------------------------------------
    /// Force an immediate (synchronous) compile + publish without going
    /// through the async queue.  Used at initialisation and in tests.
    //--------------------------------------------------------------------------
    void compileNow()
    {
        cancelPendingUpdate();
        doCompileAndPublish();
    }

    //--------------------------------------------------------------------------
    /// Replace the compiler (e.g. to inject a playback-aware resolver after the
    /// audio/playback layer is wired). Message thread only.
    //--------------------------------------------------------------------------
    void setCompiler (ArrangementCompiler compiler)
    {
        compiler_ = std::move (compiler);
    }

    //--------------------------------------------------------------------------
    /// Returns how many publishes have occurred.  Unit-test hook.
    //--------------------------------------------------------------------------
    int publishCount() const noexcept { return publishCount_; }

private:
    // juce::AsyncUpdater callback — fires once per coalescing window.
    void handleAsyncUpdate() override
    {
        doCompileAndPublish();
    }

    void doCompileAndPublish()
    {
        ArrangementSnapshot scratch;
        compiler_.compile (daw_, scratch);
        publisher_.publish (scratch);
        ++publishCount_;
    }

    const juce::ValueTree  daw_;
    ArrangementCompiler    compiler_;
    ArrangementPublisher&  publisher_;
    int                    publishCount_ { 0 };
};

} // namespace Daw

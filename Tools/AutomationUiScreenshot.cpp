//==============================================================================
// PRD-0093: AutomationUiScreenshot — a small standalone deterministic render of
// the automation lane organisms to a PNG, so the layout / DESIGN.md compliance
// can be inspected outside the live app.
//
// The seed data here is DEMO ONLY and lives ONLY in this tool — never in the app.
// It constructs a TimelineTransform, an AutomationModel over a fresh daw branch
// seeded with realistic lanes (a filter sweep with a step segment, a gentle gain
// ride, a keyLock boolean lane, and a master tempo ride), then composites the
// lane views into one image rendered with the JUCE software renderer.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/Automation/AutomationModel.h"
#include "Features/Daw/Automation/Ui/ContinuousAutomationLaneView.h"
#include "Features/Daw/Automation/Ui/BooleanAutomationLaneView.h"
#include "Features/Daw/Automation/Ui/AutomationLaneMetrics.h"

using namespace Daw;

namespace
{

void savePng (juce::Component& comp, int w, int h, const juce::File& out)
{
    comp.setBounds (0, 0, w, h);
    juce::Image img (juce::Image::RGB, w, h, true);
    {
        juce::Graphics g (img);
        g.fillAll (juce::Colour (0xFFF3F3F4)); // surface-container-low backdrop
        comp.paintEntireComponent (g, false);
    }

    out.deleteFile();
    juce::FileOutputStream stream (out);
    if (stream.openedOk())
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (img, stream);
    }
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    const int width   = 900;
    const int laneH   = AutomationLaneMetrics::kAutomationLaneHeight;

    // 50 px/beat (~one bar per 200 px at 4/4); 120 BPM => 22050 samples/beat.
    TimelineTransform::GridSnapshot grid;
    const double spb = grid.samplesPerBeat; // 22050
    TimelineTransform transform (grid, /*pixelsPerBeat*/ 50.0,
                                 /*leftEdgeSample*/ 0, /*viewportWidthPx*/ (double) width,
                                 /*contentEnd*/ (std::int64_t) (spb * 32));

    auto daw = DawState::createDawBranch();
    AutomationModel model (daw);

    // ---- Channel-A filter sweep: rise then fall, one segment held (step) -----
    {
        auto lane = model.getOrCreateContinuousLane ("A", "filter");
        lane.addBreakpoint ((std::int64_t) (spb * 0),  -0.8, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 2),  -0.2, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 4),   0.5, Interpolation::Step);   // hold
        lane.addBreakpoint ((std::int64_t) (spb * 6),   0.9, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 9),   0.3, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 12), -0.4, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 15), -0.7, Interpolation::Linear);
    }

    // ---- Channel-A gain ride (gentle, dB) ------------------------------------
    {
        auto lane = model.getOrCreateContinuousLane ("A", "gain");
        lane.addBreakpoint ((std::int64_t) (spb * 0),   0.0, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 4),  -3.0, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 8),   2.0, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 14),  0.0, Interpolation::Linear);
    }

    // ---- Channel-A keyLock boolean lane (two on/off toggles) -----------------
    {
        auto lane = model.getOrCreateBooleanLane ("A", "keyLock");
        lane.addStep ((std::int64_t) (spb * 1),  true);
        lane.addStep ((std::int64_t) (spb * 5),  false);
        lane.addStep ((std::int64_t) (spb * 9),  true);
        lane.addStep ((std::int64_t) (spb * 13), false);
    }

    // ---- Master tempo ride (BPM) ---------------------------------------------
    {
        auto lane = model.getOrCreateContinuousLane ("master", "tempo");
        lane.addBreakpoint ((std::int64_t) (spb * 0),  124.0, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 6),  128.0, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 12), 126.0, Interpolation::Linear);
        lane.addBreakpoint ((std::int64_t) (spb * 16), 130.0, Interpolation::Linear);
    }

    // A demo playhead at bar 3.
    const std::int64_t playhead = (std::int64_t) (spb * 7);
    auto playProvider = [playhead]() { return playhead; };

    // ---- Empty lanes: the dimmed default-value line / OFF baseline -----------
    // No breakpoints/steps on purpose — these demonstrate the empty-lane
    // rendering (continuous: dimmed line at the parameter default; boolean:
    // dimmed OFF baseline).
    model.getOrCreateContinuousLane ("B", "volume");
    model.getOrCreateBooleanLane ("B", "pitchStretch");

    // ---- Build the lane views -------------------------------------------------
    ContinuousAutomationLaneView filterView (
        model.getLaneNode ("A", "filter"), transform, &model, "filter");
    ContinuousAutomationLaneView gainView (
        model.getLaneNode ("A", "gain"), transform, &model, "gain");
    BooleanAutomationLaneView keyLockView (
        model.getLaneNode ("A", "keyLock"), transform, &model, "keyLock");
    ContinuousAutomationLaneView tempoView (
        model.getLaneNode ("master", "tempo"), transform, &model, "tempo");
    ContinuousAutomationLaneView emptyVolumeView (
        model.getLaneNode ("B", "volume"), transform, &model, "volume");
    BooleanAutomationLaneView emptyStretchView (
        model.getLaneNode ("B", "pitchStretch"), transform, &model, "pitchStretch");

    filterView.setPlayheadProvider (playProvider);
    gainView.setPlayheadProvider (playProvider);
    keyLockView.setPlayheadProvider (playProvider);
    tempoView.setPlayheadProvider (playProvider);
    emptyVolumeView.setPlayheadProvider (playProvider);
    emptyStretchView.setPlayheadProvider (playProvider);

    // Demonstrate the bypassed (inactive wash) state on the gain lane.
    model.setLaneEnabled (model.getLaneNode ("A", "gain"), false);

    // Per-lane PNGs.
    savePng (filterView,  width, laneH, juce::File ("/tmp/automation_ui_filter.png"));
    savePng (gainView,    width, laneH, juce::File ("/tmp/automation_ui_gain.png"));
    savePng (keyLockView, width, laneH, juce::File ("/tmp/automation_ui_keylock.png"));
    savePng (tempoView,   width, laneH, juce::File ("/tmp/automation_ui_tempo.png"));
    savePng (emptyVolumeView,  width, laneH, juce::File ("/tmp/automation_ui_empty_volume.png"));
    savePng (emptyStretchView, width, laneH, juce::File ("/tmp/automation_ui_empty_stretch.png"));

    // ---- Composite: a vertical stack of all six lanes ------------------------
    juce::Component composite;
    const int gap = 2;
    const int totalH = 6 * laneH + 5 * gap;
    composite.addAndMakeVisible (filterView);
    composite.addAndMakeVisible (gainView);
    composite.addAndMakeVisible (keyLockView);
    composite.addAndMakeVisible (tempoView);
    composite.addAndMakeVisible (emptyVolumeView);
    composite.addAndMakeVisible (emptyStretchView);
    composite.setBounds (0, 0, width, totalH);

    int y = 0;
    filterView.setBounds  (0, y, width, laneH); y += laneH + gap;
    gainView.setBounds    (0, y, width, laneH); y += laneH + gap;
    keyLockView.setBounds (0, y, width, laneH); y += laneH + gap;
    tempoView.setBounds   (0, y, width, laneH); y += laneH + gap;
    emptyVolumeView.setBounds  (0, y, width, laneH); y += laneH + gap;
    emptyStretchView.setBounds (0, y, width, laneH);

    const juce::File compositeOut ("/tmp/automation_ui_composite.png");
    savePng (composite, width, totalH, compositeOut);

    juce::Logger::writeToLog ("Wrote /tmp/automation_ui_composite.png");
    std::cout << "Wrote: " << compositeOut.getFullPathName() << std::endl;
    std::cout << "Wrote: /tmp/automation_ui_filter.png" << std::endl;
    std::cout << "Wrote: /tmp/automation_ui_gain.png (bypassed)" << std::endl;
    std::cout << "Wrote: /tmp/automation_ui_keylock.png" << std::endl;
    std::cout << "Wrote: /tmp/automation_ui_tempo.png" << std::endl;
    std::cout << "composite size: " << width << "x" << totalH << std::endl;

    return 0;
}

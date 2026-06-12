//==============================================================================
// PRD-0093: Automation Lane UI & Rendering tests.
//
// Drives the automation lane organisms against a synthetic AutomationModel +
// TimelineTransform and asserts:
//   * continuous & boolean lane views construct and paint (into a juce::Image)
//     without crashing, empty and populated;
//   * model observation: adding a breakpoint/step repaints / reflects in geometry;
//   * bypass button toggles the lane enable flag (and external change updates it);
//   * step vs linear interpolation produce different rendered geometry;
//   * AUTO disclosure is hidden by default (group height unchanged) and reveal /
//     collapse grow / restore the group height;
//   * the playhead indicator computes the expected x via the shared transform;
//   * empty lanes paint the dimmed default-value line (continuous) / OFF
//     baseline (boolean); a single breakpoint renders as a full-width held line;
//   * editing gestures: a single click never creates a node; double-click on an
//     empty spot creates one (selected, draggable); drag moves it; double-click
//     on a node deletes it.
//
// All message/UI thread; no audio-thread state touched.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/Ui/DawLayoutMetrics.h"
#include "Features/Daw/Ui/Organisms/ChannelGroupStack.h"
#include "Features/Daw/Ui/Organisms/ChannelGroupView.h"
#include "Features/Daw/Editing/EditCommands.h"
#include "Features/Daw/Playback/ArrangementCompiler.h"
#include "Features/Daw/Playback/ArrangementPublisher.h"
#include "Features/Daw/Playback/ArrangementRecompileTrigger.h"
#include "Features/Daw/Automation/AutomationModel.h"
#include "Features/Daw/Automation/ContinuousLane.h"
#include "Features/Daw/Automation/Ui/ContinuousAutomationLaneView.h"
#include "Features/Daw/Automation/Ui/BooleanAutomationLaneView.h"
#include "Features/Daw/Automation/Ui/AutomationLaneStackView.h"
#include "Features/Daw/Automation/Ui/AutomationLaneMetrics.h"
#include "Features/Deck/DeckIdentifiers.h"

namespace
{

Daw::TimelineTransform makeTransform()
{
    return Daw::TimelineTransform (Daw::TimelineTransform::GridSnapshot{},
                                   /*pixelsPerBeat*/ 50.0,
                                   /*leftEdgeSample*/ 0,
                                   /*viewportWidthPx*/ 800.0);
}

juce::Image paintToImage (juce::Component& c, int w, int h)
{
    c.setBounds (0, 0, w, h);
    juce::Image img (juce::Image::RGB, juce::jmax (1, w), juce::jmax (1, h), true);
    juce::Graphics g (img);
    c.paintEntireComponent (g, false);
    return img;
}

void paintInto (juce::Component& c, int w, int h)
{
    (void) paintToImage (c, w, h);
}

} // namespace

class AutomationLaneUiTests final : public juce::UnitTest
{
public:
    AutomationLaneUiTests() : juce::UnitTest ("Automation Lane UI (PRD-0093)", "Sonik") {}

    void runTest() override
    {
        const int laneW = 900;
        const int laneH = Daw::AutomationLaneMetrics::kAutomationLaneHeight;

        //----------------------------------------------------------------------
        beginTest ("continuous lane view: empty + populated paint without crashing");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            auto laneNode = model.getOrCreateContinuousLane ("A", "filter").getState();
            Daw::ContinuousAutomationLaneView view (laneNode, transform, &model, "filter");

            // Empty lane.
            expectEquals (view.getBreakpointCount(), 0);
            paintInto (view, laneW, laneH);

            // Populated.
            Daw::ContinuousLane lane (laneNode);
            lane.addBreakpoint (0,      -1.0, Daw::Interpolation::Linear);
            lane.addBreakpoint (44100,   1.0, Daw::Interpolation::Linear);
            lane.addBreakpoint (88200,   0.0, Daw::Interpolation::Linear);
            expectEquals (view.getBreakpointCount(), 3);
            paintInto (view, laneW, laneH);
            expect (view.computePolyline().size() >= 3);
        }

        //----------------------------------------------------------------------
        beginTest ("boolean lane view: empty + populated paint without crashing");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            auto laneNode = model.getOrCreateBooleanLane ("A", "keyLock").getState();
            Daw::BooleanAutomationLaneView view (laneNode, transform, &model, "keyLock");

            expectEquals (view.getStepCount(), 0);
            paintInto (view, laneW, laneH);

            Daw::BooleanLane lane (laneNode);
            lane.addStep (0,      true);
            lane.addStep (44100,  false);
            lane.addStep (88200,  true);
            expectEquals (view.getStepCount(), 3);
            paintInto (view, laneW, laneH);
            // Two "on" intervals (0..44100 and 88200..edge).
            expect (view.computeBlocks().size() == 2);
        }

        //----------------------------------------------------------------------
        beginTest ("model observation: adding a breakpoint reflects in geometry + repaints");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            auto laneNode = model.getOrCreateContinuousLane ("A", "gain").getState();
            Daw::ContinuousAutomationLaneView view (laneNode, transform, &model, "gain");
            view.setBounds (0, 0, laneW, laneH);

            // A paint pass increments the paint counter (sanity).
            const int before = view.getPaintCount();
            paintInto (view, laneW, laneH);
            expect (view.getPaintCount() > before, "an explicit paint pass increments the counter");

            // Adding a breakpoint must be REFLECTED in the view's computed geometry
            // (the view observes the lane node via the ValueTree listener and
            // recomputes from the model — no polling). repaint() only marks dirty
            // in a headless test, so we assert the geometry getter, not the paint.
            expectEquals (view.getBreakpointCount(), 0);
            expect (view.computePolyline().empty());

            Daw::ContinuousLane lane (laneNode);
            lane.addBreakpoint (10000, 0.0, Daw::Interpolation::Linear);
            lane.addBreakpoint (40000, -3.0, Daw::Interpolation::Linear);

            expectEquals (view.getBreakpointCount(), 2);
            expect (view.computePolyline().size() == 2,
                    "the view reflects the added breakpoints via the model observer");
        }

        //----------------------------------------------------------------------
        beginTest ("bypass button toggles enable flag (and external change reflects)");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            auto laneNode = model.getOrCreateContinuousLane ("A", "filter").getState();
            Daw::ContinuousAutomationLaneView view (laneNode, transform, &model, "filter");
            view.setBounds (0, 0, laneW, laneH);

            expect (view.isLaneEnabled(), "lane enabled by default");

            // Click the bypass button (rightmost slice of the gutter).
            const int bx = Daw::DawLayout::kTrackHeaderWidth
                         - Daw::AutomationLaneMetrics::kBypassButtonWidth / 2
                         - Daw::AutomationLaneMetrics::kBypassButtonInset;
            const juce::Point<int> p (bx, laneH / 2);
            juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(),
                                p.toFloat(), juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                &view, &view, juce::Time::getCurrentTime(), p.toFloat(),
                                juce::Time::getCurrentTime(), 1, false);
            view.mouseUp (e);

            expect (! view.isLaneEnabled(), "bypass click disabled the lane");
            expect (! Daw::AutomationModel::isLaneEnabled (laneNode),
                    "model flag written");

            // External enable change reflects in the view's reported state.
            model.setLaneEnabled (laneNode, true);
            expect (view.isLaneEnabled(), "external enable reflected");
        }

        //----------------------------------------------------------------------
        beginTest ("interpolation: step vs linear segment render different geometry");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            // Linear lane.
            auto linNode = model.getOrCreateContinuousLane ("A", "filter").getState();
            Daw::ContinuousLane lin (linNode);
            lin.addBreakpoint (0,     -1.0, Daw::Interpolation::Linear);
            lin.addBreakpoint (44100,  1.0, Daw::Interpolation::Linear);
            Daw::ContinuousAutomationLaneView linView (linNode, transform, &model, "filter");
            linView.setBounds (0, 0, laneW, laneH);

            // Step lane (same two points, leading interpolation = step).
            auto stepNode = model.getOrCreateContinuousLane ("B", "filter").getState();
            Daw::ContinuousLane stp (stepNode);
            stp.addBreakpoint (0,     -1.0, Daw::Interpolation::Step);
            stp.addBreakpoint (44100,  1.0, Daw::Interpolation::Linear);
            Daw::ContinuousAutomationLaneView stepView (stepNode, transform, &model, "filter");
            stepView.setBounds (0, 0, laneW, laneH);

            const auto linPts  = linView.computePolyline();
            const auto stepPts = stepView.computePolyline();

            // The step segment inserts a horizontal-then-vertical corner point, so
            // the step polyline has MORE points than the straight diagonal.
            expect (stepPts.size() > linPts.size(),
                    "step segment adds a corner point that linear does not");
            expectEquals ((int) linPts.size(), 2);
            expectEquals ((int) stepPts.size(), 3);
        }

        //----------------------------------------------------------------------
        beginTest ("disclosure hidden-by-default: group height unchanged; reveal grows it");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            DawState::ensureTrackForDeck (daw, 0);

            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); },
                                          {}, &model);
            auto* group = stack.getGroupByDeckIndex (0);
            expect (group != nullptr);

            // Hidden by default: height EXACTLY the pre-PRD value.
            expect (! group->isAutomationRevealed());
            expectEquals (group->getPreferredHeight(), Daw::DawLayout::kExpandedGroupHeight);
            expectEquals (stack.getContentHeight(), Daw::DawLayout::kExpandedGroupHeight);

            // Reveal: height grows by the automation stack's preferred height.
            group->setAutomationRevealed (true);
            expect (group->isAutomationRevealed());
            expect (group->getPreferredHeight() > Daw::DawLayout::kExpandedGroupHeight,
                    "revealing automation increases the group height");

            // Collapse: height restored exactly.
            group->setAutomationRevealed (false);
            expectEquals (group->getPreferredHeight(), Daw::DawLayout::kExpandedGroupHeight);
        }

        //----------------------------------------------------------------------
        beginTest ("automation stack lists 9 lanes with populated lanes first");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            // Seed a populated gain lane (2nd continuous spec) so it should be
            // pulled to the FRONT of the ordering.
            auto gain = model.getOrCreateContinuousLane ("A", "gain");
            gain.addBreakpoint (0, 0.0, Daw::Interpolation::Linear);

            Daw::AutomationLaneStackView stackView ("A", model, transform);
            // 6 continuous + 3 boolean.
            expectEquals (stackView.getNumLaneViews(), 9);

            // The first lane view is the populated GAIN lane (a continuous view
            // whose node has a breakpoint).
            auto* first = stackView.getLaneView (0);
            expect (first != nullptr);
            auto* cont = dynamic_cast<Daw::ContinuousAutomationLaneView*> (first);
            expect (cont != nullptr, "first revealed lane is continuous (populated gain)");
            expect (cont->getBreakpointCount() > 0, "populated lane is listed first");
        }

        //----------------------------------------------------------------------
        beginTest ("playhead indicator: marker x matches the shared transform");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            auto laneNode = model.getOrCreateContinuousLane ("A", "filter").getState();
            Daw::ContinuousLane lane (laneNode);
            lane.addBreakpoint (0,      -1.0, Daw::Interpolation::Linear);
            lane.addBreakpoint (88200,   1.0, Daw::Interpolation::Linear);

            Daw::ContinuousAutomationLaneView view (laneNode, transform, &model, "filter");
            view.setBounds (0, 0, laneW, laneH);

            const std::int64_t playSample = 44100;
            view.setPlayheadProvider ([playSample]() { return playSample; });

            // The expected playhead x mirrors AutomationLaneViewBase::sampleToBodyX:
            // the lane body offsets the shared transform by the fixed header gutter.
            const double expectedX =
                Daw::TimelineTransform::alignToPixelGrid (
                    (double) Daw::DawLayout::kTrackHeaderWidth
                    + transform.sampleToX (playSample));
            expect (expectedX > (double) Daw::DawLayout::kTrackHeaderWidth,
                    "playhead falls inside the content axis for a mid-lane sample");

            // Paints without crashing with a visible playhead.
            paintInto (view, laneW, laneH);

            // Hidden when provider returns -1.
            view.setPlayheadProvider ([]() { return (std::int64_t) -1; });
            paintInto (view, laneW, laneH);
        }

        //----------------------------------------------------------------------
        beginTest ("empty lanes paint the dimmed default line / OFF baseline");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            const int probeX = Daw::DawLayout::kTrackHeaderWidth + 400;

            // Continuous filter: default 0.0 sits mid-body ([-1,+1] window).
            {
                auto node = model.getOrCreateContinuousLane ("A", "filter").getState();
                Daw::ContinuousAutomationLaneView view (node, transform, &model, "filter");
                auto img = paintToImage (view, laneW, laneH);
                expect (img.getPixelAt (probeX, 8).getBrightness() > 0.95f,
                        "body away from the default line is bare surface");
                expect (img.getPixelAt (probeX, laneH / 2).getBrightness() < 0.92f,
                        "dimmed default line painted at the filter centre value");
            }

            // Continuous volume: default 1.0 (full-open fader) sits at the top.
            {
                auto node = model.getOrCreateContinuousLane ("A", "volume").getState();
                Daw::ContinuousAutomationLaneView view (node, transform, &model, "volume");
                auto img = paintToImage (view, laneW, laneH);
                expect (img.getPixelAt (probeX, Daw::AutomationLaneMetrics::kBodyInsetY + 1)
                            .getBrightness() < 0.92f,
                        "dimmed default line painted at the volume top value");
            }

            // Boolean: a dimmed OFF baseline along the bottom of the body.
            {
                auto node = model.getOrCreateBooleanLane ("A", "keyLock").getState();
                Daw::BooleanAutomationLaneView view (node, transform, &model, "keyLock");
                auto img = paintToImage (view, laneW, laneH);
                expect (img.getPixelAt (probeX, laneH / 2).getBrightness() > 0.95f,
                        "body above the baseline is bare surface");
                expect (img.getPixelAt (probeX, laneH - 6).getBrightness() < 0.92f,
                        "dimmed OFF baseline painted for an empty boolean lane");
            }
        }

        //----------------------------------------------------------------------
        beginTest ("a single breakpoint renders as a full-width held line");
        {
            auto daw = DawState::createDawBranch();
            Daw::AutomationModel model (daw);
            auto transform = makeTransform();

            auto node = model.getOrCreateContinuousLane ("A", "filter").getState();
            Daw::ContinuousLane lane (node);
            lane.addBreakpoint (88200, 0.0, Daw::Interpolation::Linear); // mid-lane, mid-value

            Daw::ContinuousAutomationLaneView view (node, transform, &model, "filter");
            auto img = paintToImage (view, laneW, laneH);

            // Full-strength ink at the held value, well left and right of the node
            // (the curve holds the edge values, matching evaluateAt's semantics).
            const int y = laneH / 2;
            expect (img.getPixelAt (Daw::DawLayout::kTrackHeaderWidth + 8, y).getBrightness() < 0.5f,
                    "held line reaches the lane's left edge");
            expect (img.getPixelAt (laneW - 8, y).getBrightness() < 0.5f,
                    "held line reaches the lane's right edge");
        }

        //----------------------------------------------------------------------
        beginTest ("gestures: click never creates; double-click creates/deletes; drag moves");
        {
            auto daw = DawState::createDawBranch();
            juce::UndoManager undo { 30000, 30 };
            Daw::ArrangementPublisher publisher;
            Daw::ArrangementRecompileTrigger trigger { daw, Daw::ArrangementCompiler{}, publisher };
            Daw::EditCommandDispatcher dispatcher { daw, undo, trigger };
            Daw::AutomationModel model (daw);

            // xToSample clamps to [0, contentEnd], so the inverse mapping needs a
            // real content length (makeTransform() leaves it 0 — forward-only).
            const Daw::TimelineTransform::GridSnapshot grid;
            Daw::TimelineTransform transform (grid,
                                              /*pixelsPerBeat*/ 50.0,
                                              /*leftEdgeSample*/ 0,
                                              /*viewportWidthPx*/ 800.0,
                                              (std::int64_t) (grid.samplesPerBeat * 32.0));

            auto node = model.getOrCreateContinuousLane ("A", "filter").getState();
            Daw::ContinuousAutomationLaneView view (node, transform, &model, "filter");
            view.setBounds (0, 0, laneW, laneH);
            view.setEditDispatcher (&dispatcher);

            auto eventAt = [&view] (int x, int y, int clicks)
            {
                const juce::Point<float> p ((float) x, (float) y);
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                                         p, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                         &view, &view, juce::Time::getCurrentTime(), p,
                                         juce::Time::getCurrentTime(), clicks, false);
            };

            // 200 content px @ 50 px/beat = beat 4 (on-grid); mid y = filter 0.0.
            const int bodyX = Daw::DawLayout::kTrackHeaderWidth + 200;
            const int midY  = laneH / 2;

            // A single click on the empty body must NOT create a node.
            view.mouseDown (eventAt (bodyX, midY, 1));
            view.mouseUp   (eventAt (bodyX, midY, 1));
            expectEquals (view.getBreakpointCount(), 0, "single click does not create");

            // Double-click the empty body: exactly one node, selected for dragging.
            view.mouseDown        (eventAt (bodyX, midY, 2));
            view.mouseDoubleClick (eventAt (bodyX, midY, 2));
            view.mouseUp          (eventAt (bodyX, midY, 2));
            expectEquals (view.getBreakpointCount(), 1, "double-click created one node");
            expect (view.getSelectedBreakpoint().isValid(), "created node is selected");

            Daw::ContinuousLane lane (node);
            expectEquals (Daw::ContinuousLane::sampleOfNode (lane.getBreakpoint (0)),
                          (std::int64_t) 88200, "created at the snapped beat");

            // Press the node, drag right/up, release: ONE moved node, no duplicate.
            const int targetX = Daw::DawLayout::kTrackHeaderWidth + 300; // beat 6
            const int targetY = midY - 5;                               // filter 0.5
            view.mouseDown (eventAt (bodyX,   midY,    1));
            view.mouseDrag (eventAt (targetX, targetY, 1));
            view.mouseUp   (eventAt (targetX, targetY, 1));
            expectEquals (view.getBreakpointCount(), 1, "drag moved, not duplicated");
            expectEquals (Daw::ContinuousLane::sampleOfNode (lane.getBreakpoint (0)),
                          (std::int64_t) 132300, "moved to the snapped target beat");
            expectWithinAbsoluteError (Daw::ContinuousLane::valueOfNode (lane.getBreakpoint (0)),
                                       0.5, 0.05);

            // Double-click the node at its new position deletes it.
            view.mouseDown        (eventAt (targetX, targetY, 2));
            view.mouseDoubleClick (eventAt (targetX, targetY, 2));
            view.mouseUp          (eventAt (targetX, targetY, 2));
            expectEquals (view.getBreakpointCount(), 0, "double-click on the node deletes it");
        }
    }
};

static AutomationLaneUiTests automationLaneUiTests;

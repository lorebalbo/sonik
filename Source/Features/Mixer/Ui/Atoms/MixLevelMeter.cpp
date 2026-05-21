#include "MixLevelMeter.h"
#include "../../State/MixerMeterSnapshot.h"

#include <atomic>
#include <cmath>

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
    const juce::Colour kHigh    { 0xFFE2E2E2 };   // empty-bar surface

    constexpr int kChassisPad = 2;     // space inside the 2-px border
    constexpr int kBarGap     = 2;     // gap between L and R bars
    constexpr int kClipBoxPx  = 6;     // clip indicator size

    /// Dithered checkerboard: pixels at (x + y) parity below threshold
    /// belong to the "lit" set. Threshold is driven by fill density.
    /// 0.0 → empty (no pixels lit), 1.0 → solid (every pixel lit).
    void fillDithered (juce::Graphics& g,
                       juce::Rectangle<int> area,
                       float density)
    {
        if (area.isEmpty()) return;
        const int x0 = area.getX();
        const int y0 = area.getY();
        const int w  = area.getWidth();
        const int h  = area.getHeight();

        // Density steps from 0..1 control how many of the 8x8 dither
        // cells are filled, plus a final pass to solid as density → 1.
        const int  totalRows = juce::jmax (1, h);
        const int  litRows   = juce::roundToInt (density * static_cast<float> (totalRows));

        // Fill bottom-up.
        g.setColour (kInk);
        for (int row = 0; row < litRows; ++row)
        {
            const int y = y0 + h - 1 - row;
            // Density at this row: lower rows always solid; top rows speckle.
            const float rowDensity = juce::jlimit (0.0f, 1.0f,
                                                    density - row / static_cast<float> (totalRows));
            for (int col = 0; col < w; ++col)
            {
                const int x = x0 + col;
                // Bayer-ish 2x2 ordered dither cutoff.
                const int cellIdx = ((x & 1) << 1) | (y & 1);
                static const float kThresholds[4] = { 0.20f, 0.60f, 0.80f, 0.40f };
                if (rowDensity >= kThresholds[cellIdx])
                    g.fillRect (x, y, 1, 1);
            }
        }
    }
}

//==============================================================================
// Construction / teardown
//==============================================================================
MixLevelMeter::MixLevelMeter (ChannelMeterSlots& slotsIn,
                                juce::String       chassisLabelIn)
    : slots (slotsIn),
      chassisLabel (std::move (chassisLabelIn))
{
    setOpaque (false);
}

MixLevelMeter::~MixLevelMeter()
{
    stopTimer();
}

//==============================================================================
// Polling
//==============================================================================
void MixLevelMeter::pollNow()
{
    cachedPeakL     = slots.levelPeakL    .load (std::memory_order_relaxed);
    cachedPeakR     = slots.levelPeakR    .load (std::memory_order_relaxed);
    cachedPeakHoldL = slots.levelPeakHoldL.load (std::memory_order_relaxed);
    cachedPeakHoldR = slots.levelPeakHoldR.load (std::memory_order_relaxed);
    cachedClip      = slots.clip          .load (std::memory_order_relaxed);
    repaint();
}

void MixLevelMeter::timerCallback()
{
    pollNow();
}

void MixLevelMeter::clearClip()
{
    slots.clearClip();
    cachedClip = false;
    repaint();
}

//==============================================================================
// Component overrides
//==============================================================================
void MixLevelMeter::parentHierarchyChanged()
{
    if (getPeer() != nullptr && isShowing())
    {
        if (! isTimerRunning())
            startTimerHz (kPollHz);
    }
    else
    {
        stopTimer();
    }
}

void MixLevelMeter::visibilityChanged()
{
    if (isShowing())
    {
        if (! isTimerRunning())
            startTimerHz (kPollHz);
    }
    else
    {
        stopTimer();
    }
}

void MixLevelMeter::mouseUp (const juce::MouseEvent& e)
{
    if (getLocalBounds().contains (e.getPosition()))
        clearClip();
}

void MixLevelMeter::resized()
{
    repaint();
}

//==============================================================================
// Linear → dB → fill fraction
//==============================================================================
float MixLevelMeter::linearToDb (float linear) noexcept
{
    if (linear <= 0.0f) return kMinDb;
    return 20.0f * std::log10 (linear);
}

float MixLevelMeter::dbToFillFraction (float db) noexcept
{
    const float clamped = juce::jlimit (kMinDb, kMaxDb, db);
    return (clamped - kMinDb) / (kMaxDb - kMinDb);
}

//==============================================================================
// Paint helpers
//==============================================================================
void MixLevelMeter::paintMeterBar (juce::Graphics& g,
                                     juce::Rectangle<int> area,
                                     float                fill,
                                     float                peakHoldFill) const
{
    // Empty-bar chassis (surface-container-highest) — provides shape
    // before the dither overlay.
    g.setColour (kHigh);
    g.fillRect (area);

    const float clampedFill = juce::jlimit (0.0f, 1.0f, fill);
    const int   litHeight   = juce::roundToInt (clampedFill * static_cast<float> (area.getHeight()));
    if (litHeight > 0)
    {
        auto litRect = juce::Rectangle<int> (area.getX(),
                                              area.getBottom() - litHeight,
                                              area.getWidth(),
                                              litHeight);
        fillDithered (g, litRect, clampedFill);
    }

    // Peak-hold tick — a 1-px ink line at the peak-hold height.
    const float clampedHold = juce::jlimit (0.0f, 1.0f, peakHoldFill);
    if (clampedHold > 0.0f)
    {
        const int holdY = area.getBottom()
                        - juce::roundToInt (clampedHold * static_cast<float> (area.getHeight()));
        g.setColour (kInk);
        g.fillRect (area.getX(), holdY, area.getWidth(), 1);
    }
}

void MixLevelMeter::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();

    // Chassis background and 2-px ink border (DESIGN.md §5).
    g.setColour (kSurface);
    g.fillRect (bounds);
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    auto inner = bounds.reduced (kChassisPad + 2);
    if (inner.isEmpty())
        return;

    // Clip indicator (top-left corner). Lit only when latched.
    {
        auto clipBox = juce::Rectangle<int> (inner.getX(), inner.getY(),
                                              kClipBoxPx, kClipBoxPx);
        if (cachedClip)
        {
            g.setColour (kInk);
            g.fillRect (clipBox);
        }
        else
        {
            g.setColour (kInk);
            g.drawRect (clipBox, 1);
        }
        inner.removeFromTop (kClipBoxPx + 2);
    }

    // Optional chassis label below the bars.
    if (chassisLabel.isNotEmpty())
    {
        auto labelArea = inner.removeFromBottom (10);
        g.setColour (kInk);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      8.0f, juce::Font::plain));
        g.drawText (chassisLabel, labelArea, juce::Justification::centred);
    }

    // Two stereo bars (L on the left, R on the right).
    const int totalW = inner.getWidth();
    const int barW   = (totalW - kBarGap) / 2;
    if (barW <= 0)
        return;

    auto leftBar  = juce::Rectangle<int> (inner.getX(), inner.getY(),
                                           barW, inner.getHeight());
    auto rightBar = juce::Rectangle<int> (inner.getX() + barW + kBarGap,
                                           inner.getY(),
                                           inner.getWidth() - barW - kBarGap,
                                           inner.getHeight());

    const float fillL = dbToFillFraction (linearToDb (cachedPeakL));
    const float fillR = dbToFillFraction (linearToDb (cachedPeakR));
    const float holdL = dbToFillFraction (linearToDb (cachedPeakHoldL));
    const float holdR = dbToFillFraction (linearToDb (cachedPeakHoldR));

    paintMeterBar (g, leftBar,  fillL, holdL);
    paintMeterBar (g, rightBar, fillR, holdR);
}

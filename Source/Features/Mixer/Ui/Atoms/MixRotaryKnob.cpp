#include "MixRotaryKnob.h"

#include <cmath>

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };

    constexpr float kDragRangePixels = 140.0f;    // full sweep per drag
    constexpr float kFineMultiplier  = 0.25f;     // shift = 4× finer
}

//==============================================================================
// Construction / teardown
//==============================================================================
MixRotaryKnob::MixRotaryKnob (juce::ValueTree boundTree,
                              juce::Identifier propertyIdIn,
                              Config           cfg)
    : tree (boundTree),
      propertyId (propertyIdIn),
      config (std::move (cfg))
{
    setOpaque (false);

    if (! config.formatter)
    {
        config.formatter = [this] (float value) { return defaultFormat (value); };
    }

    currentValue = config.defaultValue;
    updateFromTree();
    tree.addListener (this);
}

MixRotaryKnob::~MixRotaryKnob()
{
    tree.removeListener (this);
}

//==============================================================================
// Programmatic API
//==============================================================================
void MixRotaryKnob::setValue (float newValue)
{
    const float lo = juce::jmin (config.minValue, config.maxValue);
    const float hi = juce::jmax (config.minValue, config.maxValue);
    float clamped  = juce::jlimit (lo, hi, newValue);

    if (config.taper == Normalisation::Bipolar
        && std::abs (clamped) < config.bipolarDeadzone)
    {
        clamped = 0.0f;
    }

    currentValue = clamped;
    commitToTree();
    repaint();
}

void MixRotaryKnob::resetToDefault()
{
    setValue (config.defaultValue);
}

void MixRotaryKnob::setKillIndicatorBinding (juce::Identifier killBoolIdIn)
{
    killBoolId = killBoolIdIn;
    repaint();
}

//==============================================================================
// Normalisation
//==============================================================================
float MixRotaryKnob::normalise (float value) const noexcept
{
    switch (config.taper)
    {
        case Normalisation::Linear:
        {
            const float range = config.maxValue - config.minValue;
            if (std::abs (range) < 1.0e-9f) return 0.5f;
            return juce::jlimit (0.0f, 1.0f, (value - config.minValue) / range);
        }
        case Normalisation::DbTapered:
        {
            // Piecewise: 0 dB → 0.5; minValue (dB) → 0; maxValue (dB) → 1
            const float minDb = config.minValue;
            const float maxDb = config.maxValue;
            if (value <= 0.0f)
            {
                if (std::abs (minDb) < 1.0e-9f) return 0.5f;
                return juce::jlimit (0.0f, 0.5f,
                                     (value - minDb) / (0.0f - minDb) * 0.5f);
            }
            if (std::abs (maxDb) < 1.0e-9f) return 0.5f;
            return juce::jlimit (0.5f, 1.0f, 0.5f + (value / maxDb) * 0.5f);
        }
        case Normalisation::Bipolar:
        default:
        {
            // -1..+1 → 0..1, centre 0 → 0.5
            return juce::jlimit (0.0f, 1.0f, (value + 1.0f) * 0.5f);
        }
    }
}

float MixRotaryKnob::denormalise (float norm) const noexcept
{
    const float n = juce::jlimit (0.0f, 1.0f, norm);
    switch (config.taper)
    {
        case Normalisation::Linear:
            return config.minValue + n * (config.maxValue - config.minValue);
        case Normalisation::DbTapered:
            if (n <= 0.5f)
                return config.minValue + (n / 0.5f) * (0.0f - config.minValue);
            return ((n - 0.5f) / 0.5f) * config.maxValue;
        case Normalisation::Bipolar:
        default:
            return n * 2.0f - 1.0f;
    }
}

float MixRotaryKnob::angleFor (float value) const noexcept
{
    return kStartAngle + normalise (value) * (kEndAngle - kStartAngle);
}

juce::String MixRotaryKnob::defaultFormat (float value) const
{
    switch (config.taper)
    {
        case Normalisation::DbTapered:
            if (value <= config.minValue + 0.01f)
                return "-INF";
            return (value >= 0.0f ? "+" : "") + juce::String (value, 1) + " dB";
        case Normalisation::Bipolar:
            return juce::String (value, 2);
        case Normalisation::Linear:
        default:
            return juce::String (value, 2);
    }
}

//==============================================================================
// Painting (DESIGN.md compliance)
//==============================================================================
void MixRotaryKnob::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background fill — surface
    g.fillAll (kSurface);

    const bool isCompact       = config.compact;
    const bool hasBottomLabel  = config.showBottomLabel && ! isCompact;

    // Top label band — shown only in the original full mode.
    if (! isCompact && ! hasBottomLabel)
    {
        auto labelArea = bounds.removeFromTop (kLabelHeight);
        g.setColour (kInk);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      9.0f, juce::Font::plain));
        g.drawText (config.label.toUpperCase(), labelArea,
                    juce::Justification::centred);
    }

    // Bottom display band.
    juce::Rectangle<int> displayArea;
    if (hasBottomLabel)
    {
        displayArea = bounds.removeFromBottom (kLabelBandH);
    }
    else if (! isCompact)
    {
        displayArea = bounds.removeFromBottom (kDisplayHeight);
    }

    // ── Knob (pixel-art: ticks → ring → broken inner arc → needle) ──────────
    auto knobArea = bounds;
    const int knobSize = juce::jmin (knobArea.getWidth(), knobArea.getHeight());

    if (knobSize >= 8)
    {
        knobArea = knobArea.withSizeKeepingCentre (knobSize, knobSize);
        const auto knobRect = knobArea.toFloat();
        const float cx   = knobRect.getCentreX();
        const float cy   = knobRect.getCentreY();
        const float half = knobSize * 0.5f;

        // Proportions traced from the DESIGN.md reference mock: square ticks
        // orbit outside a thick ring; ring, clearance gap, inner arc and
        // needle all share one stroke weight of ~10 % of the ring radius
        // (clamped to 2 px so small knobs stay legible).
        const float tickSize  = juce::jmax (2.0f, std::round (half * 0.11f));
        const float tickOrbit = half - tickSize * 0.75f;
        const float ringOuter = tickOrbit * 0.80f;
        const float stroke    = juce::jmax (2.0f, std::round (ringOuter * 0.105f));

        const float arcRadius  = ringOuter - stroke * 2.5f;   // arc centreline
        const float needleTip  = ringOuter - stroke * 2.0f;   // arc outer edge
        const float angle      = angleFor (currentValue);

        g.setColour (kInk);

        // 11 square ticks, one per 30 degrees across the 300-degree sweep,
        // each rotated so a flat side faces the centre. The bottom 60-degree
        // dead zone stays empty.
        for (int i = 0; i <= 10; ++i)
        {
            const float a = kStartAngle
                          + (kEndAngle - kStartAngle)
                              * (static_cast<float> (i) / 10.0f);
            juce::Path square;
            square.addRectangle (-tickSize * 0.5f, -tickSize * 0.5f,
                                 tickSize, tickSize);
            g.fillPath (square,
                        juce::AffineTransform::rotation (a)
                            .translated (cx + tickOrbit * std::cos (a),
                                         cy + tickOrbit * std::sin (a)));
        }

        // Solid outer ring with a white face inside.
        g.fillEllipse (cx - ringOuter, cy - ringOuter,
                       ringOuter * 2.0f, ringOuter * 2.0f);
        const float face = ringOuter - stroke;
        g.setColour (kSurface);
        g.fillEllipse (cx - face, cy - face, face * 2.0f, face * 2.0f);

        // Inner arc: a full circle except for a gap that travels with the
        // needle, so the needle always points through open space.
        {
            // juce::Path::addCentredArc treats 0 = 12 o'clock (CW), while the
            // needle maths uses 0 = 3 o'clock (standard trig); halfPi aligns.
            constexpr float arcAngleOffset = juce::MathConstants<float>::halfPi;
            const float gapHalf = juce::degreesToRadians (30.0f);

            juce::Path arc;
            arc.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                               angle + arcAngleOffset + gapHalf,
                               angle + arcAngleOffset
                                     + juce::MathConstants<float>::twoPi - gapHalf,
                               true);
            g.setColour (kInk);
            g.strokePath (arc, juce::PathStrokeType (stroke,
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::butt));
        }

        // Needle: straight line from the centre out through the arc gap.
        g.setColour (kInk);
        g.drawLine (cx, cy,
                    cx + needleTip * std::cos (angle),
                    cy + needleTip * std::sin (angle),
                    stroke);
    }

    // ── Bottom display / label ───────────────────────────────────────────────
    if (displayArea.isEmpty())
        return;

    g.setColour (kInk);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  9.0f, juce::Font::plain));

    if (hasBottomLabel)
    {
        // Show the knob's logical name — no dB value.
        g.drawText (config.label.toUpperCase(), displayArea,
                    juce::Justification::centred);
        return;
    }

    // Original full mode: show live dB / bipolar value (or KILL overlay).
    bool killActive = false;
    if (killBoolId.toString().isNotEmpty() && tree.isValid())
        killActive = static_cast<bool> (tree.getProperty (killBoolId, false));

    const juce::String displayText = killActive
        ? juce::String ("KILL")
        : config.formatter (currentValue);

    g.drawText (displayText, displayArea, juce::Justification::centred);
}
void MixRotaryKnob::resized()
{
    repaint();
}

//==============================================================================
// Mouse interaction
//==============================================================================
void MixRotaryKnob::mouseDown (const juce::MouseEvent& e)
{
    isDragging   = true;
    dragStartY   = static_cast<float> (e.y);
    dragStartVal = currentValue;
}

void MixRotaryKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging)
        return;

    const float dy = dragStartY - static_cast<float> (e.y);   // up = increase
    const float sensitivity = (e.mods.isShiftDown() ? kFineMultiplier : 1.0f);
    const float normDelta   = (dy / kDragRangePixels) * sensitivity;

    const float startNorm = normalise (dragStartVal);
    const float newNorm   = juce::jlimit (0.0f, 1.0f, startNorm + normDelta);

    float newVal = denormalise (newNorm);

    if (config.taper == Normalisation::Bipolar
        && std::abs (newVal) < config.bipolarDeadzone)
    {
        newVal = 0.0f;
    }

    if (! juce::exactlyEqual (newVal, currentValue))
    {
        currentValue = newVal;
        commitToTree();
        repaint();
    }
}

void MixRotaryKnob::mouseUp (const juce::MouseEvent&)
{
    isDragging = false;
}

void MixRotaryKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    resetToDefault();
}

void MixRotaryKnob::mouseWheelMove (const juce::MouseEvent& e,
                                     const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f)
        return;

    const float step = config.wheelIncrement
                     * (e.mods.isShiftDown() ? kFineMultiplier : 1.0f);

    const float delta = (wheel.deltaY > 0.0f ?  step : -step);
    setValue (currentValue + delta);
}

//==============================================================================
// State sync
//==============================================================================
void MixRotaryKnob::commitToTree()
{
    if (! tree.isValid())
        return;
    tree.setProperty (propertyId, currentValue, nullptr);
}

void MixRotaryKnob::updateFromTree()
{
    if (! tree.isValid())
        return;

    if (tree.hasProperty (propertyId))
    {
        const float v = static_cast<float> (
            static_cast<double> (tree.getProperty (propertyId, currentValue)));
        const float lo = juce::jmin (config.minValue, config.maxValue);
        const float hi = juce::jmax (config.minValue, config.maxValue);
        currentValue = juce::jlimit (lo, hi, v);
    }

    repaint();
}

void MixRotaryKnob::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                               const juce::Identifier& property)
{
    if (changedTree != tree)
        return;
    if (property != propertyId && property != killBoolId)
        return;

    if (property == propertyId && isDragging)
        return;

    updateFromTree();
}

#include "MixFader.h"

#include <cmath>

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
    const juce::Colour kHigh    { 0xFFE2E2E2 };   // surface-container-highest
    const juce::Colour kShadow  { 0xBF2D2D2D };   // 75% opacity ink

    constexpr float kWheelStep = 0.02f;
}

//==============================================================================
MixFader::MixFader (juce::ValueTree boundTree,
                     juce::Identifier propertyIdIn,
                     Config           cfg)
    : tree (boundTree),
      propertyId (propertyIdIn),
      config (cfg)
{
    setOpaque (false);
    currentValue = config.defaultValue;
    readFromTree();
    tree.addListener (this);
}

MixFader::~MixFader()
{
    tree.removeListener (this);
}

//==============================================================================
void MixFader::setValue (float newValue)
{
    const float v = clampAndDetent (newValue);
    if (! juce::exactlyEqual (v, currentValue))
    {
        currentValue = v;
        commitToTree();
        repaint();
    }
}

void MixFader::resetToDefault()
{
    setValue (config.defaultValue);
}

float MixFader::clampAndDetent (float v) const noexcept
{
    const float lo = juce::jmin (config.minValue, config.maxValue);
    const float hi = juce::jmax (config.minValue, config.maxValue);
    float c = juce::jlimit (lo, hi, v);

    if (config.detentValue.has_value())
    {
        const float dz = config.detentDeadzone * (hi - lo);
        if (std::abs (c - *config.detentValue) <= dz)
            c = *config.detentValue;
    }
    return c;
}

float MixFader::normalise (float v) const noexcept
{
    const float range = config.maxValue - config.minValue;
    if (std::abs (range) < 1.0e-9f) return 0.5f;
    return juce::jlimit (0.0f, 1.0f, (v - config.minValue) / range);
}

float MixFader::denormalise (float n) const noexcept
{
    const float c = juce::jlimit (0.0f, 1.0f, n);
    return config.minValue + c * (config.maxValue - config.minValue);
}

//==============================================================================
juce::Rectangle<int> MixFader::getTrackArea() const
{
    auto bounds = getLocalBounds().reduced (kTrackPad);
    if (config.orientation == Orientation::Vertical)
    {
        // 6-px wide track centred horizontally.
        constexpr int trackWidth = 6;
        const int x = bounds.getX() + (bounds.getWidth() - trackWidth) / 2;
        return { x, bounds.getY() + kCapThickness / 2,
                 trackWidth, bounds.getHeight() - kCapThickness };
    }
    constexpr int trackHeight = 6;
    const int y = bounds.getY() + (bounds.getHeight() - trackHeight) / 2;
    return { bounds.getX() + kCapThickness / 2, y,
             bounds.getWidth() - kCapThickness, trackHeight };
}

juce::Rectangle<int> MixFader::getCapArea() const
{
    auto bounds = getLocalBounds().reduced (kTrackPad);
    const float n = normalise (currentValue);

    if (config.orientation == Orientation::Vertical)
    {
        // Vertical: value 1.0 → top (small y), value 0.0 → bottom.
        // (If invertVertical is true, the convention flips.)
        const float effectiveN = config.invertVertical ? n : (1.0f - n);
        const int   capW = bounds.getWidth();
        const int   trackY    = bounds.getY() + kCapThickness / 2;
        const int   trackH    = bounds.getHeight() - kCapThickness;
        const int   capCentre = trackY + juce::roundToInt (effectiveN * trackH);
        return { bounds.getX(), capCentre - kCapThickness / 2,
                 capW, kCapThickness };
    }

    const int trackX = bounds.getX() + kCapThickness / 2;
    const int trackW = bounds.getWidth() - kCapThickness;
    const int capCentre = trackX + juce::roundToInt (n * trackW);
    const int capH = bounds.getHeight();
    return { capCentre - kCapThickness / 2, bounds.getY(),
             kCapThickness, capH };
}

//==============================================================================
void MixFader::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();

    // Chassis background — surface-container-highest.
    g.setColour (kHigh);
    g.fillRect (bounds);

    // 2-px ink border.
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    // Track.
    const auto track = getTrackArea();
    g.setColour (kInk);
    g.fillRect (track);

    // Detent tick (if any) — small ink mark on the track.
    if (config.detentValue.has_value())
    {
        const float n = normalise (*config.detentValue);
        g.setColour (kSurface);
        if (config.orientation == Orientation::Vertical)
        {
            const float effN = config.invertVertical ? n : (1.0f - n);
            const int y = track.getY() + juce::roundToInt (effN * track.getHeight());
            g.fillRect (track.getX(), y - 1, track.getWidth(), 2);
        }
        else
        {
            const int x = track.getX() + juce::roundToInt (n * track.getWidth());
            g.fillRect (x - 1, track.getY(), 2, track.getHeight());
        }
    }

    // Cap drop shadow.
    const auto cap = getCapArea();
    g.setColour (kShadow);
    g.fillRect (cap.translated (2, 2));

    // Cap — solid ink block.
    g.setColour (kInk);
    g.fillRect (cap);

    // 1-px white stripe across the cap, perpendicular to fader axis.
    g.setColour (kSurface);
    if (config.orientation == Orientation::Vertical)
    {
        const int midY = cap.getCentreY();
        g.fillRect (cap.getX() + 2, midY, cap.getWidth() - 4, 1);
    }
    else
    {
        const int midX = cap.getCentreX();
        g.fillRect (midX, cap.getY() + 2, 1, cap.getHeight() - 4);
    }
}

void MixFader::resized()
{
    repaint();
}

//==============================================================================
void MixFader::mouseDown (const juce::MouseEvent& e)
{
    isDragging = true;
    dragStartPos = (config.orientation == Orientation::Vertical)
                      ? static_cast<float> (e.y)
                      : static_cast<float> (e.x);
    dragStartVal = currentValue;
}

void MixFader::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging) return;

    const auto track = getTrackArea();
    const float currentPos = (config.orientation == Orientation::Vertical)
                                ? static_cast<float> (e.y)
                                : static_cast<float> (e.x);
    const float trackLen = (config.orientation == Orientation::Vertical)
                              ? static_cast<float> (track.getHeight())
                              : static_cast<float> (track.getWidth());
    if (trackLen <= 0.0f) return;

    float delta = (currentPos - dragStartPos) / trackLen;
    if (config.orientation == Orientation::Vertical && ! config.invertVertical)
        delta = -delta;   // up = increase

    const float startNorm = normalise (dragStartVal);
    const float newNorm = juce::jlimit (0.0f, 1.0f, startNorm + delta);
    setValue (denormalise (newNorm));
}

void MixFader::mouseUp (const juce::MouseEvent&)
{
    isDragging = false;
}

void MixFader::mouseDoubleClick (const juce::MouseEvent&)
{
    resetToDefault();
}

void MixFader::mouseWheelMove (const juce::MouseEvent&,
                                 const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f) return;
    const float currentNorm = normalise (currentValue);
    const float newNorm = juce::jlimit (0.0f, 1.0f,
                                          currentNorm + (wheel.deltaY > 0.0f ? kWheelStep : -kWheelStep));
    setValue (denormalise (newNorm));
}

//==============================================================================
void MixFader::commitToTree()
{
    if (! tree.isValid()) return;
    tree.setProperty (propertyId, currentValue, nullptr);
}

void MixFader::readFromTree()
{
    if (! tree.isValid()) return;
    if (tree.hasProperty (propertyId))
    {
        currentValue = clampAndDetent (
            static_cast<float> (static_cast<double> (tree.getProperty (propertyId))));
    }
    else
    {
        commitToTree();
    }
}

void MixFader::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                          const juce::Identifier& property)
{
    if (changedTree != tree || property != propertyId)
        return;
    const float incoming = static_cast<float> (
        static_cast<double> (tree.getProperty (propertyId)));
    if (! juce::exactlyEqual (incoming, currentValue))
    {
        currentValue = clampAndDetent (incoming);
        repaint();
    }
}

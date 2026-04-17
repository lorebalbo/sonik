#include "GainKnobComponent.h"
#include <cmath>

GainKnobComponent::GainKnobComponent (juce::ValueTree deckTree)
    : tree (deckTree)
{
    setOpaque (false);
    updateFromState();
    tree.addListener (this);
}

GainKnobComponent::~GainKnobComponent()
{
    tree.removeListener (this);
}

float GainKnobComponent::getNormalizedValue() const
{
    return dbToNormalized (gainDb);
}

// --- Conversions ---

float GainKnobComponent::dbToLinear (float db) const
{
    if (db <= minDb)
        return 0.0f;
    return std::pow (10.0f, db / 20.0f);
}

float GainKnobComponent::linearToDb (float linear) const
{
    if (linear <= 0.0f)
        return minDb;
    float db = 20.0f * std::log10 (linear);
    return juce::jmax (minDb, db);
}

float GainKnobComponent::dbToNormalized (float db) const
{
    // Piecewise mapping: 0 dB → normalized 0.5 (12 o'clock)
    //   norm 0.0 = minDb (-60 dB, -inf), norm 0.5 = 0 dB, norm 1.0 = maxDb (+12 dB)
    if (db <= 0.0f)
        return juce::jlimit (0.0f, 0.5f, (db - minDb) / (0.0f - minDb) * 0.5f);
    else
        return juce::jlimit (0.5f, 1.0f, 0.5f + (db / maxDb) * 0.5f);
}

float GainKnobComponent::normalizedToDb (float norm) const
{
    float n = juce::jlimit (0.0f, 1.0f, norm);
    if (n <= 0.5f)
        return minDb + (n / 0.5f) * (0.0f - minDb);
    else
        return ((n - 0.5f) / 0.5f) * maxDb;
}

float GainKnobComponent::dbToAngle (float db) const
{
    float norm = dbToNormalized (db);
    return startAngle + norm * (endAngle - startAngle);
}

// --- Paint ---

void GainKnobComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // "GAIN" label at top
    {
        auto labelArea = bounds.removeFromTop (static_cast<int> (labelHeight));
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (9.0f).withStyle ("Bold"));
        g.drawText ("GAIN", labelArea, juce::Justification::centred);
    }

    // dB display at bottom
    auto displayArea = bounds.removeFromBottom (static_cast<int> (displayHeight));

    // Knob area
    auto knobBounds = bounds.reduced (4);
    int knobSize = juce::jmin (knobBounds.getWidth(), knobBounds.getHeight());
    auto knobRect = knobBounds.withSizeKeepingCentre (knobSize, knobSize).toFloat();
    float cx = knobRect.getCentreX();
    float cy = knobRect.getCentreY();
    float radius = knobSize / 2.0f - 2.0f;

    // Outer ring (pixel-art style: just a square outline)
    g.setColour (juce::Colour (0xFFE2E2E2)); // surface-container-highest
    g.fillRect (knobRect.toNearestInt());

    // Inner knob face
    auto innerRect = knobRect.reduced (3.0f);
    g.setColour (juce::Colour (0xFFF3F3F4));
    g.fillRect (innerRect.toNearestInt());

    // Border
    g.setColour (juce::Colours::black);
    g.drawRect (knobRect.toNearestInt(), 1);

    // Arc track (background arc)
    {
        juce::Path arcBg;
        float arcRadius = radius - 4.0f;
        arcBg.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                             startAngle, endAngle, true);
        g.setColour (juce::Colour (0xFFCCCCCC));
        g.strokePath (arcBg, juce::PathStrokeType (2.0f));
    }

    // Arc value (filled portion)
    {
        float currentAngle = dbToAngle (gainDb);
        // Draw from 0 dB position to current, or from start to current
        float zeroAngle = dbToAngle (0.0f);

        juce::Path arcVal;
        float arcRadius = radius - 4.0f;

        if (gainDb >= 0.0f)
        {
            arcVal.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                                  zeroAngle, currentAngle, true);
        }
        else
        {
            arcVal.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                                  currentAngle, zeroAngle, true);
        }

        g.setColour (juce::Colours::black);
        g.strokePath (arcVal, juce::PathStrokeType (2.0f));
    }

    // Position indicator line
    {
        float angle = dbToAngle (gainDb);
        float lineInner = radius * 0.35f;
        float lineOuter = radius - 5.0f;

        float x1 = cx + lineInner * std::cos (angle);
        float y1 = cy + lineInner * std::sin (angle);
        float x2 = cx + lineOuter * std::cos (angle);
        float y2 = cy + lineOuter * std::sin (angle);

        g.setColour (juce::Colours::black);
        g.drawLine (x1, y1, x2, y2, 2.0f);
    }

    // Center detent mark (1px white line at 0 dB position)
    {
        float zeroAngle = dbToAngle (0.0f);
        float markInner = radius - 1.0f;
        float markOuter = radius + 2.0f;

        float mx1 = cx + markInner * std::cos (zeroAngle);
        float my1 = cy + markInner * std::sin (zeroAngle);
        float mx2 = cx + markOuter * std::cos (zeroAngle);
        float my2 = cy + markOuter * std::sin (zeroAngle);

        g.setColour (juce::Colours::white);
        g.drawLine (mx1, my1, mx2, my2, 1.0f);
    }

    // dB display
    {
        juce::String dbText;
        if (gainDb <= minDb)
            dbText = "-inf";
        else
            dbText = juce::String (gainDb, 1) + " dB";

        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
        g.drawText (dbText, displayArea, juce::Justification::centred);
    }
}

void GainKnobComponent::resized()
{
    repaint();
}

// --- Mouse interaction ---

void GainKnobComponent::mouseDown (const juce::MouseEvent& e)
{
    isDragging = true;
    dragStartY = static_cast<float> (e.y);
    dragStartDb = gainDb;
}

void GainKnobComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging)
        return;

    float dy = dragStartY - static_cast<float> (e.y); // up = increase
    float sensitivity = (maxDb - minDb) / static_cast<float> (getHeight());
    float newDb = dragStartDb + dy * sensitivity;
    newDb = juce::jlimit (minDb, maxDb, newDb);

    setGainDb (newDb);
}

void GainKnobComponent::mouseUp (const juce::MouseEvent&)
{
    isDragging = false;
}

void GainKnobComponent::mouseDoubleClick (const juce::MouseEvent&)
{
    setGainDb (defaultDb);
}

void GainKnobComponent::mouseWheelMove (const juce::MouseEvent&,
                                         const juce::MouseWheelDetails& wheel)
{
    float delta = 0.0f;
    if (wheel.deltaY > 0.0f)
        delta = wheelIncrement;
    else if (wheel.deltaY < 0.0f)
        delta = -wheelIncrement;
    else
        return;

    float newDb = juce::jlimit (minDb, maxDb, gainDb + delta);
    setGainDb (newDb);
}

// --- State management ---

void GainKnobComponent::setGainDb (float newDb)
{
    gainDb = newDb;
    commitToState();
    repaint();
}

void GainKnobComponent::commitToState()
{
    if (! tree.isValid())
        return;

    float linear = dbToLinear (gainDb);
    tree.setProperty (IDs::gain, linear, nullptr);
}

void GainKnobComponent::updateFromState()
{
    if (! tree.isValid())
        return;

    float linear = static_cast<float> (tree.getProperty (IDs::gain, 1.0f));
    gainDb = linearToDb (linear);
    repaint();
}

void GainKnobComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                    const juce::Identifier& property)
{
    if (changedTree != tree)
        return;

    if (property == IDs::gain)
    {
        if (isDragging)
            return;

        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
                safeThis->updateFromState();
        });
    }
}

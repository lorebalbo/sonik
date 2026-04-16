#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::UnitTestRunner runner;
    runner.runTestsInCategory ("Sonik");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        auto* result = runner.getResult (i);
        if (result != nullptr)
            failures += result->failures;
    }

    return failures > 0 ? 1 : 0;
}

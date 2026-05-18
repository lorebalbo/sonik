#include "MidiSettingsPanel.h"

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kBg     = 0xFFFDFDFD;
        constexpr juce::uint32 kBorder = 0xFF2D2D2D;

        constexpr int kRowGap       = 8;
        constexpr int kOuterPad     = 12;
        constexpr int kEmptyHeight  = 80;
    }

    //==========================================================================
    // Content
    //==========================================================================
    void MidiSettingsPanel::Content::resized()
    {
        auto bounds = getLocalBounds().reduced (kOuterPad);
        int y = bounds.getY();
        int totalHeight = kOuterPad;
        for (int i = 0; i < getNumChildComponents(); ++i)
        {
            if (auto* c = getChildComponent (i))
            {
                const int h = juce::jmax (60, ((DeviceHeader*) c)->getPreferredHeight());
                c->setBounds (bounds.getX(), y, bounds.getWidth(), h);
                y += h + kRowGap;
                totalHeight += h + kRowGap;
            }
        }
        totalHeight += kOuterPad - kRowGap;

        // If our height assumption is off (e.g. mapping size changed), update.
        if (getNumChildComponents() > 0 && getHeight() != totalHeight)
            setSize (getWidth(), juce::jmax (kEmptyHeight, totalHeight));
    }

    void MidiSettingsPanel::Content::setHeaders (std::vector<std::unique_ptr<DeviceHeader>>& src)
    {
        removeAllChildren();
        for (auto& h : src)
            if (h != nullptr)
                addAndMakeVisible (*h);

        int total = kOuterPad * 2;
        for (auto& h : src)
            if (h != nullptr)
                total += juce::jmax (60, h->getPreferredHeight()) + kRowGap;
        if (! src.empty())
            total -= kRowGap;

        setSize (getWidth() > 0 ? getWidth() : 800,
                 juce::jmax (kEmptyHeight, total));
        resized();
    }

    //==========================================================================
    // MidiSettingsPanel
    //==========================================================================
    MidiSettingsPanel::MidiSettingsPanel (MappingStore&        s,
                                          MidiDeviceManager&   dm,
                                          MidiInboundRouter&   r,
                                          SoftTakeoverManager& st)
        : store               (s),
          deviceManager       (dm),
          inboundRouter       (r),
          softTakeoverManager (st)
    {
        juce::ignoreUnused (inboundRouter, softTakeoverManager);

        loadErrorBanner.setVisible (false);
        loadErrorBanner.onReload = [this]() { store.reloadUserMappings(); };
        addAndMakeVisible (loadErrorBanner);

        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);

        deviceManager.addDeviceListChangeListener (this);
        store        .addListener                 (this);

        refreshLoadErrorBanner();
        rebuildDeviceList();
    }

    MidiSettingsPanel::~MidiSettingsPanel()
    {
        store        .removeListener                 (this);
        deviceManager.removeDeviceListChangeListener (this);
    }

    //--------------------------------------------------------------------------
    void MidiSettingsPanel::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (kBg));

        g.setColour (juce::Colour (kBorder));
        g.drawRect (getLocalBounds(), 2);
    }

    void MidiSettingsPanel::resized()
    {
        auto inner = getLocalBounds().reduced (2);
        const int bannerH = loadErrorBanner.isVisible() ? loadErrorBanner.getPreferredHeight() : 0;
        if (bannerH > 0)
            loadErrorBanner.setBounds (inner.removeFromTop (bannerH));
        else
            loadErrorBanner.setBounds ({});

        viewport.setBounds (inner);
        // Re-fit the content's width to the viewport's interior so child
        // headers stretch horizontally.
        content.setSize (viewport.getMaximumVisibleWidth(), content.getHeight());
        content.resized();
    }

    void MidiSettingsPanel::refreshLoadErrorBanner()
    {
        loadErrorBanner.setErrors (store.getLoadErrors());
        resized();
    }

    //--------------------------------------------------------------------------
    void MidiSettingsPanel::rebuildDeviceList()
    {
        // Collect input devices only — output-only devices have no inbound
        // bindings to configure in Phase 3.
        const auto records = deviceManager.getDevices();

        headers.clear();
        for (const auto& rec : records)
        {
            if (! rec.isInput)
                continue;
            headers.push_back (std::make_unique<DeviceHeader> (rec, store, deviceManager, inboundRouter, softTakeoverManager));
        }

        content.setHeaders (headers);
        resized();
    }

    void MidiSettingsPanel::rebuildOnMessageThread()
    {
        juce::Component::SafePointer<MidiSettingsPanel> safe { this };
        juce::MessageManager::callAsync ([safe]()
        {
            if (safe != nullptr)
                safe->rebuildDeviceList();
        });
    }

    //--------------------------------------------------------------------------
    // DeviceListChangeListener
    //--------------------------------------------------------------------------
    void MidiSettingsPanel::midiDeviceAdded   (std::uint64_t) { rebuildOnMessageThread(); }
    void MidiSettingsPanel::midiDeviceRemoved (std::uint64_t) { rebuildOnMessageThread(); }
    void MidiSettingsPanel::midiDeviceOpened  (std::uint64_t) { rebuildOnMessageThread(); }
    void MidiSettingsPanel::midiDeviceClosed  (std::uint64_t) { rebuildOnMessageThread(); }

    //--------------------------------------------------------------------------
    // MappingStoreListener
    //--------------------------------------------------------------------------
    void MidiSettingsPanel::userProfilesLoaded()
    {
        refreshLoadErrorBanner();
        for (auto& h : headers)
            if (h != nullptr)
                h->refreshFromStore();
    }

    void MidiSettingsPanel::activeMappingChanged (std::uint64_t deviceId)
    {
        for (auto& h : headers)
            if (h != nullptr && h->getDeviceId() == deviceId)
                h->refreshFromStore();
    }

    void MidiSettingsPanel::mappingAdded (juce::String)
    {
        for (auto& h : headers)
            if (h != nullptr)
                h->refreshFromStore();
    }

    void MidiSettingsPanel::mappingRemoved (juce::String)
    {
        for (auto& h : headers)
            if (h != nullptr)
                h->refreshFromStore();
    }
}

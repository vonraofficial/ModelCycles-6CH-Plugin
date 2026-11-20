[cpp]
/*
  ==============================================================================
    Model:Cycles 6CH - Companion VST3 / AU Plugin
    Open-source utility for Elektron Model:Cycles 6-Channel USB Audio mod
    (Modded-Cycles / ms-multi-output).

    Features:
      - CC 7  (Volume + Dist): 0..64 attenuates -inf..0 dB (hardware-like taper),
                               64..127 stays at 0 dB unity so hardware overdrive
                               in the 6CH stem passes through cleanly.
      - CC 94 (Track Mute):    0 = unmuted, >0 = smooth click-free mute.
      - CC 10 (Pan):           0..127 (64 = exact center, 0 dB unity pan law).
      - CC 95 (Track Level):   Optional toggleable attenuation (0..127).
      - Dual MIDI Input Mode:  "DAW MIDI" (routed from DAW track) OR
                               "Direct Hardware MIDI" (shared singleton hub so
                               all 6 track instances read Model:Cycles directly
                               with zero DAW MIDI routing).
      - Compatible with VST3 (macOS/Windows/Linux) & AU (macOS Logic/GarageBand).
  ==============================================================================
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

#ifndef JucePlugin_Name
 #define JucePlugin_Name "ModelCycles 6CH"
#endif

namespace mc6ch
{

// ==============================================================================
// Cross-version JUCE 7 / JUCE 8 / JUCE 9 warning-free font helper
// ==============================================================================
inline juce::Font makeFont (float height, bool bold = false)
{
   #if JUCE_MAJOR_VERSION >= 8
    auto options = juce::FontOptions (height);
    if (bold)
        options = options.withStyle ("Bold");
    return juce::Font (options);
   #else
    return juce::Font (height, bold ? juce::Font::bold : juce::Font::plain);
   #endif
}

// ==============================================================================
// Elektron-inspired Minimal Palette
// ==============================================================================
namespace Palette
{
    static const juce::Colour bgMain       { 0xff15171c };
    static const juce::Colour bgPanel      { 0xff1e2128 };
    static const juce::Colour bgControl    { 0xff282c36 };
    static const juce::Colour border       { 0xff343946 };
    static const juce::Colour accentOrange { 0xffff6a1a }; // Elektron Vol+Dist / Active Track
    static const juce::Colour accentCyan   { 0xff38bdf8 }; // Pan
    static const juce::Colour accentGreen  { 0xff22c55e }; // MIDI LED / Meter
    static const juce::Colour accentRed    { 0xffef4444 }; // Mute / Clip
    static const juce::Colour textPrimary  { 0xfff1f5f9 };
    static const juce::Colour textMuted    { 0xff94a3b8 };
}

// ==============================================================================
// Parameter IDs (with version hint = 1 for AU compatibility)
// ==============================================================================
namespace ParamIDs
{
    static constexpr const char* trackChannel  = "trackChannel";
    static constexpr const char* volDist       = "volDist";
    static constexpr const char* trackLevel    = "trackLevel";
    static constexpr const char* useTrackLevel = "useTrackLevel";
    static constexpr const char* mute          = "mute";
    static constexpr const char* pan           = "pan";
    static constexpr const char* inputMode     = "inputMode";
}

// ==============================================================================
// Shared Lock-Free Hardware MIDI Hub
// Allows all 6 plugin instances inside the DAW to share ONE hardware MIDI port
// without single-client port conflicts on Windows or duplicate listeners on Mac.
// ==============================================================================
class DirectMidiHub final : public juce::MidiInputCallback
{
public:
    struct ChannelCCState
    {
        std::atomic<int>      cc7Val  { 64 };
        std::atomic<uint32_t> cc7Seq  { 0 };

        std::atomic<int>      cc95Val { 127 };
        std::atomic<uint32_t> cc95Seq { 0 };

        std::atomic<int>      cc94Val { 0 };
        std::atomic<uint32_t> cc94Seq { 0 };

        std::atomic<int>      cc10Val { 64 };
        std::atomic<uint32_t> cc10Seq { 0 };
    };

    DirectMidiHub() = default;

    ~DirectMidiHub() override
    {
        closeDevice();
    }

    // Opens a hardware MIDI device by exact name (called from message thread only)
    bool openDeviceByName (const juce::String& targetName)
    {
        const juce::ScopedLock sl (deviceLock);

        if (targetName.isEmpty() || targetName == "DAW MIDI")
            return false;

        if (midiInput != nullptr && currentDeviceName == targetName)
            return true;

        closeDeviceUnlocked();

        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& dev : devices)
        {
            if (dev.name == targetName)
            {
                auto opened = juce::MidiInput::openDevice (dev.identifier, this);
                if (opened != nullptr)
                {
                    opened->start();
                    midiInput = std::move (opened);
                    currentDeviceName = dev.name;
                    isOpen.store (true, std::memory_order_release);
                    return true;
                }
            }
        }

        return false;
    }

    void closeDevice()
    {
        const juce::ScopedLock sl (deviceLock);
        closeDeviceUnlocked();
    }

    bool isDeviceOpen (const juce::String& name) const
    {
        const juce::ScopedLock sl (deviceLock);
        return isOpen.load (std::memory_order_acquire) && currentDeviceName == name;
    }

    juce::String getCurrentDeviceName() const
    {
        const juce::ScopedLock sl (deviceLock);
        return currentDeviceName;
    }

    // Lock-free access for audio thread (chIndex is 0..15)
    const ChannelCCState& getChannelState (int chIndex) const noexcept
    {
        return channels[static_cast<size_t> (juce::jlimit (0, 15, chIndex))];
    }

    // MidiInputCallback (runs on OS MIDI thread - strictly lock-free & allocation-free)
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override
    {
        if (! message.isController())
            return;

        const int ch = message.getChannel() - 1; // 0..15
        if (ch < 0 || ch >= 16)
            return;

        const int cc  = message.getControllerNumber();
        const int val = juce::jlimit (0, 127, message.getControllerValue());
        auto& st = channels[static_cast<size_t> (ch)];

        if (cc == 7)
        {
            st.cc7Val.store (val, std::memory_order_relaxed);
            st.cc7Seq.fetch_add (1, std::memory_order_release);
        }
        else if (cc == 95)
        {
            st.cc95Val.store (val, std::memory_order_relaxed);
            st.cc95Seq.fetch_add (1, std::memory_order_release);
        }
        else if (cc == 94)
        {
            st.cc94Val.store (val, std::memory_order_relaxed);
            st.cc94Seq.fetch_add (1, std::memory_order_release);
        }
        else if (cc == 10)
        {
            st.cc10Val.store (val, std::memory_order_relaxed);
            st.cc10Seq.fetch_add (1, std::memory_order_release);
        }
    }

private:
    void closeDeviceUnlocked()
    {
        isOpen.store (false, std::memory_order_release);
        if (midiInput != nullptr)
        {
            midiInput->stop();
            midiInput.reset();
        }
        currentDeviceName.clear();
    }

    mutable juce::CriticalSection deviceLock;
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::String currentDeviceName;
    std::atomic<bool> isOpen { false };
    std::array<ChannelCCState, 16> channels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DirectMidiHub)
};

// ==============================================================================
// Custom Minimal Elektron-Inspired LookAndFeel
// ==============================================================================
class MinimalLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MinimalLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, Palette::bgControl);
        setColour (juce::ComboBox::outlineColourId,    Palette::border);
        setColour (juce::ComboBox::textColourId,       Palette::textPrimary);
        setColour (juce::ComboBox::arrowColourId,      Palette::textMuted);

        setColour (juce::PopupMenu::backgroundColourId,            Palette::bgPanel);
        setColour (juce::PopupMenu::textColourId,                  Palette::textPrimary);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::accentOrange);
        setColour (juce::PopupMenu::highlightedTextColourId,       Palette::bgMain);
    }

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPosProportional,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<float> (static_cast<float> (x),
                                                    static_cast<float> (y),
                                                    static_cast<float> (width),
                                                    static_cast<float> (height)).reduced (6.0f);

        const float radius  = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre  = bounds.getCentre();
        const float angle   = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        const float arcRad  = radius - 3.0f;
        const float arcW    = 3.5f;

        const bool enabled   = slider.isEnabled();
        const auto accent    = slider.findColour (juce::Slider::rotarySliderFillColourId);
        const auto activeCol = enabled ? accent : Palette::border;

        // Background track arc
        juce::Path bgArc;
        bgArc.addCentredArc (centre.x, centre.y, arcRad, arcRad, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Palette::bgControl);
        g.strokePath (bgArc, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const bool isBipolar   = static_cast<bool> (slider.getProperties().getWithDefault ("bipolar", false));
        const bool showMidTick = static_cast<bool> (slider.getProperties().getWithDefault ("midTick", false));

        const float midAngle = rotaryStartAngle + (64.0f / 127.0f) * (rotaryEndAngle - rotaryStartAngle);

        // Active value arc
        juce::Path valArc;
        if (isBipolar)
        {
            const float a1 = juce::jmin (midAngle, angle);
            const float a2 = juce::jmax (midAngle, angle);
            if (std::abs (a2 - a1) > 0.01f)
                valArc.addCentredArc (centre.x, centre.y, arcRad, arcRad, 0.0f, a1, a2, true);
        }
        else
        {
            if (sliderPosProportional > 0.005f)
                valArc.addCentredArc (centre.x, centre.y, arcRad, arcRad, 0.0f,
                                      rotaryStartAngle, angle, true);
        }

        g.setColour (activeCol);
        g.strokePath (valArc, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Unity / Center reference tick at value 64 (12 o'clock)
        if (showMidTick)
        {
            const float tickR1 = arcRad + 3.0f;
            const float tickR2 = arcRad + 6.5f;
            const float tx1 = centre.x + tickR1 * std::sin (midAngle);
            const float ty1 = centre.y - tickR1 * std::cos (midAngle);
            const float tx2 = centre.x + tickR2 * std::sin (midAngle);
            const float ty2 = centre.y - tickR2 * std::cos (midAngle);
            g.setColour (Palette::textMuted.withAlpha (0.75f));
            g.drawLine (tx1, ty1, tx2, ty2, 1.5f);
        }

        // Knob cap body
        const float capR = radius - 10.0f;
        g.setColour (Palette::bgPanel.brighter (0.06f));
        g.fillEllipse (centre.x - capR, centre.y - capR, capR * 2.0f, capR * 2.0f);
        g.setColour (Palette::border);
        g.drawEllipse (centre.x - capR, centre.y - capR, capR * 2.0f, capR * 2.0f, 1.2f);

        // Pointer indicator line
        juce::Path pointer;
        const float ptrLen = capR * 0.62f;
        pointer.addRoundedRectangle (-1.4f, -capR + 3.0f, 2.8f, ptrLen, 1.2f);
        g.setColour (enabled ? Palette::textPrimary : Palette::textMuted.withAlpha (0.4f));
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    }

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.75f);
        auto baseCol = backgroundColour;

        if (shouldDrawButtonAsDown)
            baseCol = baseCol.darker (0.12f);
        else if (shouldDrawButtonAsHighlighted)
            baseCol = baseCol.brighter (0.08f);

        g.setColour (baseCol);
        g.fillRoundedRectangle (bounds, 6.0f);

        g.setColour (button.getToggleState() ? baseCol.brighter (0.25f) : Palette::border);
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return makeFont (juce::jlimit (11.0f, 14.0f, static_cast<float> (buttonHeight) * 0.45f), true);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return makeFont (12.0f, false);
    }
};

// ==============================================================================
// Lightweight Stereo Peak Meter Component
// ==============================================================================
class StereoMeterComponent final : public juce::Component
{
public:
    void setLevels (float leftLinear, float rightLinear)
    {
        levelL = (leftLinear  > levelL) ? leftLinear  : (levelL * 0.86f);
        levelR = (rightLinear > levelR) ? rightLinear : (levelR * 0.86f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (Palette::bgMain);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (area, 4.0f, 1.0f);

        auto inner = area.reduced (3.0f);
        const float gap = 2.0f;
        const float barW = (inner.getWidth() - gap) * 0.5f;

        drawBar (g, inner.withWidth (barW), levelL);
        drawBar (g, inner.withLeft (inner.getX() + barW + gap), levelR);
    }

private:
    static void drawBar (juce::Graphics& g, juce::Rectangle<float> bar, float linearVal)
    {
        const float db = juce::Decibels::gainToDecibels (linearVal, -60.0f);
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

        g.setColour (Palette::bgControl);
        g.fillRoundedRectangle (bar, 2.0f);

        if (norm > 0.001f)
        {
            const float fillH = bar.getHeight() * norm;
            auto fillRect = bar.withTop (bar.getBottom() - fillH);

            juce::Colour col = Palette::accentGreen;
            if (linearVal >= 0.99f)
                col = Palette::accentRed;
            else if (db > -6.0f)
                col = Palette::accentOrange;

            g.setColour (col);
            g.fillRoundedRectangle (fillRect, 2.0f);
        }
    }

    float levelL = 0.0f;
    float levelR = 0.0f;
};

// ==============================================================================
// Audio Processor
// ==============================================================================
class ModelCycles6CHProcessor final : public juce::AudioProcessor,
                                      public juce::AudioProcessorValueTreeState::Listener,
                                      private juce::Timer
{
public:
    ModelCycles6CHProcessor()
        : AudioProcessor (BusesProperties()
                            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
    {
        apvts.addParameterListener (ParamIDs::trackChannel,  this);
        apvts.addParameterListener (ParamIDs::volDist,       this);
        apvts.addParameterListener (ParamIDs::trackLevel,    this);
        apvts.addParameterListener (ParamIDs::useTrackLevel, this);
        apvts.addParameterListener (ParamIDs::mute,          this);
        apvts.addParameterListener (ParamIDs::pan,           this);
        apvts.addParameterListener (ParamIDs::inputMode,     this);

        syncAllDspFromAPVTS();
        startTimerHz (60);
    }

    ~ModelCycles6CHProcessor() override
    {
        stopTimer();
        apvts.removeParameterListener (ParamIDs::trackChannel,  this);
        apvts.removeParameterListener (ParamIDs::volDist,       this);
        apvts.removeParameterListener (ParamIDs::trackLevel,    this);
        apvts.removeParameterListener (ParamIDs::useTrackLevel, this);
        apvts.removeParameterListener (ParamIDs::mute,          this);
        apvts.removeParameterListener (ParamIDs::pan,           this);
        apvts.removeParameterListener (ParamIDs::inputMode,     this);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamIDs::trackChannel, 1 },
            "Track Channel", 1, 6, 1));

        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamIDs::volDist, 1 },
            "CC7 Volume+Dist", 0, 127, 64));

        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamIDs::trackLevel, 1 },
            "CC95 Track Level", 0, 127, 127));

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::useTrackLevel, 1 },
            "Enable CC95 Level", false));

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::mute, 1 },
            "CC94 Track Mute", false));

        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamIDs::pan, 1 },
            "CC10 Pan", 0, 127, 64));

        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::inputMode, 1 },
            "Input Mode", juce::StringArray { "Mono (L+R)", "Mono (L Only)" }, 0));

        return { params.begin(), params.end() };
    }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override    { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto mainIn  = layouts.getMainInputChannelSet();
        const auto mainOut = layouts.getMainOutputChannelSet();

        const bool validMonoToMono     = (mainIn == juce::AudioChannelSet::mono()   && mainOut == juce::AudioChannelSet::mono());
        const bool validMonoToStereo   = (mainIn == juce::AudioChannelSet::mono()   && mainOut == juce::AudioChannelSet::stereo());
        const bool validStereoToStereo = (mainIn == juce::AudioChannelSet::stereo() && mainOut == juce::AudioChannelSet::stereo());

        return validMonoToMono || validMonoToStereo || validStereoToStereo;
    }

    void prepareToPlay (double sampleRate, int /*samplesPerBlock*/) override
    {
        const double smoothSeconds = 0.015; // 15 ms click-free smoothing
        smoothedGain.reset (sampleRate, smoothSeconds);
        smoothedPanL.reset (sampleRate, smoothSeconds);
        smoothedPanR.reset (sampleRate, smoothSeconds);

        updateDspTargets (true);
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int totalNumInputChannels  = getTotalNumInputChannels();
        const int totalNumOutputChannels = getTotalNumOutputChannels();
        const int numSamples             = buffer.getNumSamples();

        for (int ch = totalNumInputChannels; ch < totalNumOutputChannels; ++ch)
            buffer.clear (ch, 0, numSamples);

        const int targetChannel = dspTrackChannel.load (std::memory_order_relaxed); // 1..6

        if (useDirectMidi.load (std::memory_order_acquire))
        {
            pollDirectMidiHub (targetChannel - 1);
        }
        else
        {
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                if (! msg.isController())
                    continue;

                if (msg.getChannel() == targetChannel)
                    applyIncomingCC (msg.getControllerNumber(), msg.getControllerValue());
            }
        }

        updateDspTargets (false);

        const bool leftOnly = (dspInputMode.load (std::memory_order_relaxed) == 1);
        float blockPeakL = 0.0f;
        float blockPeakR = 0.0f;

        if (totalNumOutputChannels == 1)
        {
            auto* outL = buffer.getWritePointer (0);
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = smoothedGain.getNextValue();
                outL[i] *= g;
                blockPeakL = juce::jmax (blockPeakL, std::abs (outL[i]));
            }
            blockPeakR = blockPeakL;
        }
        else if (totalNumOutputChannels >= 2)
        {
            auto* outL = buffer.getWritePointer (0);
            auto* outR = buffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                const float g    = smoothedGain.getNextValue();
                const float panL = smoothedPanL.getNextValue();
                const float panR = smoothedPanR.getNextValue();

                float monoIn = outL[i];
                if (totalNumInputChannels >= 2 && ! leftOnly)
                    monoIn = 0.5f * (outL[i] + outR[i]);

                const float scaled = monoIn * g;
                const float sL = scaled * panL;
                const float sR = scaled * panR;

                outL[i] = sL;
                outR[i] = sR;

                blockPeakL = juce::jmax (blockPeakL, std::abs (sL));
                blockPeakR = juce::jmax (blockPeakR, std::abs (sR));
            }
        }

        meterPeakL.store (blockPeakL, std::memory_order_relaxed);
        meterPeakR.store (blockPeakR, std::memory_order_relaxed);
    }

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        if (xml != nullptr)
        {
            xml->setAttribute ("midiSourceName", selectedMidiSourceName);
            copyXmlToBinary (*xml, destData);
        }
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
        if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        {
            const auto restoredSource = xml->getStringAttribute ("midiSourceName", "DAW MIDI");
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            syncAllDspFromAPVTS();
            setMidiSource (restoredSource);
        }
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void parameterChanged (const juce::String& parameterID, float newValue) override
    {
        if (updatingFromMidi.load (std::memory_order_acquire))
            return;

        if (parameterID == ParamIDs::trackChannel)
        {
            const int ch = juce::jlimit (1, 6, juce::roundToInt (newValue));
            dspTrackChannel.store (ch, std::memory_order_relaxed);
            syncDirectMidiSequences (ch - 1);
        }
        else if (parameterID == ParamIDs::volDist)
        {
            dspVolDist.store (juce::jlimit (0, 127, juce::roundToInt (newValue)), std::memory_order_relaxed);
        }
        else if (parameterID == ParamIDs::trackLevel)
        {
            dspTrackLevel.store (juce::jlimit (0, 127, juce::roundToInt (newValue)), std::memory_order_relaxed);
        }
        else if (parameterID == ParamIDs::useTrackLevel)
        {
            dspUseTrackLevel.store (newValue >= 0.5f, std::memory_order_relaxed);
        }
        else if (parameterID == ParamIDs::mute)
        {
            dspMute.store (newValue >= 0.5f, std::memory_order_relaxed);
        }
        else if (parameterID == ParamIDs::pan)
        {
            dspPan.store (juce::jlimit (0, 127, juce::roundToInt (newValue)), std::memory_order_relaxed);
        }
        else if (parameterID == ParamIDs::inputMode)
        {
            dspInputMode.store (juce::roundToInt (newValue), std::memory_order_relaxed);
        }
    }

    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }

    bool setMidiSource (const juce::String& sourceName)
    {
        selectedMidiSourceName = sourceName.isEmpty() ? "DAW MIDI" : sourceName;

        if (selectedMidiSourceName == "DAW MIDI")
        {
            useDirectMidi.store (false, std::memory_order_release);
            directMidiOnline.store (false, std::memory_order_relaxed);
            return true;
        }

        const bool ok = directMidiHub->openDeviceByName (selectedMidiSourceName);
        syncDirectMidiSequences (dspTrackChannel.load (std::memory_order_relaxed) - 1);
        useDirectMidi.store (ok, std::memory_order_release);
        directMidiOnline.store (ok, std::memory_order_relaxed);
        return ok;
    }

    juce::String getMidiSourceName() const { return selectedMidiSourceName; }
    bool isUsingDirectMidi() const noexcept { return useDirectMidi.load (std::memory_order_relaxed); }
    bool isDirectMidiOnline() const noexcept { return directMidiOnline.load (std::memory_order_relaxed); }

    float getMeterPeakL() const noexcept { return meterPeakL.load (std::memory_order_relaxed); }
    float getMeterPeakR() const noexcept { return meterPeakR.load (std::memory_order_relaxed); }

    uint32_t getMidiActivityCounter() const noexcept
    {
        return midiActivityCounter.load (std::memory_order_relaxed);
    }

    float computeCurrentLinearGain() const noexcept
    {
        if (dspMute.load (std::memory_order_relaxed))
            return 0.0f;

        const int v = dspVolDist.load (std::memory_order_relaxed);
        float g = (v >= 64) ? 1.0f : std::pow (static_cast<float> (v) / 64.0f, 1.6f);

        if (dspUseTrackLevel.load (std::memory_order_relaxed))
        {
            const int lv = dspTrackLevel.load (std::memory_order_relaxed);
            g *= std::pow (static_cast<float> (lv) / 127.0f, 1.6f);
        }
        return g;
    }

private:
    void applyIncomingCC (int ccNumber, int ccValue) noexcept
    {
        const int val = juce::jlimit (0, 127, ccValue);

        if (ccNumber == 7)
        {
            dspVolDist.store (val, std::memory_order_relaxed);
            pendingHostVolDist.store (val, std::memory_order_release);
            midiActivityCounter.fetch_add (1, std::memory_order_relaxed);
        }
        else if (ccNumber == 95)
        {
            dspTrackLevel.store (val, std::memory_order_relaxed);
            pendingHostTrackLevel.store (val, std::memory_order_release);
            midiActivityCounter.fetch_add (1, std::memory_order_relaxed);
        }
        else if (ccNumber == 94)
        {
            const bool isMuted = (val > 0);
            dspMute.store (isMuted, std::memory_order_relaxed);
            pendingHostMute.store (isMuted ? 1 : 0, std::memory_order_release);
            midiActivityCounter.fetch_add (1, std::memory_order_relaxed);
        }
        else if (ccNumber == 10)
        {
            dspPan.store (val, std::memory_order_relaxed);
            pendingHostPan.store (val, std::memory_order_release);
            midiActivityCounter.fetch_add (1, std::memory_order_relaxed);
        }
    }

    void pollDirectMidiHub (int chIndex) noexcept
    {
        const auto& st = directMidiHub->getChannelState (chIndex);

        const uint32_t s7 = st.cc7Seq.load (std::memory_order_acquire);
        if (s7 != lastDirectSeq7)
        {
            lastDirectSeq7 = s7;
            applyIncomingCC (7, st.cc7Val.load (std::memory_order_relaxed));
        }

        const uint32_t s95 = st.cc95Seq.load (std::memory_order_acquire);
        if (s95 != lastDirectSeq95)
        {
            lastDirectSeq95 = s95;
            applyIncomingCC (95, st.cc95Val.load (std::memory_order_relaxed));
        }

        const uint32_t s94 = st.cc94Seq.load (std::memory_order_acquire);
        if (s94 != lastDirectSeq94)
        {
            lastDirectSeq94 = s94;
            applyIncomingCC (94, st.cc94Val.load (std::memory_order_relaxed));
        }

        const uint32_t s10 = st.cc10Seq.load (std::memory_order_acquire);
        if (s10 != lastDirectSeq10)
        {
            lastDirectSeq10 = s10;
            applyIncomingCC (10, st.cc10Val.load (std::memory_order_relaxed));
        }
    }

    void syncDirectMidiSequences (int chIndex) noexcept
    {
        const auto& st  = directMidiHub->getChannelState (chIndex);
        lastDirectSeq7  = st.cc7Seq.load (std::memory_order_acquire);
        lastDirectSeq95 = st.cc95Seq.load (std::memory_order_acquire);
        lastDirectSeq94 = st.cc94Seq.load (std::memory_order_acquire);
        lastDirectSeq10 = st.cc10Seq.load (std::memory_order_acquire);
    }

    void updateDspTargets (bool immediate) noexcept
    {
        const float targetGain = computeCurrentLinearGain();

        const int panCC = dspPan.load (std::memory_order_relaxed);
        const float p = (panCC <= 64)
                            ? (static_cast<float> (panCC) / 128.0f)
                            : (0.5f + static_cast<float> (panCC - 64) / 126.0f);

        const float targetPanL = (p <= 0.5f)
                                     ? 1.0f
                                     : std::cos ((p - 0.5f) * juce::MathConstants<float>::pi);
        const float targetPanR = (p >= 0.5f)
                                     ? 1.0f
                                     : std::sin (p * juce::MathConstants<float>::pi);

        if (immediate)
        {
            smoothedGain.setCurrentAndTargetValue (targetGain);
            smoothedPanL.setCurrentAndTargetValue (targetPanL);
            smoothedPanR.setCurrentAndTargetValue (targetPanR);
        }
        else
        {
            smoothedGain.setTargetValue (targetGain);
            smoothedPanL.setTargetValue (targetPanL);
            smoothedPanR.setTargetValue (targetPanR);
        }
    }

    void syncAllDspFromAPVTS()
    {
        dspTrackChannel.store  (juce::roundToInt (apvts.getRawParameterValue (ParamIDs::trackChannel)->load()),  std::memory_order_relaxed);
        dspVolDist.store       (juce::roundToInt (apvts.getRawParameterValue (ParamIDs::volDist)->load()),       std::memory_order_relaxed);
        dspTrackLevel.store    (juce::roundToInt (apvts.getRawParameterValue (ParamIDs::trackLevel)->load()),    std::memory_order_relaxed);
        dspUseTrackLevel.store (apvts.getRawParameterValue (ParamIDs::useTrackLevel)->load() >= 0.5f,            std::memory_order_relaxed);
        dspMute.store          (apvts.getRawParameterValue (ParamIDs::mute)->load() >= 0.5f,                     std::memory_order_relaxed);
        dspPan.store           (juce::roundToInt (apvts.getRawParameterValue (ParamIDs::pan)->load()),           std::memory_order_relaxed);
        dspInputMode.store     (juce::roundToInt (apvts.getRawParameterValue (ParamIDs::inputMode)->load()),     std::memory_order_relaxed);
    }

    void timerCallback() override
    {
        auto pushIntParam = [this] (const char* paramID, std::atomic<int>& pending)
        {
            const int val = pending.exchange (-1, std::memory_order_acq_rel);
            if (val >= 0)
            {
                if (auto* param = apvts.getParameter (paramID))
                {
                    updatingFromMidi.store (true, std::memory_order_release);
                    param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (val)));
                    updatingFromMidi.store (false, std::memory_order_release);
                }
            }
        };

        pushIntParam (ParamIDs::volDist,    pendingHostVolDist);
        pushIntParam (ParamIDs::trackLevel, pendingHostTrackLevel);
        pushIntParam (ParamIDs::mute,       pendingHostMute);
        pushIntParam (ParamIDs::pan,        pendingHostPan);
    }

    juce::AudioProcessorValueTreeState apvts;
    juce::SharedResourcePointer<DirectMidiHub> directMidiHub;

    juce::String selectedMidiSourceName { "DAW MIDI" };
    std::atomic<bool> useDirectMidi     { false };
    std::atomic<bool> directMidiOnline  { false };

    std::atomic<int>  dspTrackChannel  { 1 };
    std::atomic<int>  dspVolDist       { 64 };
    std::atomic<int>  dspTrackLevel    { 127 };
    std::atomic<bool> dspUseTrackLevel { false };
    std::atomic<bool> dspMute          { false };
    std::atomic<int>  dspPan           { 64 };
    std::atomic<int>  dspInputMode     { 0 };

    std::atomic<int>  pendingHostVolDist    { -1 };
    std::atomic<int>  pendingHostTrackLevel { -1 };
    std::atomic<int>  pendingHostMute       { -1 };
    std::atomic<int>  pendingHostPan        { -1 };
    std::atomic<bool> updatingFromMidi      { false };

    uint32_t lastDirectSeq7  = 0;
    uint32_t lastDirectSeq95 = 0;
    uint32_t lastDirectSeq94 = 0;
    uint32_t lastDirectSeq10 = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedGain { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedPanL { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedPanR { 1.0f };

    std::atomic<float>    meterPeakL          { 0.0f };
    std::atomic<float>    meterPeakR          { 0.0f };
    std::atomic<uint32_t> midiActivityCounter { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModelCycles6CHProcessor)
};

// ==============================================================================
// Minimal Graphical User Interface
// ==============================================================================
class ModelCycles6CHEditor final : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit ModelCycles6CHEditor (ModelCycles6CHProcessor& p)
        : AudioProcessorEditor (&p),
          processor (p)
    {
        setLookAndFeel (&lookAndFeel);
        setSize (476, 292);

        midiSourceBox.setTooltip ("Select DAW MIDI (standard routing) or a direct hardware MIDI input");
        midiSourceBox.onChange = [this]
        {
            if (isPopulatingMidiBox)
                return;
            const auto chosen = midiSourceBox.getText();
            if (chosen.startsWith ("Direct: "))
                processor.setMidiSource (chosen.fromFirstOccurrenceOf ("Direct: ", false, false));
            else
                processor.setMidiSource ("DAW MIDI");
            updateStatusText();
        };
        addAndMakeVisible (midiSourceBox);

        refreshMidiBtn.setButtonText ("R");
        refreshMidiBtn.setTooltip ("Refresh hardware MIDI device list");
        refreshMidiBtn.setColour (juce::TextButton::buttonColourId, Palette::bgControl);
        refreshMidiBtn.setColour (juce::TextButton::textColourOffId, Palette::textMuted);
        refreshMidiBtn.onClick = [this] { populateMidiSources(); };
        addAndMakeVisible (refreshMidiBtn);

        populateMidiSources();

        for (int i = 0; i < 6; ++i)
        {
            auto& btn = trackButtons[static_cast<size_t> (i)];
            btn.setButtonText ("T" + juce::String (i + 1));
            btn.setClickingTogglesState (false);
            btn.setColour (juce::TextButton::buttonColourId,   Palette::bgControl);
            btn.setColour (juce::TextButton::buttonOnColourId, Palette::accentOrange);
            btn.setColour (juce::TextButton::textColourOffId,  Palette::textMuted);
            btn.setColour (juce::TextButton::textColourOnId,   Palette::bgMain);
            btn.onClick = [this, ch = i + 1]
            {
                if (auto* param = processor.getAPVTS().getParameter (ParamIDs::trackChannel))
                    param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (ch)));
            };
            addAndMakeVisible (btn);
        }

        configureRotary (volDistSlider, Palette::accentOrange, 64.0, false, true);
        configureRotary (panSlider,     Palette::accentCyan,   64.0, true,  true);
        configureRotary (levelSlider,   Palette::textPrimary, 127.0, false, false);

        configureTitleLabel (volDistTitle, "VOL + DIST");
        configureTitleLabel (panTitle,     "PAN");
        configureTitleLabel (levelTitle,   "LEVEL");

        configureValueLabel (volDistValue);
        configureValueLabel (panValue);
        configureValueLabel (levelValue);

        cc95Toggle.setButtonText ("CC95 OFF");
        cc95Toggle.setClickingTogglesState (true);
        cc95Toggle.setColour (juce::TextButton::buttonColourId,   Palette::bgControl);
        cc95Toggle.setColour (juce::TextButton::buttonOnColourId, Palette::accentOrange.withAlpha (0.85f));
        cc95Toggle.setColour (juce::TextButton::textColourOffId,  Palette::textMuted);
        cc95Toggle.setColour (juce::TextButton::textColourOnId,   Palette::bgMain);
        addAndMakeVisible (cc95Toggle);

        muteButton.setButtonText ("MUTE");
        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonColourId,   Palette::bgControl);
        muteButton.setColour (juce::TextButton::buttonOnColourId, Palette::accentRed);
        muteButton.setColour (juce::TextButton::textColourOffId,  Palette::textPrimary);
        muteButton.setColour (juce::TextButton::textColourOnId,   Palette::textPrimary);
        addAndMakeVisible (muteButton);

        addAndMakeVisible (meter);

        inputModeBtn.setColour (juce::TextButton::buttonColourId,  Palette::bgControl);
        inputModeBtn.setColour (juce::TextButton::textColourOffId, Palette::textMuted);
        inputModeBtn.onClick = [this]
        {
            if (auto* param = processor.getAPVTS().getParameter (ParamIDs::inputMode))
            {
                const float nextVal = (param->getValue() < 0.5f) ? 1.0f : 0.0f;
                param->setValueNotifyingHost (nextVal);
            }
        };
        addAndMakeVisible (inputModeBtn);

        statusLabel.setFont (makeFont (11.5f, false));
        statusLabel.setColour (juce::Label::textColourId, Palette::textMuted);
        statusLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (statusLabel);

        auto& apvts = processor.getAPVTS();
        volDistAttachment  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, ParamIDs::volDist,       volDistSlider);
        panAttachment      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, ParamIDs::pan,           panSlider);
        levelAttachment    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, ParamIDs::trackLevel,    levelSlider);
        cc95Attachment     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ParamIDs::useTrackLevel, cc95Toggle);
        muteAttachment     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ParamIDs::mute,          muteButton);

        lastMidiCounter = processor.getMidiActivityCounter();
        refreshAllVisuals();
        startTimerHz (30);
    }

    ~ModelCycles6CHEditor() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::bgMain);

        auto header = getLocalBounds().removeFromTop (44);
        g.setColour (Palette::bgPanel);
        g.fillRect (header);
        g.setColour (Palette::border);
        g.drawHorizontalLine (header.getBottom() - 1, 0.0f, static_cast<float> (getWidth()));

        g.setColour (Palette::accentOrange);
        g.setFont (makeFont (16.0f, true));
        g.drawText ("M:C 6CH", 14, 7, 90, 20, juce::Justification::centredLeft);

        g.setColour (Palette::textMuted);
        g.setFont (makeFont (10.5f, false));
        g.drawText ("CC7 / CC94 / CC10", 14, 24, 130, 14, juce::Justification::centredLeft);

        const juce::Rectangle<float> ledRect (156.0f, 17.0f, 10.0f, 10.0f);
        g.setColour (midiLedAlpha > 0.05f
                         ? Palette::accentGreen.interpolatedWith (Palette::bgControl, 1.0f - midiLedAlpha)
                         : Palette::bgControl);
        g.fillEllipse (ledRect);
        g.setColour (Palette::border);
        g.drawEllipse (ledRect, 1.0f);

        g.setColour (Palette::textMuted);
        g.setFont (makeFont (10.0f, true));
        g.drawText ("MIDI", 170, 15, 34, 14, juce::Justification::centredLeft);

        auto mainCard = juce::Rectangle<float> (14.0f, 94.0f, static_cast<float> (getWidth()) - 28.0f, 158.0f);
        g.setColour (Palette::bgPanel);
        g.fillRoundedRectangle (mainCard, 8.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (mainCard, 8.0f, 1.0f);

        g.setColour (Palette::textMuted);
        g.setFont (makeFont (10.5f, true));
        g.drawText ("MIDI CH", 14, 58, 52, 24, juce::Justification::centredLeft);
    }

    void resized() override
    {
        midiSourceBox.setBounds (210, 10, 222, 24);
        refreshMidiBtn.setBounds (436, 10, 26, 24);

        const int padStartX = 70;
        const int padY      = 56;
        const int padGap    = 6;
        const int padW      = (getWidth() - padStartX - 14 - padGap * 5) / 6;
        for (int i = 0; i < 6; ++i)
            trackButtons[static_cast<size_t> (i)].setBounds (padStartX + i * (padW + padGap), padY, padW, 28);

        const int knobY   = 124;
        const int knobSz  = 86;

        volDistTitle.setBounds  (22, 104, 102, 16);
        volDistSlider.setBounds (30, knobY, knobSz, knobSz);
        volDistValue.setBounds  (22, 214, 102, 18);

        panTitle.setBounds  (132, 104, 96, 16);
        panSlider.setBounds (137, knobY, knobSz, knobSz);
        panValue.setBounds  (132, 214, 96, 18);

        levelTitle.setBounds  (236, 104, 96, 16);
        levelSlider.setBounds (241, knobY, knobSz, knobSz);
        levelValue.setBounds  (236, 208, 96, 15);
        cc95Toggle.setBounds  (248, 226, 72, 19);

        muteButton.setBounds (346, 128, 72, 86);
        meter.setBounds      (428, 108, 22, 130);

        inputModeBtn.setBounds (14, 260, 108, 22);
        statusLabel.setBounds  (132, 260, getWidth() - 146, 22);
    }

private:
    void configureRotary (juce::Slider& s, juce::Colour accent, double defaultVal, bool bipolar, bool midTick)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setColour (juce::Slider::rotarySliderFillColourId, accent);
        s.setDoubleClickReturnValue (true, defaultVal);
        s.getProperties().set ("bipolar", bipolar);
        s.getProperties().set ("midTick", midTick);
        addAndMakeVisible (s);
    }

    void configureTitleLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (makeFont (11.0f, true));
        l.setColour (juce::Label::textColourId, Palette::textMuted);
        l.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    }

    void configureValueLabel (juce::Label& l)
    {
        l.setFont (makeFont (11.5f, true));
        l.setColour (juce::Label::textColourId, Palette::textPrimary);
        l.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    }

    void populateMidiSources()
    {
        isPopulatingMidiBox = true;
        midiSourceBox.clear (juce::dontSendNotification);

        midiSourceBox.addItem ("DAW MIDI", 1);

        const auto devices = juce::MidiInput::getAvailableDevices();
        int itemId = 2;
        int selectedId = 1;
        const auto currentSrc = processor.getMidiSourceName();

        for (const auto& dev : devices)
        {
            const juce::String label = "Direct: " + dev.name;
            midiSourceBox.addItem (label, itemId);
            if (currentSrc == dev.name)
                selectedId = itemId;
            ++itemId;
        }

        if (selectedId == 1 && currentSrc != "DAW MIDI" && currentSrc.isNotEmpty())
        {
            midiSourceBox.addItem ("Direct: " + currentSrc + " (offline)", itemId);
            selectedId = itemId;
        }

        midiSourceBox.setSelectedId (selectedId, juce::dontSendNotification);
        isPopulatingMidiBox = false;
    }

    void refreshAllVisuals()
    {
        auto& apvts = processor.getAPVTS();

        const int activeCh = juce::roundToInt (apvts.getRawParameterValue (ParamIDs::trackChannel)->load());
        for (int i = 0; i < 6; ++i)
            trackButtons[static_cast<size_t> (i)].setToggleState ((i + 1) == activeCh, juce::dontSendNotification);

        const int vd = juce::roundToInt (volDistSlider.getValue());
        if (vd == 0)
            volDistValue.setText ("0 (-inf dB)", juce::dontSendNotification);
        else if (vd < 64)
        {
            const float g  = std::pow (static_cast<float> (vd) / 64.0f, 1.6f);
            const float db = juce::Decibels::gainToDecibels (g, -60.0f);
            volDistValue.setText (juce::String (vd) + " (" + juce::String (db, 1) + " dB)", juce::dontSendNotification);
        }
        else if (vd == 64)
            volDistValue.setText ("64 (UNITY)", juce::dontSendNotification);
        else
            volDistValue.setText (juce::String (vd) + " (DRIVE)", juce::dontSendNotification);

        const int pn = juce::roundToInt (panSlider.getValue());
        if (pn == 64)
            panValue.setText ("CENTER", juce::dontSendNotification);
        else if (pn < 64)
            panValue.setText ("L " + juce::String (64 - pn), juce::dontSendNotification);
        else
            panValue.setText ("R " + juce::String (pn - 64), juce::dontSendNotification);

        const bool cc95On = cc95Toggle.getToggleState();
        levelSlider.setEnabled (cc95On);
        cc95Toggle.setButtonText (cc95On ? "CC95 ON" : "CC95 OFF");
        const int lv = juce::roundToInt (levelSlider.getValue());
        levelValue.setText (cc95On ? juce::String (lv) : "BYPASS", juce::dontSendNotification);
        levelValue.setColour (juce::Label::textColourId, cc95On ? Palette::textPrimary : Palette::textMuted);

        const int inMode = juce::roundToInt (apvts.getRawParameterValue (ParamIDs::inputMode)->load());
        inputModeBtn.setButtonText (inMode == 0 ? "IN: MONO L+R" : "IN: MONO L");

        updateStatusText();
    }

    void updateStatusText()
    {
        const float linGain = processor.computeCurrentLinearGain();
        juce::String gainStr;
        if (muteButton.getToggleState())
            gainStr = "MUTED (-inf dB)";
        else if (linGain <= 0.0001f)
            gainStr = "Gain: -inf dB";
        else
            gainStr = "Gain: " + juce::String (juce::Decibels::gainToDecibels (linGain, -60.0f), 1) + " dB";

        juce::String modeStr = "DAW MIDI";
        if (processor.getMidiSourceName() != "DAW MIDI")
            modeStr = processor.isDirectMidiOnline() ? "Direct MIDI OK" : "Direct MIDI Offline";

        statusLabel.setText (modeStr + "  |  " + gainStr, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const uint32_t currentCnt = processor.getMidiActivityCounter();
        if (currentCnt != lastMidiCounter)
        {
            lastMidiCounter = currentCnt;
            midiLedAlpha = 1.0f;
            repaint (150, 12, 22, 20);
        }
        else if (midiLedAlpha > 0.0f)
        {
            midiLedAlpha = juce::jmax (0.0f, midiLedAlpha - 0.14f);
            repaint (150, 12, 22, 20);
        }

        meter.setLevels (processor.getMeterPeakL(), processor.getMeterPeakR());
        refreshAllVisuals();
    }

    ModelCycles6CHProcessor& processor;
    MinimalLookAndFeel lookAndFeel;

    juce::ComboBox midiSourceBox;
    juce::TextButton refreshMidiBtn;
    bool isPopulatingMidiBox = false;

    std::array<juce::TextButton, 6> trackButtons;

    juce::Slider volDistSlider, panSlider, levelSlider;
    juce::Label  volDistTitle,  panTitle,  levelTitle;
    juce::Label  volDistValue,  panValue,  levelValue;

    juce::TextButton cc95Toggle;
    juce::TextButton muteButton;
    StereoMeterComponent meter;

    juce::TextButton inputModeBtn;
    juce::Label      statusLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volDistAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cc95Attachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment;

    uint32_t lastMidiCounter = 0;
    float    midiLedAlpha    = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModelCycles6CHEditor)
};

// ==============================================================================
// Editor Factory Definition
// ==============================================================================
juce::AudioProcessorEditor* ModelCycles6CHProcessor::createEditor()
{
    return new ModelCycles6CHEditor (*this);
}

} // namespace mc6ch

// ==============================================================================
// JUCE Plugin Entry Point
// ==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new mc6ch::ModelCycles6CHProcessor();
}

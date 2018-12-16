/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com
*/


namespace tracktion_engine
{

//==============================================================================
AirWindowsCallback::AirWindowsCallback (AirWindowsPlugin& o)
    : owner (o)
{
}

double AirWindowsCallback::getSampleRate()
{
    return owner.sampleRate;
}

//==============================================================================
class AirWindowsAutomatableParameter   : public AutomatableParameter
{
public:
    AirWindowsAutomatableParameter (AirWindowsPlugin& p, int idx)
        : AutomatableParameter (getParamId (p, idx), getParamName (p, idx), p, {0, 1}),
        plugin (p), index (idx)
    {
        id = getParamId (p, idx);
        
        valueToStringFunction = [] (float v)  { return String (v); };
        stringToValueFunction = [] (String v) { return v.getFloatValue(); };
    }
    
    ~AirWindowsAutomatableParameter()
    {
        notifyListenersOfDeletion();
    }
    
    String getCurrentValueAsString() override
    {
        char paramText[kVstMaxParamStrLen];
        plugin.impl->getParameterDisplay (index, paramText);
        return String (paramText).substring (0, 4);
    }
    
    static String getParamId (AirWindowsPlugin& p, int idx)
    {
        return getParamName (p, idx).toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz");
    }

    static String getParamName (AirWindowsPlugin& p, int idx)
    {
        char paramName[kVstMaxParamStrLen];
        p.impl->getParameterName (idx, paramName);
        return String (paramName);
    }
    
    AirWindowsPlugin& plugin;
    String id;
    int index = 0;
};
    
//==============================================================================
/** specialised AutomatableParameter for wet/dry.
 Having a subclass just lets it label itself more nicely.
 */
struct AirWindowsWetDryAutomatableParam  : public AutomatableParameter
{
    AirWindowsWetDryAutomatableParam (const String& xmlTag, const String& name, AirWindowsPlugin& owner)
        : AutomatableParameter (xmlTag, name, owner, { 0.0f, 1.0f })
    {
    }
    
    ~AirWindowsWetDryAutomatableParam()
    {
        notifyListenersOfDeletion();
    }
    
    String valueToString (float value) override     { return Decibels::toString (Decibels::gainToDecibels (value), 1); }
    float stringToValue (const String& s) override  { return dbStringToDb (s); }
    
    AirWindowsWetDryAutomatableParam() = delete;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AirWindowsWetDryAutomatableParam)
};
    
//==============================================================================
AirWindowsPlugin::AirWindowsPlugin (PluginCreationInfo info, std::unique_ptr<AirWindowsBase> base)
    : Plugin (info), callback (*this), impl (std::move (base))
{
    auto um = getUndoManager();
    
    for (int i = 0; i < impl->getNumParameters(); i++)
    {        
        auto param = new AirWindowsAutomatableParameter (*this, i);
        
        addAutomatableParameter (param);
        parameters.add (param);
        
        auto* value = new CachedValue<float>();
        value->referTo (state, Identifier (param->id), um, impl->getParameter (i));
        values.add (value);
        
        param->attachToCurrentValue (*value);
    }
    
    addAutomatableParameter (dryGain = new AirWindowsWetDryAutomatableParam ("dry level", TRANS("Dry Level"), *this));
    addAutomatableParameter (wetGain = new AirWindowsWetDryAutomatableParam ("wet level", TRANS("Wet Level"), *this));
    
    dryValue.referTo (state, IDs::dry, um);
    wetValue.referTo (state, IDs::wet, um, 1.0f);
    
    dryGain->attachToCurrentValue (dryValue);
    wetGain->attachToCurrentValue (wetValue);
}
    
AirWindowsPlugin::~AirWindowsPlugin()
{
    for (auto p : parameters)
        p->detachFromCurrentValue();
    
    dryGain->detachFromCurrentValue();
    wetGain->detachFromCurrentValue();
}
    
int AirWindowsPlugin::getNumOutputChannelsGivenInputs (int)
{
    return impl->getNumOutputs();
}

void AirWindowsPlugin::initialise (const PlaybackInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
}

void AirWindowsPlugin::deinitialise()
{
}

void AirWindowsPlugin::applyToBuffer (const AudioRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK
    
    for (int i = 0; i < impl->getNumParameters(); i++)
        impl->setParameter (i, parameters[i]->getCurrentValue());

    juce::AudioBuffer<float> asb (fc.destBuffer->getArrayOfWritePointers(), fc.destBuffer->getNumChannels(),
                                  fc.bufferStartSample, fc.bufferNumSamples);
    
    auto dry = dryGain->getCurrentValue();
    auto wet = wetGain->getCurrentValue();
    
    if (dry <= 0.00004f)
    {
        processBlock (asb);
        zeroDenormalisedValuesIfNeeded (asb);
        
        if (wet < 0.999f)
            asb.applyGain (0, fc.bufferNumSamples, wet);
    }
    else
    {
        auto numChans = asb.getNumChannels();
        AudioScratchBuffer dryAudio (numChans, fc.bufferNumSamples);
        
        for (int i = 0; i < numChans; ++i)
            dryAudio.buffer.copyFrom (i, 0, asb, i, 0, fc.bufferNumSamples);
        
        processBlock (asb);
        zeroDenormalisedValuesIfNeeded (asb);
        
        if (wet < 0.999f)
            asb.applyGain (0, fc.bufferNumSamples, wet);
        
        for (int i = 0; i < numChans; ++i)
            asb.addFrom (i, 0, dryAudio.buffer.getReadPointer (i), fc.bufferNumSamples, dry);
    }
}

void AirWindowsPlugin::processBlock (juce::AudioBuffer<float>& buffer)
{
    const int numChans = buffer.getNumChannels();
    const int samps    = buffer.getNumSamples();
    
    AudioScratchBuffer output (numChans, samps);
    output.buffer.clear();
    
    impl->processReplacing (buffer.getArrayOfWritePointers(),
                            output.buffer.getArrayOfWritePointers(),
                            samps);
    
    for (int i = 0; i < numChans; ++i)
        buffer.copyFrom (i, 0, output.buffer, i, 0, samps);
}
    
void AirWindowsPlugin::restorePluginStateFromValueTree (const ValueTree& v)
{
    for (auto* cv : values)
    {
        const juce::Identifier& prop = cv->getPropertyID();
        
        if (v.hasProperty (prop))
            *cv = float (v.getProperty (prop));
    }
    
    CachedValue<float>* cvsFloat[]  = { &wetValue, &dryValue, nullptr };
    copyPropertiesToNullTerminatedCachedValues (v, cvsFloat);
}
    
//==============================================================================
const char* AirWindowsDeEss::xmlTypeName = "airwindows_deess";
    
AirWindowsDeEss::AirWindowsDeEss (PluginCreationInfo info)
    : AirWindowsPlugin (info, std::make_unique<airwindows::deess::DeEss> (&callback))
{
}

}

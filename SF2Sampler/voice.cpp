/*
 * ----------------------------------------------------------------------------
 * ESP32-S3 SF2 Synthesizer Firmware
 * 
 * Description:
 *   Real-time SF2 (SoundFont) compatible wavetable synthesizer with USB MIDI, I2S audio,
 *   multi-layer voice allocation, per-channel filters, reverb, chorus and delay.
 *   GM/GS/XG support is partly implemented
 * 
 * Hardware:
 *   - ESP32-S3 with PSRAM
 *   - I2S DAC output (44100Hz stereo, 16-bit PCM)
 *   - USB MIDI input
 *   - Optional SD card and/or LittleFS
 * 
 * Author: Evgeny Aslovskiy AKA Copych
 * License: MIT
 * Repository: https://github.com/copych/ESP32-S3_SF2_Sampler_Synthesizer
 * 
 * File: voice.cpp
 * Purpose: Voice generation routines
 * ----------------------------------------------------------------------------
 */


#include "voice.h"
#include "misc.h"
#include <esp_dsp.h>

#include "SamplePool.h"
extern SamplePool samplePool;
static const char* TAG = "Voice";

inline float velocityToGain(uint32_t velocity) {
    //    return velocity * velocity * DIV_127 * DIV_127; // square velocity 
    return velocity * DIV_127; // linear velocity
    //return powf(velocity, 0.6f); // 0.6 = 60% powerlaw, approximating sqrt()
}


void Voice::prepareStart(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan) {
    zone = z;

    sampleID = zone.sampleID; 
    // expect sampleHandle already set before calling prepareStart
    if (!sampleHandle) {
        active = false;
        return;
    }

    data = (const int16_t*)__builtin_assume_aligned(sampleHandle->data, 4);
  //  ESP_LOGI("VOICE", "PREP ch=%u note=%u sid=%u handle=%p data=%p len=%u rate=%u",
  //           ch, note_, zone.sampleID, sampleHandle, sampleHandle->data,
  //           sampleHandle->length, sampleHandle->sampleRate);

    int startNote = chan->portaCurrentNote;

    note = note_;
    velocity = vel;
    channel = ch;
    forward = true;
    phase = 1.0f;
    noteHeld = true;
    samplesRun = lastSamplesRun = 0;
    exclusiveClass = zone.exclusiveClass;

    modWheel = &chan->modWheel;
    modVolume = &chan->volume;
    modExpression = &chan->expression;
    modPitchBendFactor = &chan->pitchBendFactor;
    modPan = &chan->pan;
    modSustain = &chan->sustainPedal;
    modPortaTime = &chan->portaTime;
    modPortamento = &chan->portamento;

    chFilter.setCoeffs(&chan->filterCoeffs);
    chFilter.resetState();

    velocityVolume = velocityToGain(velocity) * zone.attenuation;

    float modEnvStaticTune = (zone.modAttackTime < 1.0f) ?
        (1.0f - zone.modSustainLevel) * zone.modEnvToPitch * 0.01f : 0.0f;

    int rootKey = (zone.rootKey >= 0) ? zone.rootKey : sampleHandle->rootKey;
    float semi = float(note_ - rootKey) + zone.coarseTune + zone.fineTune + chan->tuningSemitones;

    float noteRatio = exp2f((modEnvStaticTune + semi) * DIV_12);
    float baseStep = float(sampleHandle->sampleRate) * DIV_SAMPLE_RATE;
    basePhaseIncrement = baseStep * noteRatio;

    vibLfoPhase = 0.0f;
    vibLfoPhaseIncrement = zone.vibLfoFreq * DIV_SAMPLE_RATE;
    vibLfoToPitch = (zone.vibLfoToPitch == 0.0f) ? 50.0f : zone.vibLfoToPitch;
    vibLfoDelaySamples = zone.vibLfoDelay * SAMPLE_RATE;
    vibLfoCounter = 0;
    vibLfoActive = false;

    portamentoActive = modPortamento && *modPortamento;
    if (portamentoActive) {
        float noteDiff = float(note_ - startNote);
        float freqRatio = exp2f(noteDiff * DIV_12);
        float timeSec = 0.01f + (*modPortaTime) * 0.5f;
        float totalSamples = timeSec * SAMPLE_RATE;
        currentPhaseIncrement = basePhaseIncrement / freqRatio;
        portamentoFactor = 1.0f / freqRatio;
        portamentoLogDelta = exp2f(log2f(freqRatio) / totalSamples);
    } else {
        currentPhaseIncrement = basePhaseIncrement;
        portamentoLogDelta = 1.0f;
        portamentoFactor = 1.0f;
    }

    updatePitch();
    updatePan();

    reverbAmount = zone.reverbSend * chan->reverbSend;
    chorusAmount = zone.chorusSend * chan->chorusSend;

    ampEnv.setAttackTime(zone.attackTime * chan->attackModifier);
    ampEnv.setDecayTime(zone.decayTime);
    ampEnv.setHoldTime(zone.holdTime);
    ampEnv.setSustainLevel(zone.sustainLevel);
    ampEnv.setReleaseTime(zone.releaseTime * chan->releaseModifier);

    int32_t loopStartOffset = zone.loopStartOffset + (zone.loopStartCoarseOffset << 15);
    int32_t loopEndOffset   = zone.loopEndOffset   + (zone.loopEndCoarseOffset << 15);

    length    = sampleHandle->length;
    loopStart = sampleHandle->loopStart + loopStartOffset;
    loopEnd   = sampleHandle->loopEnd   + loopEndOffset;
    loopLength = loopEnd - loopStart;

    loopType = static_cast<LoopType>(zone.sampleModes & 0x0003);
    if (loopType == UNUSED || loopStart >= loopEnd || loopEnd > length) {
        loopType = NO_LOOP;
    }

#ifdef ENABLE_IN_VOICE_FILTERS
    filterCutoff = fclamp(zone.filterFc, 10.0f, 20000.0f);
    filterResonance = (zone.filterQ <= 0.0f) ? 0.707f : 1.0f / powf(10.0f, zone.filterQ / 20.0f);
    filter.resetState();
    filter.setFreqAndQ(filterCutoff, filterResonance);
#endif
}

void Voice::startNew(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan) {
    prepareStart(ch, note_, vel, z, chan);
    if (!sampleHandle || !data) {
        ESP_LOGE("VOICE", "START ABORT ch=%u note=%u sid=%u handle=%p data=%p",
                 ch, note_, z.sampleID, sampleHandle, data);
        active = false;
        return;
    }
    ampEnv.retrigger(Adsr::END_NOW);
    active = true;
 //   ESP_LOGI("VOICE", "START OK ch=%u note=%u sid=%u phase=%.3f inc=%.6f len=%u loop=%u [%u..%u]",
 //            ch, note_, z.sampleID, phase, effectivePhaseIncrement, length,
 //            (unsigned)loopType, (unsigned)loopStart, (unsigned)loopEnd);
}


void Voice::updatePitchOnly(uint8_t newNote, ChannelState* chan) {
    if (!sampleHandle) return;

    int rootKey = (zone.rootKey >= 0) ? zone.rootKey : sampleHandle->rootKey;

    float semi = float(newNote - rootKey) + zone.coarseTune + zone.fineTune;
    float noteRatio = exp2f(semi * DIV_12);

    basePhaseIncrement = float(sampleHandle->sampleRate) * DIV_SAMPLE_RATE * noteRatio;

    portamentoActive = modPortamento && *modPortamento;

    if (portamentoActive) {
        float noteDiff = float(newNote - chan->portaCurrentNote);
        float freqRatio = exp2f(noteDiff * DIV_12);
        float timeSec = 0.01f + (*modPortaTime) * 0.5f;
        float totalSamples = timeSec * SAMPLE_RATE;

        portamentoFactor = 1.0f / freqRatio;
        portamentoLogDelta = exp2f(log2f(freqRatio) / totalSamples);
    } else {
        currentPhaseIncrement = basePhaseIncrement;
        portamentoFactor = 1.0f;
        portamentoLogDelta = 1.0f;
    }

    note = newNote;
    updatePitch();
}



void Voice::stop() {
    if (!(modSustain && *modSustain) ) {
        noteHeld = false;
        ampEnv.end(Adsr::END_REGULAR);
    }
}

void Voice::releaseSample() {
    if (!sampleHandle) return;
    samplePool.release(sampleID);
    sampleHandle = nullptr;
    data = nullptr;
}

void Voice::kill() {
    releaseSample();
    ampEnv.end(Adsr::END_NOW);
    noteHeld = false;
    active = false;
}

void Voice::die() {
    noteHeld = false;
    ampEnv.end(Adsr::END_FAST);
}

bool Voice::isRunning() const {
    return ampEnv.isRunning();
}

float __attribute__((hot,always_inline)) IRAM_ATTR Voice::nextSample() {
    if (!sampleHandle) {
        active = false;
        return 0.0f;
    }

    updatePitch();
    
    // for syncing time-based functions (like LFOs)
    samplesRun++;
    
    // Sample fetch + linear interpolation (unrolled and minimal branching)
    uint32_t idx = (uint32_t)phase;
    float frac = phase - (float)idx;


if (__builtin_expect(
        !data ||
        idx == 0 ||
        idx >= (uint32_t)length,
        0)) {

    ESP_EARLY_LOGE(
        "VOICE",
        "BAD FETCH v=%u active=%d handle=%p data=%p "
        "phase=%f idx=%u len=%f sid=%u loop=%u",
        id,
        active,
        sampleHandle,
        data,
        phase,
        idx,
        length,
        sampleID,
        loopType
    );

    active = false;
    return 0.0f;
}





    // float s0 = data[(idx > 0) ? (idx - 1) : 0];
    // phase[0] = 1.0, so never <= 0, cons: we never get clean 1st sample, pros: it's branchless
    float s0 = data[idx - 1];
    float s1 = data[idx];

    // smp = (s0 + frac * (s1 - s0)) * ONE_DIV_32768;
    float interp = __builtin_fmaf((s1 - s0), frac, s0);  // s0 + (s1 - s0) * frac
    float smp = interp * ONE_DIV_32768;

    // Envelope process
    float env = ampEnv.process();
    float val = smp * velocityVolume * env * (*modVolume) * (*modExpression);

#ifdef ENABLE_IN_VOICE_FILTERS
    val = filter.process(val);
#endif

#ifdef ENABLE_CH_FILTER_M
    val = chFilter.process(val);
#endif

    // Fused phase advance, wrapping, voice lifetime control
    switch (loopType) {
        case FORWARD_LOOP:
            phase += effectivePhaseIncrement;
            if (phase >= loopEnd) phase -= loopLength;
            break;

        case SUSTAIN_LOOP:
            phase += effectivePhaseIncrement;
            //if (ampEnv.getCurrentSegment() != Adsr::ADSR_SEG_RELEASE) {
            if (noteHeld) {
                if (phase >= loopEnd)
                    phase -= loopLength;
            } else {
              loopType = NO_LOOP; // switch to a simplier route
              if (phase >= length) {
                  releaseSample();
                  active = false;
                  return 0.0f;
              }
            }
            break;

        case PING_PONG_LOOP:
            if (forward) {
                phase += effectivePhaseIncrement;
                if (phase >= loopEnd) {
                    phase = 2.0f * loopEnd - phase; // reflect back
                    forward = false;
                }
            } else {
                phase -= effectivePhaseIncrement;
                if (phase <= loopStart) {
                    phase = 2.0f * loopStart - phase; // reflect forward
                    forward = true;
                }
            }
            break; 

        case NO_LOOP:
        default:
            phase += effectivePhaseIncrement;
            if (phase >= length) {
                releaseSample();
                active = false;
                return 0.0f;
            }
            break;
    }

    if (ampEnv.isIdle()) {
        releaseSample();
        active = false;
        return 0.0f;
    }
    
    return val;

}

void Voice::renderBlock(float* block) {
    for (uint32_t i = 0; i < DMA_BUFFER_LEN; i++) {
        block[i] = nextSample() ;
    }
}

void Voice::updateScore() {
    if (!active || !sampleHandle) {
        score = 0.0f;
        return;
    }

    score = ampEnv.getVal() * velocityVolume;

    if (!isRunning() || ampEnv.isIdle()) {
        score *= 0.1f;
    }
}

void __attribute__((always_inline))  Voice::updatePitchFactors() {
    // not clean if cross-threaded, but it's granular anyway ;-)
    float deltaSamplesRun = samplesRun - lastSamplesRun;
    lastSamplesRun = samplesRun;

    // Vibrato LFO
    if (!vibLfoActive) {
        vibLfoCounter += deltaSamplesRun;
        if (vibLfoCounter >= vibLfoDelaySamples)
            vibLfoActive = true;
        pitchMod = 1.0f;
    } else {
        vibLfoPhase += vibLfoPhaseIncrement * deltaSamplesRun;
        if (vibLfoPhase >= 1.0f) vibLfoPhase -= 1.0f;

        float lfo = sin_lut(vibLfoPhase);
        float cents = lfo * (*modWheel) * vibLfoToPitch;
        pitchMod = fastExp2(cents * (1.0f * DIV_1200));
    }

    // Portamento 
    if (portamentoActive) {
        portamentoFactor *= powf(portamentoLogDelta, deltaSamplesRun);
        currentPhaseIncrement = basePhaseIncrement * portamentoFactor;

        // Stop when close enough
        if ( (portamentoLogDelta >= 1.0f && portamentoFactor >= 1.0f) ||
            (portamentoLogDelta <= 1.0f && portamentoFactor <= 1.0f) ) {
            portamentoFactor = 1.0f;
            portamentoActive = false;
            currentPhaseIncrement = targetPhaseIncrement;
        }
        
    }

    // Mod LFO 
    // modFactor = ...
}

void Voice::init() {
    active = false;
    panL = 1.0f;
    panR = 1.0f;
    velocityVolume = 1.0f;

    sampleHandle = nullptr;
    data = nullptr;

    ampEnv.init(SAMPLE_RATE);
    id = usage;
    usage++;
}

void Voice::printState() {
    ESP_LOGI(TAG, "id=%d seg=%s val=%.5f", id, ampEnv.getCurrentSegmentStr(), ampEnv.getVal() );
}

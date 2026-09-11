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
 * File: voice.h
 * Purpose: Voice generator struct for SF2 synthesizer
 * ----------------------------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "config.h"
#include "channel.h"
#include "misc.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "biquad2.h"
#include "SamplePool.h"

enum LoopType {
    NO_LOOP = 0,
    FORWARD_LOOP = 1,
    UNUSED = 2,
    SUSTAIN_LOOP = 3,
    PING_PONG_LOOP = 4 // never used
};

struct DRAM_ATTR Voice {
    float phase = 0.0f; 
    float velocityVolume = 1.0f;
    float panL = 1.0f, panR = 1.0f;
    float score = 0.0f;
    float reverbAmount = 0.0f;
    float chorusAmount = 0.0f;
    float expression = 0.0f;
    float volume = 1.0f;
    uint32_t exclusiveClass = 0;
    size_t samplesRun = 0;
    size_t lastSamplesRun = 0;

#ifdef ENABLE_IN_VOICE_FILTERS
    BiquadFilterInternalCoeffs filter;
    float filterCutoff = 20000.0f;
    float filterResonance = 0.0f;
#endif

#ifdef ENABLE_CH_FILTER_M
    BiquadFilterSharedCoeffs chFilter;

#endif
    float* modWheel = nullptr; // Pointer to channel mod wheel
    float* modVolume = nullptr;
    float* modExpression = nullptr;
    float* modPitchBendFactor = nullptr;
    float* modPan = nullptr;
    float* modPortaTime = nullptr;
    uint32_t* modPortamento = nullptr;
    uint32_t* modSustain = nullptr;
    uint32_t noteHeld = false;
    

    float modFactor = 1.0f;
    float vibFactor = 1.0f;
    float pitchMod = 1.0f;
 

    // LFO state
    float vibLfoPhase = 0.0f;
    float vibLfoPhaseIncrement = 0.0f;
    uint32_t vibLfoCounter = 0;
    uint32_t vibLfoDelaySamples = 0;
    bool vibLfoActive = false;
    float vibLfoToPitch = 50.0f;

    float basePhaseIncrement = 1.0f;     // from note and tuning
    float targetPhaseIncrement = 0.0f;     // updated by pitch bend
    float currentPhaseIncrement = 0.0f;  // final value used in phase stepping
    float effectivePhaseIncrement = 0.0f; // current value used in phase stepping
    float portamentoLogDelta = 0.0f;
    float div_basePhaseIncrement = 0.0f;

    // Portamento
    float portamentoFactor = 1.0f;         // current factor applied
    float targetPortamentoFactor = 1.0f;   // target to glide to
    float portamentoRate = 0.0005f;        // tweak as needed for glide speed (smaller = slower)  float targetPhaseIncrement;
    float portamentoTime = 0.0f; // in seconds
    float portamentoSpeed = 0.0f; // computed based on time
    uint32_t portamentoActive = false;

    uint32_t  length = 0;
    uint32_t  loopStart = 0;
    uint32_t  loopEnd = 0;
    uint32_t  loopLength = 0;
    uint32_t  active = false;

    // Cross-core voice ownership. Core0 holds this for the complete per-voice
    // audio block; Core1 takes it for lifetime/restart mutations. This keeps
    // start/stop/steal operations out of nextSample() without adding a
    // per-sample synchronization branch.
    volatile uint32_t stateLock = 0;

    inline bool __attribute__((always_inline)) tryLockState() {
        uint32_t expected = 0;
        return __atomic_compare_exchange_n(&stateLock, &expected, 1, false,
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    inline void lockState() {
        while (!tryLockState()) taskYIELD();
    }

    inline void __attribute__((always_inline)) unlockState() {
        __atomic_store_n(&stateLock, 0, __ATOMIC_RELEASE);
    }

    uint32_t  forward = true; // for ping-pong
    LoopType  loopType = NO_LOOP; 
    Zone zone = {};
  //  ChannelState* ch = nullptr; 

    SampleHandle* sampleHandle = nullptr;
    uint32_t sampleID = 0;

    const int16_t* data = nullptr;

    Adsr ampEnv;

    uint32_t note = 0;
    uint32_t velocity = 0;
    uint32_t channel = 0;

    void prepareStart(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan);
    void startNew(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan);
    void startLegato(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan, bool retrig);

    void stop();
    void kill();
    void die();
    void releaseSample();
    bool isRunning() const;
    float nextSample();
    void renderBlock(float* block);
    void init();
    static int usage; // = 0
    int id = 0;

    void updateScore();
    void updatePortamento();
    void updatePitchOnly(uint8_t newNote, ChannelState* chan);

    inline void  __attribute__((always_inline)) updatePan() {
        float pZone = zone.pan;
        float pMod  = modPan ? (*modPan * 2.0f - 1.0f) : 0.0f;

        float p = fclamp(pZone + pMod, -1.0f, 1.0f);  // clamp to [-1, 1]
        p = 0.5f * (p + 1.0f);                        // remap to [0, 1]

        // Equal-power pan could be used instead here if needed
        panL = 1.0f - p * 0.5f;
        panR = 0.5f + p * 0.5f;
    }

    void updatePitchFactors();

    inline void __attribute__((always_inline)) updatePitch() {
        effectivePhaseIncrement = basePhaseIncrement * (*modPitchBendFactor) * portamentoFactor * pitchMod;
                        //        * modFactor
    }
    
    void setPortamentoTarget(float targetNoteRatio) ;
    void printState();
    bool isLegato = false;
};



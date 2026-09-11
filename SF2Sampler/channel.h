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
 * File: channel.h
 * Purpose: ChannelState struct for SF2 synthesizer
 * ----------------------------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "config.h"
#include "misc.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "biquad2.h"

float const activitySmoothingFactor = 0.9f;

struct DRAM_ATTR ChannelState {
    bool isDrum;
    float  dryL[DMA_BUFFER_LEN] = {0};
    float  dryR[DMA_BUFFER_LEN] = {0};

    float portaTime = 0.2f;  // CC#5 value, mapped from 0.0 to 1.0
    float volume = 1.0f;        // CC#7, 0.0–1.0
    float expression = 1.0f;    // CC#11, 0.0–1.0
    float pan = 0.5f;           // CC#10, 0.0 = left, 1.0 = right
    
    float modWheel = 0.0f;       // CC#1, 0.0–1.0
    
    int portaCurrentNote = 60;
    
    float activity = 0.0f;

    // Effects
    float reverbSend = 0.0f;    // CC#91, 0.0–1.0
    float chorusSend = 0.0f;    // CC#93, 0.0–1.0
    float delaySend = 0.0f;     // CC#95, 0.0-1.0

    float attackModifier  = 1.0f;	//CC#73	1.0 .. 4.8
    float releaseModifier = 1.0f;	//CC#72 1.0 .. 4.8

    // Pitch bend
    float pitchBend = 0.0f;     // -1.0 to +1.0 (centered)
    float pitchBendRange = 2.0f; // default ±2 semitones
    float pitchBendFactor = 1.0f;  // derived from pitchBend and pitchBendRange
    
    // Tuning
    float tuningSemitones = 0.0f;
    
    // Pedals
    uint32_t sustainPedal = false;  // CC#64
    uint32_t portamento = false ; // CC#65

	// bank / program
    uint32_t  bankMSB = 0;     // CC#0
    uint32_t  bankLSB = 0;     // CC#32
    uint32_t  program = 0;     // Program Change

    uint32_t  wantBankMSB = 0;     // CC#0
    uint32_t  wantBankLSB = 0;     // CC#32
    uint32_t  wantProgram = 0;     // Program Change desired
    
	// NRPN
    struct ParamPair { uint8_t msb = 0x7F, lsb = 0x7F; };
    ParamPair rpn;
    ParamPair nrpn;
	
	// Channel Mono/Poly
    enum MonoMode : uint8_t { Poly = 0, MonoLegato = 1, MonoRetrig = 2 };
    MonoMode monoMode = Poly;

    std::array<uint8_t, 8> noteStack = {};  
    uint8_t stackSize = 0;

    inline void pushNote(uint8_t note) {
        if (stackSize < noteStack.size()) {
            noteStack[stackSize++] = note;
        }
    }

    inline void removeNote(uint8_t note) {
        for (uint8_t i = 0; i < stackSize; ++i) {
            if (noteStack[i] == note) {
                for (uint8_t j = i; j < stackSize - 1; ++j)
                    noteStack[j] = noteStack[j + 1];
                --stackSize;
                break;
            }
        }
    }

    inline uint8_t topNote() const {
        return stackSize > 0 ? noteStack[stackSize - 1] : 0xFF;
    }

    inline bool hasNotes() const {
        return stackSize > 0;
    }

    inline void clearNoteStack() {
        stackSize = 0;
    }

    // Utility
    inline uint16_t getBank() const {
        return (bankMSB << 7) | bankLSB;
    }
    inline uint16_t getWantBank() const {
        return (wantBankMSB << 7) | wantBankLSB;
    }

    inline void setBank(uint16_t bank) {
        bankMSB = 0b01111111 & (bank >> 7);
        bankLSB = 0b01111111 & bank;
    }

    inline float getEffectiveVolume() const {
        return volume * expression;
    }

#ifdef ENABLE_CH_FILTER
    BiquadFilterInternalCoeffs filter;

    // Filter parameters
    float filterCutoff = 20000.0f;  // default no filtering
    float filterResonance = 0.707f;
    
    inline void updateFilter(float cutoff, float resonance) {
      filterCutoff = cutoff;
      filterResonance = resonance;
      filter.setFreqAndQ(cutoff, resonance);
    }
	
    inline void recalcFilter() {
        filter.setFreqAndQ(filterCutoff, filterResonance);
    }
    
    inline void resetFilter() {
      updateFilter(20000.0f, 0.707f);
    }

#elif defined(ENABLE_CH_FILTER_M)
    
    BiquadCalc::Coeffs  filterCoeffs;
    
    // Filter parameters
    float filterCutoff = 20000.0f;  // default no filtering
    float filterResonance = 0.707f;

    inline void updateFilter(float cutoff, float resonance) {
        filterCutoff = cutoff;
        filterResonance = resonance;
        filterCoeffs = BiquadCalc::calcCoeffs(cutoff, resonance, BiquadCalc::LowPass);
    }
	
    inline void recalcFilter() {
        filterCoeffs = BiquadCalc::calcCoeffs(filterCutoff, filterResonance, BiquadCalc::LowPass);
    }
    
    inline void resetFilter() {
      updateFilter(20000.0f, 0.707f);
    }
#endif

    inline void activityIncrease(uint8_t velo) {
        activity += (float)velo * DIV_127 * volume * expression;
        if (activity > 1.0f) { activity = 1.0f; }
    }

    inline void activityUpdate() {
        activity *= activitySmoothingFactor;
    }

    inline void reset() {
        isDrum = false;
        volume = 1.0f;         // CC#7
        pan = 0.0f;            // CC#10
        expression = 1.0f;     // CC#11
        pitchBend = 0.0f;         // Center
        pitchBendRange = 2.0f;     // Default
        pitchBendFactor = 1.0f; // No pitch bend
        modWheel = 0.0f;
        reverbSend = 0.05f; // CC#91
        chorusSend = 0.0f;  // CC#93
        delaySend = 0.0f;   // CC#95
        sustainPedal = false; // CC#64
        portaTime = 0.2f;      // CC#5
        portamento = false;    // CC#65
        attackModifier = 1.0f;  // CC#73
        releaseModifier = 1.0f; // CC#72
        tuningSemitones = 0.0f; //
        monoMode = Poly;
        clearNoteStack() ;
#if defined(ENABLE_CH_FILTER) || defined(ENABLE_CH_FILTER_M)
        resetFilter();
#endif
      
    }
	
	std::vector<SampleHeader*> loadedSamples;
};

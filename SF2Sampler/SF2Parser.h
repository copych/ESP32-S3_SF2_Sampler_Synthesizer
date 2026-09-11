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
 * File: SF2Parser.h
 * Purpose: SF2 file parser class header.
 * ----------------------------------------------------------------------------
 */


#pragma once
#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <map>
#include "SamplePool.h"

#include <LittleFS.h>





struct __attribute__((packed)) Generator {
    uint16_t oper;
    union {
        struct __attribute__((packed)) {
            uint8_t lo;
            uint8_t hi;
        } range;
        uint16_t uAmount;
        int16_t sAmount;
    } amount;
};

struct SampleHeader {

    char name[20];

    uint32_t start;
    uint32_t end;
    uint32_t startLoop;
    uint32_t endLoop;
    uint32_t sampleRate;

    uint8_t originalPitch;
    int8_t pitchCorrection;
    uint16_t sampleLink;
    uint16_t sampleType;

    const int16_t* data;
    size_t dataSize = 0;
    uint32_t sampleID = 0;

    bool cached = false;
    uint8_t refCount = 0;   // channels using this sample
	uint16_t poolBlocks = 0; 
};


struct Zone {    // --- Mandatory params ---
    uint8_t velLo = 0;
    uint8_t velHi = 127;
    uint8_t keyLo = 0;
    uint8_t keyHi = 127;
    SampleHeader* sample = nullptr;
    uint32_t sampleID;
    
    // --- generators from SF2 ---
    int rootKey = -1;          // OverridingRootKey (if no -> sample->originalPitch)
    int sampleModes = 0;       // SampleModes (loop type)
    int exclusiveClass = 0;    // ExclusiveClass
    float fineTune = 0.0f;     // FineTune (cents)
    float coarseTune = 0.0f;   // CoarseTune (semitones)
    
    // --- amp envelope ---
    float attackTime = 0.0f;   // AttackVolEnv (seconds)
    float holdTime = 0.0f;
    float decayTime = 0.0f;    // DecayVolEnv
    float sustainLevel = 1.0f; // SustainVolEnv (0.0–1.0)
    float releaseTime = 0.0f;  // ReleaseVolEnv
    float pan = 0.0f;          // Pan (-1.0–1.0)
    float modAttackTime = 0.0f;
    float modReleaseTime = 0.0f;
    float modDecayTime = -0.1f;
    float modSustainLevel = 0.0f;
    float attenuation = 1.0f;
    float modEnvToPitch = 0.0f;

    // Vibrato LFO (pitch modulation only)
    float vibLfoFreq = 0.0f;       // Hz
    float vibLfoDelay = 0.0f;      // seconds
    float vibLfoToPitch = 0.0f;    // cents

    // Modulation LFO
    float modLfoFreq = 0.0f;          // Hz
    float modLfoDelay = 0.0f;         // seconds
    float modLfoToPitch = 0.0f;       // cents
    float modLfoToVolume = 0.0f;      // centibels
    float modLfoToFilterFc = 0.0f;    // cents
    
    // --- filter ---
    float filterFc = 13500.0f; // InitialFilterFc (cutoff)
    float filterQ = 0.0f;      // InitialFilterQ (reso)
    
    // --- effects ---
    float reverbSend = 0.0f;   // ReverbEffectsSend (0.0–1.0)
    float chorusSend = 0.0f;   // ChorusEffectsSend
    
    int32_t loopStartOffset = 0;
    int32_t loopEndOffset = 0;
    int32_t loopStartCoarseOffset = 0;
    int32_t loopEndCoarseOffset = 0;

};

struct SF2Zone {
    std::vector<Generator> generators;
};

struct SF2Instrument {
    String name;
    std::vector<SF2Zone> zones;
    std::vector<Generator> globalGenerators;
};

struct SF2Preset {
    String name;
    uint16_t bank;
    uint16_t program;
    std::vector<SF2Zone> zones;
    std::vector<Generator> globalGenerators;
};



class SF2Parser {
public:
    SF2Parser(const char* path, fs::FS* fs = &LittleFS);
    bool parse();
    std::vector<SampleHeader>& getSamples();
    std::vector<Zone> getZonesForNote(uint8_t note, uint8_t velocity, uint16_t bank, uint16_t program);
    std::vector<SF2Preset> & getPresets() { return presets; } 
    const std::vector<SF2Preset>& getPresets() const { return presets; } 
    void dumpPresetStructure() ;
    bool hasPreset(uint16_t bank, uint16_t program) const ;
    void clear();
    std::vector<SampleHeader*> getSamplesForPreset(uint16_t bank, uint16_t program);
    bool loadSample(SampleHeader& s);
 
    SampleHandle* readSampleIntoPool(uint32_t sid);
    bool preloadAllIntoPool();
    void dumpInstrumentSizes();

private:
    uint32_t smplStart;
    bool parseHeaderChunks();
    bool parseSDTA();
    bool parsePDTA();
    void seekTo(uint32_t offset);
    bool parseInstrumentZones();
    bool readSampleHeaders(uint32_t offset, uint32_t size);
    SampleHeader* resolveSample(uint32_t sampleID);
    uint32_t hashSampleName(const char* name) ; 
    void applyGenerators(const std::vector<Generator>& gens, Zone& zone) ;

    File file; 
    String filepath;
    
    fs::FS* filesystem;
    std::vector<SampleHeader>  samples;
    std::vector<Zone> zones; 
    std::vector<SF2Preset> presets;
    std::vector<SF2Instrument> instruments;
    std::map<int, SampleHeader*> sampleMap;
    std::map<uint32_t, SampleHeader*> startPosMap;

    uint32_t sdtaOffset = 0;
    uint32_t sdtaSize = 0;
    uint32_t shdrOffset = 0;
    uint32_t pdtaOffset = 0;
    uint32_t pdtaSize = 0;

};



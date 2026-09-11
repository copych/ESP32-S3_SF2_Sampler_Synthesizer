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
 * File: synth.h
 * Purpose: SF2 synthesizer class
 * ----------------------------------------------------------------------------
 */

#pragma once
#include "channel.h"
#include "voice.h"
#include "SF2Parser.h"

enum class FileSystemType {
    LITTLEFS,
    SD
};

class DRAM_ATTR Synth {
public:

    Synth(SF2Parser& parserRef);
    bool begin();
    void noteOn(uint8_t ch, uint8_t note, uint8_t vel);
    void noteOff(uint8_t ch, uint8_t note);
    void controlChange(uint8_t ch, uint8_t control, uint8_t value);
    void allNotesOff(uint8_t ch);
    bool handleSysEx(const uint8_t* data, size_t len); 
    void soundOff(uint8_t ch);
    void reset();
    void applyBankProgram(uint8_t ch);
    void programChange(uint8_t channel, uint8_t program);
    void pitchBend(uint8_t ch, int value);
    void updateScores();
    void printState();
    void renderLR(float* sampleL, float* sampleR);
    void GMReset(); 
    ChannelState& getChannelState(uint8_t channel) { return channels[channel]; }
    void setFileSystem(FileSystemType type) { fsType = type; }
    bool loadSf2File(const char* path);
    bool loadNextSf2();
    void scanSf2Files();
    void renderLRBlock(float*, float*);
    void setChannelMode(uint8_t ch, ChannelState::MonoMode mode);
    void getActivityString(char str[49]);
    void updateActivity();
    const std::vector<String>& getSf2List() const { return sf2Files; }
    int getCurrentSf2Index() const { return currentFileIndex; }
    FileSystemType getCurrentFsType() const { return fsType; }
    ChannelState channels[16];
    bool loadSf2ByIndex(int index);
    SF2Parser& parser;
    bool loadSynthState(const char* path=DEFAULT_CONFIG_FILE);
    bool saveSynthState(const char* path=DEFAULT_CONFIG_FILE);
    const String& getCurrentSf2Path() const { return currentSf2Path; }

    // Asset residency changes run on Core1 while audio renders on Core0.
    // These form a block-boundary handshake: Core1 never invalidates PCM
    // until Core0 has acknowledged that it is outside renderLRBlock().
    void beginAssetUpdate();
    void endAssetUpdate();
    bool audioAssetUpdateRequested() const;
    void setAudioAssetPaused(bool paused);

private:
    
    String currentSf2Path;  // full path of currently loaded SF2 file
    float volume_scaler = 0.5f ;
    int currentFileIndex = -1;
    float pitchBendRatio(int value);
    Voice* allocateVoice(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass);
    Voice* findWeakestVoiceOnNote(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass);
    Voice* findWorstVoice();

    Voice voices[MAX_VOICES];

    fs::FS* getFileSystem() ;

//    FileSystemType fsType = FileSystemType::LITTLEFS;  // default
    FileSystemType fsType = FileSystemType::SD;  // default
    std::vector<String> sf2Files;

    volatile bool assetUpdateRequested = false;
    volatile bool audioAssetPaused = false;
    uint8_t assetUpdateDepth = 0;
};

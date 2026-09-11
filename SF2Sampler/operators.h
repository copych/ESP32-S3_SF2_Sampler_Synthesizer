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
 * File: operators.h
 * Purpose: Helper enums and functions for SF2 generator operators
 * ----------------------------------------------------------------------------
 */
#pragma once

enum class GeneratorOperator : uint16_t {
 // Invalid                    = 0xFFFF,
    StartAddrOffset            = 0,
    EndAddrOffset              = 1,
    StartLoopAddrOffset        = 2,
    EndLoopAddrOffset          = 3,
    StartAddrCoarseOffset      = 4,
    ModLfoToPitch              = 5,
    VibLfoToPitch              = 6,
    ModEnvToPitch              = 7,
    InitialFilterFc            = 8,
    InitialFilterQ             = 9,
    ModLfoToFilterFc           = 10,
    ModEnvToFilterFc           = 11,
    EndAddrCoarseOffset        = 12,
    ModLfoToVolume             = 13,
    ChorusEffectsSend          = 15,
    ReverbEffectsSend          = 16,
    Pan                        = 17,
    ModLfoDelay                = 21,
    ModLfoFreq                 = 22,
    VibLfoDelay                = 23,
    VibLfoFreq                 = 24,
    DelayModEnv                = 25,
    AttackModEnv               = 26,
    HoldModEnv                 = 27,
    DecayModEnv                = 28,
    SustainModEnv              = 29,
    ReleaseModEnv              = 30,
    KeynumToModEnvHold         = 31,
    KeynumToModEnvDecay        = 32,
    DelayVolEnv                = 33,
    AttackVolEnv               = 34,
    HoldVolEnv                 = 35,
    DecayVolEnv                = 36,
    SustainVolEnv              = 37,
    ReleaseVolEnv              = 38,
    KeynumToVolEnvHold         = 39,
    KeynumToVolEnvDecay        = 40,
    Instrument                 = 41,
    KeyRange                   = 43,
    VelRange                   = 44,
    StartLoopAddrCoarseOffset  = 45,
    Keynum                     = 46,
    Velocity                   = 47,
    InitialAttenuation         = 48,
    EndLoopAddrCoarseOffset    = 50,
    CoarseTune                 = 51,
    FineTune                   = 52,
    SampleID                   = 53,
    SampleModes                = 54,
    ScaleTuning                = 56,
    ExclusiveClass             = 57,
    OverridingRootKey          = 58,
};

inline const char* toString(GeneratorOperator op) {
    switch (op) {
        case GeneratorOperator::StartAddrOffset:            return "StartAddrOffset";
        case GeneratorOperator::EndAddrOffset:              return "EndAddrOffset";
        case GeneratorOperator::StartLoopAddrOffset:        return "StartLoopAddrOffset";
        case GeneratorOperator::EndLoopAddrOffset:          return "EndLoopAddrOffset";
        case GeneratorOperator::StartAddrCoarseOffset:      return "StartAddrCoarseOffset";
        case GeneratorOperator::ModLfoToPitch:              return "ModLfoToPitch";
        case GeneratorOperator::VibLfoToPitch:              return "VibLfoToPitch";
        case GeneratorOperator::ModEnvToPitch:              return "ModEnvToPitch";
        case GeneratorOperator::InitialFilterFc:            return "InitialFilterFc";
        case GeneratorOperator::InitialFilterQ:             return "InitialFilterQ";
        case GeneratorOperator::ModLfoToFilterFc:           return "ModLfoToFilterFc";
        case GeneratorOperator::ModEnvToFilterFc:           return "ModEnvToFilterFc";
        case GeneratorOperator::EndAddrCoarseOffset:        return "EndAddrCoarseOffset";
        case GeneratorOperator::ModLfoToVolume:             return "ModLfoToVolume";
        case GeneratorOperator::ChorusEffectsSend:          return "ChorusEffectsSend";
        case GeneratorOperator::ReverbEffectsSend:          return "ReverbEffectsSend";
        case GeneratorOperator::Pan:                        return "Pan";
        case GeneratorOperator::ModLfoDelay:                return "ModLfoDelay";
        case GeneratorOperator::ModLfoFreq:                 return "ModLfoFreq";
        case GeneratorOperator::VibLfoDelay:                return "VibLfoDelay";
        case GeneratorOperator::VibLfoFreq:                 return "VibLfoFreq";
        case GeneratorOperator::DelayModEnv:                return "DelayModEnv";
        case GeneratorOperator::AttackModEnv:               return "AttackModEnv";
        case GeneratorOperator::HoldModEnv:                 return "HoldModEnv";
        case GeneratorOperator::DecayModEnv:                return "DecayModEnv";
        case GeneratorOperator::SustainModEnv:              return "SustainModEnv";
        case GeneratorOperator::ReleaseModEnv:              return "ReleaseModEnv";
        case GeneratorOperator::KeynumToModEnvHold:         return "KeynumToModEnvHold";
        case GeneratorOperator::KeynumToModEnvDecay:        return "KeynumToModEnvDecay";
        case GeneratorOperator::DelayVolEnv:                return "DelayVolEnv";
        case GeneratorOperator::AttackVolEnv:               return "AttackVolEnv";
        case GeneratorOperator::HoldVolEnv:                 return "HoldVolEnv";
        case GeneratorOperator::DecayVolEnv:                return "DecayVolEnv";
        case GeneratorOperator::SustainVolEnv:              return "SustainVolEnv";
        case GeneratorOperator::ReleaseVolEnv:              return "ReleaseVolEnv";
        case GeneratorOperator::KeynumToVolEnvHold:         return "KeynumToVolEnvHold";
        case GeneratorOperator::KeynumToVolEnvDecay:        return "KeynumToVolEnvDecay";
        case GeneratorOperator::Instrument:                 return "Instrument";
        case GeneratorOperator::KeyRange:                   return "KeyRange";
        case GeneratorOperator::VelRange:                   return "VelRange";
        case GeneratorOperator::StartLoopAddrCoarseOffset:  return "StartLoopAddrCoarseOffset";
        case GeneratorOperator::Keynum:                     return "Keynum";
        case GeneratorOperator::Velocity:                   return "Velocity";
        case GeneratorOperator::InitialAttenuation:         return "InitialAttenuation";
        case GeneratorOperator::EndLoopAddrCoarseOffset:    return "EndLoopAddrCoarseOffset";
        case GeneratorOperator::CoarseTune:                 return "CoarseTune";
        case GeneratorOperator::FineTune:                   return "FineTune";
        case GeneratorOperator::SampleID:                   return "SampleID";
        case GeneratorOperator::SampleModes:                return "SampleModes";
        case GeneratorOperator::ScaleTuning:                return "ScaleTuning";
        case GeneratorOperator::ExclusiveClass:             return "ExclusiveClass";
        case GeneratorOperator::OverridingRootKey:          return "OverridingRootKey";
        default:                                            return "UnknownOperator";
    }
}
// number to Gen
inline GeneratorOperator toGeneratorOperator(uint16_t raw) {
    if (raw <= static_cast<uint16_t>(GeneratorOperator::OverridingRootKey)) {
        return static_cast<GeneratorOperator>(raw);
    }
    return GeneratorOperator::StartAddrOffset;
    //return GeneratorOperator::Invalid;
}


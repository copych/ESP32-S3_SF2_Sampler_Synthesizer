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
 * File: synth.cpp
 * Purpose: SF2 synthesizer core logic
 * ----------------------------------------------------------------------------
 */

#include "synth.h"
#include "config.h"
#include "dump.h"
#include <float.h>
#include <math.h>
#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include "TLVStorage.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static void* g_dma_reserve = nullptr;
static size_t g_dma_reserve_size = 64 * 1024;

static void dma_reserve_acquire()
{
    g_dma_reserve = heap_caps_malloc(g_dma_reserve_size, MALLOC_CAP_DMA);

    if (!g_dma_reserve) {
        ESP_LOGE("DMA", "reserve FAILED (%u)", (unsigned)g_dma_reserve_size);
        return;
    }

    ESP_LOGI("DMA", "reserve ACQUIRED ptr=%p size=%u",
             g_dma_reserve, (unsigned)g_dma_reserve_size);

    // Optional but recommended: touch memory
    memset(g_dma_reserve, 1, g_dma_reserve_size);
}

static void dma_reserve_release()
{
    if (g_dma_reserve) {
        heap_caps_free(g_dma_reserve);
        ESP_LOGI("DMA", "reserve RELEASED");

        g_dma_reserve = nullptr;
    }
}

static void dump_mem_caps(const char* tag)
{
    size_t free_8bit   = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_dma    = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    size_t largest_8bit   = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t largest_dma    = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI("MEM",
        "[%s] free: 8bit=%u dma=%u psram=%u | largest: 8bit=%u dma=%u psram=%u",
        tag,
        (unsigned)free_8bit,
        (unsigned)free_dma,
        (unsigned)free_spiram,
        (unsigned)largest_8bit,
        (unsigned)largest_dma,
        (unsigned)largest_spiram
    );
}

#ifdef ENABLE_CH_FILTER_M
    #include "biquad2.h"
#endif

#ifdef ENABLE_CHORUS
    #include "fx_chorus.h"
    extern FxChorus chorus;
#endif

#ifdef ENABLE_REVERB
    #include "fx_reverb.h"
    extern FxReverb reverb;
#endif

#ifdef ENABLE_DELAY
    #include "fx_delay.h"
    extern FxDelay delayfx;
#endif

#include "SamplePool.h"
extern SamplePool samplePool;

static const char* TAG = "Synth";

Synth::Synth(SF2Parser& parserRef) : parser(parserRef) {
    // Initialize all 16 MIDI channels with default values
    for (int i = 0; i < 16; ++i) {
        channels[i] = ChannelState();  // Default-initialized
    }

    // Initialize all voices
    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i].init();
    }

    volume_scaler = 1.0f / sqrtf(MAX_VOICES);
}

void Synth::beginAssetUpdate() {
    // Nested operations occur during SF2 load -> GMReset -> programChange().
    // Only the outermost operation owns the Core0 pause handshake.
    if (assetUpdateDepth++ != 0) return;

    __atomic_store_n(&assetUpdateRequested, true, __ATOMIC_RELEASE);

    // Core0 acknowledges only between render blocks. Until then no PCM
    // pointer or pool allocation may be invalidated.
    while (!__atomic_load_n(&audioAssetPaused, __ATOMIC_ACQUIRE)) {
        taskYIELD();
    }
}

void Synth::endAssetUpdate() {
    if (assetUpdateDepth == 0) return;
    if (--assetUpdateDepth != 0) return;

    __atomic_store_n(&assetUpdateRequested, false, __ATOMIC_RELEASE);
}

bool Synth::audioAssetUpdateRequested() const {
    return __atomic_load_n(&assetUpdateRequested, __ATOMIC_ACQUIRE);
}

void Synth::setAudioAssetPaused(bool paused) {
    __atomic_store_n(&audioAssetPaused, paused, __ATOMIC_RELEASE);
}

bool Synth::begin() {
    // loadSynthState() may load/parse an SF2. loadSf2File() now creates the
    // sample arena only AFTER parser metadata is resident.
    if (loadSynthState() && !parser.getSamples().empty()) return true;

    // No saved SF2 was restored. Parse the parser's configured file with all
    // PSRAM available to metadata, then give the remaining PSRAM to samples.
    samplePool.deinit();

    dump_mem_caps("before_parse");
    dma_reserve_acquire();
    dump_mem_caps("after_acquire");

    if (!parser.parse()) {
        ESP_LOGW(TAG, "No SF2 parsed. Auto-loading next SF2...");
        dump_mem_caps("before_release");
        dma_reserve_release();
        dump_mem_caps("after_release");
        return loadNextSf2();
    }

    dma_reserve_release();
    dump_mem_caps("after_parse_before_pool");

    if (!samplePool.init(1300 * 1024)) { // bytes kept free after metadata
        ESP_LOGE(TAG, "Sample pool initialization failed after SF2 parse");
        return false;
    }

    for (int ch = 0; ch < 16; ++ch) {
        programChange(ch, channels[ch].program);
    }
    return true;
}

void Synth::noteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    if (vel == 0) {
        noteOff(ch, note);
        return;
    }

#ifdef ENABLE_GUI
    block_gui();
#endif

    ChannelState* chan = &channels[ch];

    chan->activityIncrease(vel);

    bool isMono = chan->monoMode != ChannelState::Poly;
    bool retrig = chan->monoMode != ChannelState::MonoLegato;

    auto zones = parser.getZonesForNote(note, vel, chan->getBank(), chan->program);
    ESP_LOGI("NOTE", "ON ch=%u note=%u vel=%u bank=%u prog=%u zones=%u",
             ch, note, vel, chan->getBank(), chan->program, (uint32_t)zones.size());
    if (zones.empty()) {
        ESP_LOGE("NOTE", "NO ZONES ch=%u note=%u bank=%u prog=%u",
                 ch, note, chan->getBank(), chan->program);
        return;
    }

    chan->pushNote(note);

    if (isMono) {
        if (retrig) {
            // Kill all existing voices on this channel
            for (auto& v : voices)
                if (v.active && v.channel == ch)
                    v.die();

            // Start new voices for all zones
            for (auto& zone : zones) {
                float score = vel * DIV_127;

                SampleHandle* h = samplePool.acquire(zone.sampleID);
                if (!h) {
                    ESP_LOGE("NOTE", "ACQUIRE FAIL ch=%u note=%u sid=%u [mono-retrig]",
                             ch, note, zone.sampleID);
                    continue;
                }
                ESP_LOGI("NOTE", "ACQUIRE OK ch=%u note=%u sid=%u ptr=%p len=%u rate=%u",
                         ch, note, zone.sampleID, h->data, h->length, h->sampleRate);

                Voice* v = allocateVoice(ch, note, score, zone.exclusiveClass); 
                if (v) {
                    v->sampleHandle = h;
                    v->sampleID = zone.sampleID;
                    v->startNew(ch, note, vel, zone, chan);
                    ESP_LOGI("NOTE", "VOICE START ch=%u note=%u sid=%u voice=%p active=%u",
                             ch, note, zone.sampleID, v, (unsigned)v->active);
                } else {
                    ESP_LOGE("NOTE", "VOICE ALLOC FAIL ch=%u note=%u sid=%u", ch, note, zone.sampleID);
                    samplePool.release(zone.sampleID);
                }
            }
        } else {
            // Legato: update pitch of ALL existing voices, or start new if none
            bool reused = false;
            for (Voice& v : voices) {
                if (  v.active && v.channel == ch) {
                    if (!v.noteHeld) {
                        v.die();
                    } else {
                        v.updatePitchOnly(note, chan);
                        reused = true;
                    }
                }
            }
            if (!reused) {
                // First note: start new voices
                for (auto& zone : zones) {
                    if (zone.sampleID >= parser.getSamples().size()) continue;

                    SampleHandle* h = samplePool.acquire(zone.sampleID);
                    if (!h) {
                        ESP_LOGE("NOTE", "ACQUIRE FAIL ch=%u note=%u sid=%u [mono-legato-first]",
                                 ch, note, zone.sampleID);
                        continue;
                    }
                    ESP_LOGI("NOTE", "ACQUIRE OK ch=%u note=%u sid=%u ptr=%p len=%u rate=%u",
                             ch, note, zone.sampleID, h->data, h->length, h->sampleRate);

                    float score = vel * DIV_127;
                    Voice* v = allocateVoice(ch, note, score, zone.exclusiveClass);
                    if (v) {
                        v->sampleHandle = h;
                        v->sampleID = zone.sampleID;
                        v->startNew(ch, note, vel, zone, chan);
                        ESP_LOGI("NOTE", "VOICE START ch=%u note=%u sid=%u voice=%p active=%u",
                                 ch, note, zone.sampleID, v, (unsigned)v->active);
                    } else {
                        ESP_LOGE("NOTE", "VOICE ALLOC FAIL ch=%u note=%u sid=%u", ch, note, zone.sampleID);
                        samplePool.release(zone.sampleID);
                    }
                }
            }
        }
    } else {
        // Polyphonic: start new voices normally
        for (auto& zone : zones) {
            if (zone.sampleID >= parser.getSamples().size()) continue;

            SampleHandle* h = samplePool.acquire(zone.sampleID);
            if (!h) {
                ESP_LOGE("NOTE", "ACQUIRE FAIL ch=%u note=%u sid=%u [poly]",
                         ch, note, zone.sampleID);
                continue;
            }
            ESP_LOGI("NOTE", "ACQUIRE OK ch=%u note=%u sid=%u ptr=%p len=%u rate=%u",
                     ch, note, zone.sampleID, h->data, h->length, h->sampleRate);

            float score = vel * DIV_127;
            Voice* v = allocateVoice(ch, note, score, zone.exclusiveClass);

            if (v) {
                v->sampleHandle = h;
                v->sampleID = zone.sampleID;
                v->startNew(ch, note, vel, zone, chan);
                ESP_LOGI("NOTE", "VOICE START ch=%u note=%u sid=%u voice=%p active=%u",
                         ch, note, zone.sampleID, v, (unsigned)v->active);
            } else {
                ESP_LOGE("NOTE", "VOICE ALLOC FAIL ch=%u note=%u sid=%u", ch, note, zone.sampleID);
                samplePool.release(zone.sampleID);
            }
        }
    }
    chan->portaCurrentNote = note;
}

void Synth::noteOff(uint8_t ch, uint8_t note) {
    if (ch >= 16) return;
    
#ifdef ENABLE_GUI
    block_gui();
#endif

    ChannelState* chan = &channels[ch];
    bool isMono = chan->monoMode != ChannelState::Poly;
    bool isRetrig = chan->monoMode == ChannelState::MonoRetrig;

    chan->removeNote(note);
    uint8_t nextNote = chan->topNote();

    for (Voice& v : voices) {
        if (!v.active || v.channel != ch) continue;

        if (isMono) {
            if (!chan->hasNotes()) {
                // No more held notes → kill all
                v.noteHeld = false;
                v.stop();
            } else if (isRetrig) {
                if (v.note == note) {
                    v.noteHeld = false;
                    v.die();
                }
            } else {
                // MonoLegato: switch pitch of ALL voices to next note
                if (v.note != nextNote) {
                    v.updatePitchOnly(nextNote, chan);
                }
            }
        } else {
            // Poly mode
            if (v.note == note) {
                v.noteHeld = false;
                v.stop();
            }
        }
    }
}


Voice*  __attribute__((always_inline))   Synth::allocateVoice(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass){
    Voice* v = findWeakestVoiceOnNote(ch, note, newScore, exclusiveClass);
    if (!v) v = findWorstVoice();

    // A returned slot may still own the previous voice's sample.
    // Reuse is immediate, so release it before assigning a new handle.
    if (v && v->active) v->kill();
    return v;
}
 

void Synth::pitchBend(uint8_t ch, int value) {
    if (ch >= 16) return;

#ifdef ENABLE_GUI
    block_gui();
#endif

    float norm = (value - PITCH_BEND_CENTER) * DIV_8192;
    float semis = norm * channels[ch].pitchBendRange;

    channels[ch].pitchBend = norm;
    channels[ch].pitchBendFactor = exp2f(semis * DIV_12);

}


void Synth::controlChange(uint8_t ch, uint8_t ctrl, uint8_t val) {

    if (ch >= 16) return;

#ifdef ENABLE_GUI
    block_gui();
#endif

    auto& state = channels[ch];
    float fval = val * DIV_127;

    switch (ctrl) {
        case 0:  // Bank Select MSB
            state.wantBankMSB = val & 0x7F;
            break;
        case 32: // Bank Select LSB
            state.wantBankLSB = val & 0x7F;
            break;
        case 1:  // Mod Wheel
            state.modWheel = fval;  
            break;
        case 5:  // Portamento Time
            state.portaTime = fval;  
            break;
        case 7:  // Channel Volume
            state.volume = fval;
            break;
        case 11: // Expression
            state.expression = fval;
            break;
        case 10: // Pan
            state.pan = fval;
            for (Voice& v : voices) {
                if ( v.channel == ch) {
                    v.updatePan(); 
                }
            }
            break;
        case 64: // Sustain Pedal
            {
                bool sustainOn = val >= 64;
                state.sustainPedal = sustainOn;
                if (!sustainOn) {
                    // Release all sustained voices on this channel
                    for (Voice& v : voices) {
                        if (v.active && v.channel == ch && !v.noteHeld) {
                            v.stop(); 
                        }
                    }
                }
            }
            break;
        case 65: // Portamento on/off
            {
                bool portamento = val >= 64;
                state.portamento = portamento;
            }
            break;
#ifdef ENABLE_CH_FILTER
        case 71: // Filter Resonance
            state.filterResonance = knob_tbl[val] * (FILTER_MAX_Q - 0.5f) + 0.5f;
            state.recalcFilter();
            break;
        case 74: // Filter Cutoff
            state.filterCutoff = knob_tbl[val] * CH_FILTER_MAX_FREQ + CH_FILTER_MIN_FREQ;
            state.recalcFilter();
            break;
#elif defined(ENABLE_CH_FILTER_M)
        case 71: // Filter Resonance        
            state.filterResonance = knob_tbl[val] * (FILTER_MAX_Q - 0.5f) + 0.5f;
            state.filterCoeffs = BiquadCalc::calcCoeffs(state.filterCutoff, state.filterResonance, BiquadCalc::LowPass);
            break;
        case 74: // Filter Cutoff
            state.filterCutoff = knob_tbl[val] * CH_FILTER_MAX_FREQ + CH_FILTER_MIN_FREQ;
            state.filterCoeffs = BiquadCalc::calcCoeffs(state.filterCutoff, state.filterResonance, BiquadCalc::LowPass);
            break;
#endif        

        case 72: // Release time modifier (64-centered)
            state.releaseModifier = knob_tbl[val] * 4.8072f; // 1.0 at val=64
            break;
        case 73: // Attack time modifier (64-centered)
            state.attackModifier = knob_tbl[val] * 4.8072f; // 1.0 at val=64
            break;
		case 84: // Portamento control
			state.portaCurrentNote = val;
            break;
        case 91: // Reverb Send
            state.reverbSend = fval;
            break;
        case 93: // Chorus Send
            state.chorusSend = fval;
            break; 
        case 95: // Delay Send
            state.delaySend = fval;
            break;
		case 99: state.nrpn.msb = val; state.rpn = {0x7F, 0x7F}; break; // NRPN MSB (disable RPN)
		case 98: state.nrpn.lsb = val; state.rpn = {0x7F, 0x7F}; break; // NRPN LSB
		case 101: state.rpn.msb = val; state.nrpn = {0x7F, 0x7F}; break; // RPN MSB (disable NRPN)
		case 100: state.rpn.lsb = val; state.nrpn = {0x7F, 0x7F}; break; // RPN LSB

		case 6: // Data Entry MSB
			if (state.rpn.msb == 0 && state.rpn.lsb == 0) {
				state.pitchBendRange = static_cast<float>(val);
			} else if (state.nrpn.msb == 0x01 && state.nrpn.lsb == 0x10) { // nrpn 0x01 0x10 to switch mono/poly ()
				switch (val) {
					case 0: setChannelMode(ch, ChannelState::MonoLegato); break;
					case 1: setChannelMode(ch, ChannelState::MonoRetrig); break;
                    default:
					case 2: setChannelMode(ch, ChannelState::Poly);
				}
			}
			break;

		case 38: // Data Entry LSB
			// Optional: support for LSB pitch bend range or future NRPNs
			break;
        case 120: // All Sound Off
            soundOff(ch);
            break;
        case 121: // Reset All Controllers
            state.reset(); // Reset modulation values for channel
            break;
        case 123: // All Notes Off
            allNotesOff(ch);
            break;
		case 126: // Mono Mode On
			// val > 0 = MonoLegato with val voices, val == 0 = MonoRetrig
            setChannelMode(ch, (val > 0) ? ChannelState::MonoLegato : ChannelState::MonoRetrig);
			ESP_LOGI(TAG, "CC126: Channel %u → %s", ch+1, (val > 0 ? "MonoLegato" : "MonoRetrig"));
			break;
		case 127: // Poly Mode On
            setChannelMode(ch, ChannelState::Poly);
			ESP_LOGI(TAG, "CC127: Channel %u → Poly", ch+1);
			break;
        default:
            break;
    }
}

void Synth::applyBankProgram(uint8_t ch) {
    if (ch >= 16) return;
	
#ifdef ENABLE_GUI
    block_gui();
#endif

    auto& state = channels[ch];
    state.clearNoteStack(); 
    const uint8_t program = state.wantProgram;
    const uint16_t bank   = state.getWantBank();

    // === Detect Drum Channel ===
    if (ch == 9 || state.wantBankMSB == 127 || state.wantBankMSB == 120 || bank == 128) {
        state.isDrum = true;
    } else {
        state.isDrum = false;
    }

    // === Try Requested Bank ===
    if (parser.hasPreset(bank, program)) {
        state.program = program;
        state.setBank(bank);
        ESP_LOGD(TAG, "Ch%u: Program=%u, Bank=%u (%s)", ch+1, program, bank, state.isDrum ? "Drum" : "Melodic");
        return;
    }

    // === Melodic fallback: try Bank 0 ===
    if (!state.isDrum && parser.hasPreset(0, program)) {
        state.program = program;
        state.setBank(0);
        ESP_LOGW(TAG, "Ch%u: Bank %u not found, fallback to Bank 0 (Program=%u)", ch+1, bank, program);
        return;
    }

    // === Final fallback: Program 0, Bank depends on drum status ===
    const uint16_t fallbackBank = state.isDrum ? 128 : 0;
    if (parser.hasPreset(fallbackBank, 0)) {
        state.program = 0;
        state.setBank(fallbackBank);
        ESP_LOGW(TAG, "Ch%u: Fallback to Program=0, Bank=%u (%s)", ch+1, fallbackBank, state.isDrum ? "Drum" : "Melodic");
    } else {
        ESP_LOGE(TAG, "Ch%u: No valid preset for Program=%u in any known bank", ch+1, program);
    }
}

void Synth::programChange(uint8_t ch, uint8_t program) {
    if (ch >= 16) return;

    beginAssetUpdate();

    dump_mem_caps("before_prog_ch");

    auto& state = channels[ch];
    state.wantProgram = program & 0x7F;

    // Program Change is a hard boundary in this sampler. Old notes do not
    // survive it, which makes sample ownership deterministic.
    allNotesOff(ch);
    soundOff(ch);

    applyBankProgram(ch);

    auto& all = parser.getSamples();
    auto needed = parser.getSamplesForPreset(state.getBank(), state.program);

    ESP_LOGI("PC", "ch=%u bank=%u program=%u needed=%u totalSamples=%u",
             ch, state.getBank(), state.program,
             (uint32_t)needed.size(), (uint32_t)all.size());

    auto containsSample = [](const std::vector<SampleHeader*>& list,
                             const SampleHeader* sample) -> bool {
        for (auto* item : list)
            if (item == sample) return true;
        return false;
    };

    // First drop samples which this channel no longer needs. Shared samples
    // stay pinned, so the normal browsing path reuses as much PCM as possible.
    auto oldLoaded = std::move(state.loadedSamples);
    state.loadedSamples.clear();

    for (auto* sample : oldLoaded) {
        if (!sample || containsSample(needed, sample)) continue;

        if (sample->refCount) sample->refCount--;
        samplePool.release(sample->sampleID);
        if (!samplePool.get(sample->sampleID)) sample->data = nullptr;
    }

    state.loadedSamples.reserve(needed.size());

    uint32_t loaded = 0;
    uint32_t reused = 0;
    bool incrementalOk = true;

    for (auto* sample : needed) {
        if (!sample || sample->sampleID >= all.size()) {
            incrementalOk = false;
            break;
        }

        // This channel already owned the sample before the PC. Keep that
        // ownership unchanged; no acquire/release pair is needed.
        if (containsSample(oldLoaded, sample)) {
            SampleHandle* h = samplePool.get(sample->sampleID);
            if (!h || !h->data) {
                ESP_LOGE("PC", "resident sample missing during reuse: ch=%u sid=%u", ch, sample->sampleID);
                incrementalOk = false;
                break;
            }

            ESP_LOGI("PC", "KEEP sid=%u ptr=%p len=%u", sample->sampleID, h->data, h->length);
            sample->data = h->data;
            state.loadedSamples.push_back(sample);
            reused++;
            continue;
        }

        const uint32_t sid = sample->sampleID;
        SampleHandle* h = samplePool.get(sid);

        if (!h) {
            ESP_LOGI("PC", "LOAD sid=%u", sid);
            h = parser.readSampleIntoPool(sid);
            if (!h) {
                ESP_LOGE("PC", "LOAD FAIL sid=%u", sid);
                incrementalOk = false;
                break;
            }
            loaded++;
        } else {
            ESP_LOGI("PC", "FOUND sid=%u ptr=%p len=%u", sid, h->data, h->length);
            reused++;
        }

        h = samplePool.acquire(sid);
        if (!h || !h->data) {
            ESP_LOGE("PC", "PIN FAIL sid=%u h=%p data=%p", sid, h, h ? h->data : nullptr);
            incrementalOk = false;
            break;
        }
        ESP_LOGI("PC", "PIN OK sid=%u ptr=%p len=%u", sid, h->data, h->length);

        sample->data = h->data;
        if (sample->refCount < UINT8_MAX) sample->refCount++;
        state.loadedSamples.push_back(sample);
    }

    if (incrementalOk) {
        ESP_LOGI("PC", "incremental OK: loaded=%u reused=%u",
                 loaded, reused);
        dump_mem_caps("after_prog_ch");
        endAssetUpdate();
        return;
    }

    ESP_LOGW("PC", "incremental allocation failed, rebuilding full layout");

    // A full rebuild invalidates every pool pointer. Stop all voices first.
    for (uint8_t c = 0; c < 16; ++c) soundOff(c);

    // Forget all ownership and allocation metadata. The PSRAM arena itself is
    // retained; only its logical layout is reset.
    for (uint8_t c = 0; c < 16; ++c) channels[c].loadedSamples.clear();
    for (auto& sample : all) {
        sample.data = nullptr;
        sample.refCount = 0;
    }
    samplePool.reset();

    // Rebuild the union of samples required by all currently selected channel
    // programs. Starting from an empty first-fit arena gives a packed layout.
    bool rebuildOk = true;
    uint32_t rebuiltLoaded = 0;
    uint32_t rebuiltReused = 0;

    for (uint8_t c = 0; c < 16 && rebuildOk; ++c) {
        auto channelNeeded = parser.getSamplesForPreset(
            channels[c].getBank(), channels[c].program);
        channels[c].loadedSamples.reserve(channelNeeded.size());

        for (auto* sample : channelNeeded) {
            if (!sample || sample->sampleID >= all.size()) {
                rebuildOk = false;
                break;
            }

            const uint32_t sid = sample->sampleID;
            SampleHandle* h = samplePool.get(sid);

            if (!h) {
                h = parser.readSampleIntoPool(sid);
                if (!h) {
                    rebuildOk = false;
                    break;
                }
                rebuiltLoaded++;
            } else {
                rebuiltReused++;
            }

            h = samplePool.acquire(sid);
            if (!h || !h->data) {
                rebuildOk = false;
                break;
            }

            sample->data = h->data;
            if (sample->refCount < UINT8_MAX) sample->refCount++;
            channels[c].loadedSamples.push_back(sample);
        }
    }

    if (!rebuildOk) {
        ESP_LOGE("PC", "FULL LAYOUT FAILED: current 16-channel working set does not fit");

        for (uint8_t c = 0; c < 16; ++c) channels[c].loadedSamples.clear();
        for (auto& sample : all) {
            sample.data = nullptr;
            sample.refCount = 0;
        }
        samplePool.reset();
        dump_mem_caps("after_prog_ch_fail");
        endAssetUpdate();
        return;
    }

    ESP_LOGI("PC", "full layout OK: loaded=%u reused=%u",
             rebuiltLoaded, rebuiltReused);
    dump_mem_caps("after_prog_ch_rebuild");
    endAssetUpdate();
}

// Inline filter processing to reduce function call overhead
#define PROCESS_FILTER_LR(flt, inL, inR) { \
    float tmpL = inL, tmpR = inR;         \
    flt.processLR(&tmpL, &tmpR);         \
    inL = tmpL; inR = tmpR;              \
}


void   __attribute__((hot,always_inline)) IRAM_ATTR Synth::renderLRBlock(float* outL, float* outR) {
     float dryL[DMA_BUFFER_LEN] = {0};
     float dryR[DMA_BUFFER_LEN] = {0};
#ifdef ENABLE_CHORUS
     float choL[DMA_BUFFER_LEN] = {0}, choR[DMA_BUFFER_LEN] = {0};
#endif
#ifdef ENABLE_REVERB
     float revL[DMA_BUFFER_LEN] = {0}, revR[DMA_BUFFER_LEN] = {0};
#endif
#ifdef ENABLE_DELAY
     float delL[DMA_BUFFER_LEN] = {0}, delR[DMA_BUFFER_LEN] = {0};
#endif

#ifdef ENABLE_CH_FILTER
    // Clear channel dry buffers before mixing
    for (int ch = 0; ch < 16; ++ch) {
        memset(channels[ch].dryL, 0, sizeof(float)*DMA_BUFFER_LEN);
        memset(channels[ch].dryR, 0, sizeof(float)*DMA_BUFFER_LEN);
    }
#endif

    for (int v = 0; v < MAX_VOICES; ++v) {
        Voice& voice = voices[v];
        if (!voice.active) continue;

        float volL = volume_scaler * (*voice.modVolume) * (*voice.modExpression) * voice.velocityVolume * voice.panL;
        float volR = volume_scaler * (*voice.modVolume) * (*voice.modExpression) * voice.velocityVolume * voice.panR;

#ifdef ENABLE_CHORUS
        float cAmt = voice.chorusAmount;
#endif
#ifdef ENABLE_REVERB
        float rAmt = voice.reverbAmount;
#endif
#ifdef ENABLE_DELAY
        float dAmt = channels[voice.channel].delaySend;
#endif

#ifdef ENABLE_CH_FILTER
        // Mix into channel dry buffers for filtering
        float* dryLp = channels[voice.channel].dryL;
        float* dryRp = channels[voice.channel].dryR;
#else
        // Mix directly into global dry buffers
        float* dryLp = dryL;
        float* dryRp = dryR;
#endif

#ifdef ENABLE_CHORUS
        float* choLp = choL;
        float* choRp = choR;
#endif
#ifdef ENABLE_REVERB
        float* revLp = revL;
        float* revRp = revR;
#endif
#ifdef ENABLE_DELAY
        float* delLp = delL;
        float* delRp = delR;
#endif

        for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
            float smp = voice.nextSample();

            float l = smp * volL;
            float r = smp * volR;

            dryLp[i] += l;
            dryRp[i] += r;

#ifdef ENABLE_CHORUS
            float lCho = l * cAmt;
            float rCho = r * cAmt;
            choLp[i] += lCho;
            choRp[i] += rCho;
#endif
#ifdef ENABLE_REVERB
            float lRev = l, rRev = r;
#ifdef ENABLE_CHORUS
            lRev += lCho; rRev += rCho;
#endif
            revLp[i] += lRev * rAmt;
            revRp[i] += rRev * rAmt;
#endif
#ifdef ENABLE_DELAY
            float lDel = l, rDel = r;
#ifdef ENABLE_CHORUS
            lDel += lCho; rDel += rCho;
#endif
            delLp[i] += lDel * dAmt;
            delRp[i] += rDel * dAmt;
#endif
        }
    }

#ifdef ENABLE_CH_FILTER
    // Process filters per channel and accumulate to global dry buffers
    for (int ch = 0; ch < 16; ++ch) {
        ChannelState& chan = channels[ch];
        float* bufL = chan.dryL;
        float* bufR = chan.dryR;

        for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
            float l = bufL[i];
            float r = bufR[i];
            PROCESS_FILTER_LR(chan.filter, l, r);
            dryL[i] += l;
            dryR[i] += r;
        }
    }
#endif

#ifdef ENABLE_CHORUS
    chorus.processBlock(choL, choR);
#endif
#ifdef ENABLE_DELAY
    delayfx.ProcessBlock(delL, delR);
#endif
#ifdef ENABLE_REVERB
    reverb.processBlock(revL, revR);
#endif

    for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
        outL[i] = dryL[i];
        outR[i] = dryR[i];
#ifdef ENABLE_CHORUS
        outL[i] += choL[i]; outR[i] += choR[i];
#endif
#ifdef ENABLE_REVERB
        outL[i] += revL[i]; outR[i] += revR[i];
#endif
#ifdef ENABLE_DELAY
        outL[i] += delL[i]; outR[i] += delR[i];
#endif
    }
}


Voice* Synth::findWeakestVoiceOnNote(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass) {
    Voice* weakest = nullptr;
    float weakestScore = FLT_MAX;
    int count = 0;

    for (int i = 0; i < MAX_VOICES; ++i) {
        Voice& v = voices[i];
        if (v.active && v.channel == ch) { 
            if (v.exclusiveClass > 0 && v.exclusiveClass == exclusiveClass) {
                v.die(); // Kill voices of the same class
            }
            if (v.note == note) {
                count++;
                v.updateScore();
                if (v.score < weakestScore) {
                    weakestScore = v.score;
                    weakest = &v;
                }
            }
        }
    }

    if (count >= MAX_VOICES_PER_NOTE && weakest)// && weakestScore < newScore)
        return weakest;

    return nullptr;
}

Voice* Synth::findWorstVoice() {
    Voice* worst = nullptr;
    float minScore = FLT_MAX;

    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i].updateScore();
        if (!voices[i].active || !voices[i].isRunning()) return &voices[i];

        if (voices[i].score < minScore) {
            minScore = voices[i].score;
            worst = &voices[i];
        }
    }

    return worst;
}

void Synth::updateScores() {
    for (Voice& v : voices) {
        v.updateScore();
        if (!v.active) continue;
        v.updatePitch();
        v.updatePitchFactors();
    }
}

void Synth::reset() {
    // Release voice ownership before touching channel state.
    for (Voice& v : voices) v.kill();

    // Release one pool reference for every channel-owned sample.
    for (int ch = 0; ch < 16; ++ch) {
        for (auto* sample : channels[ch].loadedSamples) {
            if (!sample) continue;
            if (sample->refCount) sample->refCount--;
            samplePool.release(sample->sampleID);
            if (!samplePool.get(sample->sampleID)) sample->data = nullptr;
        }
        channels[ch].loadedSamples.clear();
        channels[ch].reset();
    }
}

void Synth::soundOff(uint8_t ch) {
    if (ch >= 16) return;
    for (Voice& v : voices) {
        if (v.active && v.channel == ch) {
            v.kill();
        }
    }
}

void Synth::allNotesOff(uint8_t ch) {
    if (ch >= 16) return;
    for (Voice& v : voices) {
        if (v.active && v.channel == ch) {
            if (*v.modSustain) {
               // v.sustainHeld = true; // wait until pedal release
            } else {
                v.stop();
            }
        }
    }
}

void Synth::GMReset() {
	
#ifdef ENABLE_GUI
    block_gui();
#endif

    for (uint8_t ch = 0; ch < 16; ++ch) {
        auto& state = channels[ch];
        state.reset();

        // Set default program and bank requests
        state.wantProgram = 0;
        state.wantBankMSB = (ch == 9) ? 1 : 0; // Bank 128
        state.wantBankLSB = 0;

        applyBankProgram(ch);

        allNotesOff(ch);
        soundOff(ch);
    }
    for (int ch = 0; ch < 16; ++ch) {
        channels[ch].program = 0;
        channels[ch].setBank(0);
        programChange(ch, 0);
    }
    ESP_LOGI(TAG, "General MIDI Reset complete, Channel 10 locked to drum bank.");
}

bool Synth::handleSysEx(const uint8_t* data, size_t len) {
	
#ifdef ENABLE_GUI
    block_gui();
#endif

    if (len == 6 &&
        data[0] == 0xF0 &&
        data[1] == 0x7E &&
        //data[2] is device ID (can be 0x7F for "all devices") */
        data[3] == 0x09 &&
        data[4] == 0x01 &&
        data[5] == 0xF7) {
        
        GMReset();
        ESP_LOGI(TAG, "Received GM System On SysEx");
        return true;
    }
	
	// XG System On (F0 43 1x 4C 00 00 7E 00 F7)
    if (len == 9 &&
        data[0] == 0xF0 &&
        data[1] == 0x43 &&
        // data[2] == 0x10 && 	// is device ID, ignored
        data[3] == 0x4C &&
        data[4] == 0x00 &&
        data[5] == 0x00 &&
        data[6] == 0x7E &&
        data[7] == 0x00 &&
        data[8] == 0xF7) {

        GMReset(); // we actually fully reset the synth
        ESP_LOGI(TAG, "Received XG System On SysEx");
        return true;
    }
    

    if (len == 9 &&
    data[0] == 0xF0 &&
    data[1] == 0x43 &&
    // data[2] == 0x10 //(device ID)
    data[3] == 0x4C &&
    data[4] == 0x08 &&
    data[8] == 0xF7) {
        uint8_t part = data[5]; // MIDI channel 0–15
        uint8_t param = data[6]; 
        uint8_t val = data[7]; // 0 = GM, 1 = Drum
        if (part < 16) {
            auto& state = channels[part];
            if (param == 0x05) {
                bool mono = (val == 0x00);
                setChannelMode(part, mono ? ChannelState::MonoLegato : ChannelState::Poly );
                ESP_LOGI(TAG, "Received XG Mono/Poly SysEx: Part %u → %s", part + 1, mono ? "Mono" : "Poly");
                return true;
            } else if (param == 0x08) {
                state.tuningSemitones = val - 64.0f;
                ESP_LOGI(TAG, "Received XG part note shift SysEx: Part %u → %d", part + 1, (int)(val-64.0f));
                return true;
            } else if (param == 0x07) {
                if (val == 0) {
                    state.wantBankMSB = 0;
                    state.wantBankLSB = 0;
                    ESP_LOGI(TAG, "XG: Ch%u set to General MIDI (Bank 0)", part+1);
                } else {
                    state.wantBankMSB = 1;  // Bank 128
                    state.wantBankLSB = 0;
                    ESP_LOGI(TAG, "XG: Ch%u set to Drum Kit (Bank 128)", part+1);
                }

                applyBankProgram(part);
                return true;
            }
        }
    }
	
    return false;
     
}

fs::FS* Synth::getFileSystem() {
    switch (fsType) {
        case FileSystemType::LITTLEFS: return &LittleFS;
        case FileSystemType::SD: return &SD_MMC;
        default: return nullptr;
    }
}

void Synth::scanSf2Files() {
    sf2Files.clear();
    fs::FS* fs = getFileSystem();
    if (!fs) return;

    File dir = fs->open(SF2_PATH);
    if (!dir || !dir.isDirectory()) {
        ESP_LOGE("Synth", "Can't open directory %s", SF2_PATH);
        return;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        String lower = name;
        lower.toLowerCase();
        if (!entry.isDirectory() && lower.endsWith(".sf2")) {
            sf2Files.push_back(name);
        }
    }

    dir.close();
    currentFileIndex = -1;
}

bool Synth::loadSf2File(const char* filename) {
    beginAssetUpdate();

    // First terminate every user of the current PCM while the old arena still
    // exists, then return the entire arena to PSRAM before parsing metadata.
    reset();
    samplePool.deinit();
    parser.clear();

    ESP_LOGI(TAG, "\n\nFree heap before SF2 parse: %u, PSRAM: %u\n\n",
             heap_caps_get_free_size(MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    fs::FS* fs = getFileSystem();
    if (!fs) {
        ESP_LOGE("Synth", "Filesystem not initialized");
        endAssetUpdate();
        return false;
    }

    String fullPath(filename);
    if (!fullPath.startsWith(SF2_PATH))
        fullPath = String(SF2_PATH) + filename;

    ESP_LOGI("Synth", "\n\nLoading SF2: %s\n\n", fullPath.c_str());

    SF2Parser tempParser(fullPath.c_str(), fs);
    dump_mem_caps("before_tmp_parse");
    dma_reserve_acquire();

    if (!tempParser.parse()) {
        ESP_LOGE("Synth", "Failed to parse %s", fullPath.c_str());
        dma_reserve_release();
        dump_mem_caps("after_!tmp_parse");
        endAssetUpdate();
        return false;
    }

    dma_reserve_release();
    parser = std::move(tempParser);
    dump_mem_caps("after_tmp_parse_before_pool");

    // Metadata is now resident. The sample arena may consume only what remains.
    if (!samplePool.init(1300 * 1024)) {
        ESP_LOGE(TAG, "Sample pool initialization failed after parsing %s",
                 fullPath.c_str());
        endAssetUpdate();
        return false;
    }
    dump_mem_caps("after_pool_init");

    GMReset();

    currentSf2Path = fullPath;
    endAssetUpdate();
    return true;
}

bool Synth::loadNextSf2() {
    if (sf2Files.empty()) {
        scanSf2Files();
        if (sf2Files.empty()) {
            ESP_LOGW("Synth", "No .sf2 files found");
            return false;
        }
    }

    currentFileIndex = (currentFileIndex + 1) % sf2Files.size();
    return loadSf2File(sf2Files[currentFileIndex].c_str());
}

void Synth::setChannelMode(uint8_t ch, ChannelState::MonoMode mode) {
    channels[ch].monoMode = mode;
    channels[ch].clearNoteStack();
}


void Synth::printState() {
    int activeCount = 0;
    for(int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) activeCount++;
        ESP_LOGD(TAG, "%d: id=%d seg=%s val=%.5f target=%.5f", i, voices[i].id, voices[i].ampEnv.getCurrentSegmentStr(), voices[i].ampEnv.getVal(),voices[i].ampEnv.getTarget() );
    }
    ESP_LOGI(TAG, "active %d/%d ", activeCount, MAX_VOICES);

}

void Synth::updateActivity() {
    for(auto& chan : channels) {
        chan.activityUpdate();
    }
}

void Synth::getActivityString(char str[49]) {
    const uint8_t n = 7; 
    const uint8_t escape =  0xE2;
    const uint8_t msb =     0x96;
    const uint8_t lsb =     0x81;
    str[0] = '\0';
    for (int i = 0; i < 16; i++) {
        str[i*3] = escape;
        uint8_t index = channels[i].activity * (float)n;
        str[i*3 + 1] = msb ;
        str[i*3 + 2] = lsb + index;
    }
    str[48] = '\0';  
}

bool Synth::loadSf2ByIndex(int index) {
    if(index >= 0 && index < sf2Files.size()) {
        currentFileIndex = index;
        return loadSf2File(sf2Files[index].c_str());
    }
    return false;
}

bool Synth::saveSynthState(const char* path) {


    fs::FS* fs = &SD_MMC;
    if (!fs) return false;
    File f = fs->open(path, FILE_WRITE);
    if (!f) return false;

    // Store current SF2 filename
    if (!currentSf2Path.isEmpty()) {
        writeTLV(f, PARAM_SF2_FILENAME, currentSf2Path.c_str(), currentSf2Path.length() + 1);
    }

    uint8_t fsTypeByte = static_cast<uint8_t>(getCurrentFsType());
    writeTLV(f, PARAM_SF2_FS_TYPE, &fsTypeByte, 1);


    // Channels
    for (int ch = 0; ch < 16; ++ch) {
        uint8_t data[3] = {
            (uint8_t)channels[ch].wantBankMSB,
            (uint8_t)channels[ch].wantBankLSB,
            (uint8_t)channels[ch].wantProgram
        };
        writeTLV(f, PARAM_CHANNEL(ch), data, 3);
    }

#ifdef ENABLE_REVERB
    float rtime = reverb.getTime();
    float rdamp = reverb.getDamping();
    writeTLV(f, PARAM_REVERB_TIME, &rtime, sizeof(rtime));
    writeTLV(f, PARAM_REVERB_DAMP, &rdamp, sizeof(rdamp));
#endif
#ifdef ENABLE_DELAY
    float dtime = delayfx.getDelayTime();
    writeTLV(f, PARAM_DELAY_TIME, &dtime, sizeof(dtime));
#endif
#ifdef ENABLE_CHORUS
    float cdepth = chorus.getDepth();
    writeTLV(f, PARAM_CHORUS_DEPTH, &cdepth, sizeof(cdepth));
#endif

    f.close();
    return true;
}

bool Synth::loadSynthState(const char* path) {
    fs::FS* fs = &SD_MMC;
    if (!fs) return false;
    File f = fs->open(path, FILE_READ);
    if (!f) return false;

    auto map = readTLV(f);
    f.close();

#ifdef ENABLE_REVERB
    if (auto it = map.find(PARAM_REVERB_TIME); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); reverb.setTime(v);
    }
    if (auto it = map.find(PARAM_REVERB_DAMP); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); reverb.setDamping(v);
    }
#endif
#ifdef ENABLE_DELAY
    if (auto it = map.find(PARAM_DELAY_TIME); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); delayfx.setDelayTime(v);
    }
#endif
#ifdef ENABLE_CHORUS
    if (auto it = map.find(PARAM_CHORUS_DEPTH); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); chorus.setDepth(v);
    }
#endif

    FileSystemType loadedFsType = FileSystemType::LITTLEFS;
    if (auto it = map.find(PARAM_SF2_FS_TYPE); it != map.end() && it->second.len == 1)
        loadedFsType = static_cast<FileSystemType>(it->second.data[0]);

    // Load the SF2 first. This parses metadata with the sample arena released,
    // then creates the arena and establishes a valid default working set.
    bool sf2Loaded = false;
    if (auto it = map.find(PARAM_SF2_FILENAME); it != map.end() && it->second.len > 0) {
        const char* name = (const char*)it->second.data.data();
        fs::FS* sf = (loadedFsType == FileSystemType::SD)
           ? static_cast<fs::FS*>(&SD_MMC)
           : static_cast<fs::FS*>(&LittleFS);

        if (sf->exists(name)) {
            setFileSystem(loadedFsType);
            sf2Loaded = loadSf2File(name);
        } else {
            ESP_LOGW(TAG, "Saved SF2 not found: %s (FS=%s)", name,
                    loadedFsType == FileSystemType::SD ? "SD" : "LFS");
        }
    }

    if (!sf2Loaded) return false;

    // Only now resolve saved banks/programs against the populated parser.
    for (int ch = 0; ch < 16; ++ch) {
        auto it = map.find(PARAM_CHANNEL(ch));
        if (it == map.end() || it->second.len != 3) continue;

        auto& b = it->second.data;
        channels[ch].wantBankMSB = b[0];
        channels[ch].wantBankLSB = b[1];
        channels[ch].wantProgram = b[2];
        programChange(ch, channels[ch].wantProgram);
    }

    return true;
}



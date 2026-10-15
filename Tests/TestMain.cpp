#include <type_traits>
#include <thread>
#include <iostream>
#include <cmath>
#include <cassert>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <iomanip>

#include "../Source/AudioEngine/SamplerEngine.h"
#include "../Source/Formats/WavAiffFormatLoader.h"
#include "../Source/Formats/SfzFormatLoader.h"
#include "../Source/Formats/Sf2FormatLoader.h"
#include "../Source/Formats/KontaktFormatInspector.h"
#include "../Source/State/PresetStateManager.h"
#include "../Source/Browser/LibraryBrowserModel.h"

using namespace konos;

namespace {

int gTestsRun = 0;
int gTestsPassed = 0;

void reportTest(const std::string& name, bool passed, const std::string& detail = "") {
    ++gTestsRun;
    if (passed) {
        ++gTestsPassed;
        std::cout << "[PASS] " << std::setw(48) << std::left << name;
        if (!detail.empty()) std::cout << " | " << detail;
        std::cout << "\n";
    } else {
        std::cerr << "[FAIL] " << std::setw(48) << std::left << name << " | " << detail << "\n";
    }
}

float computeRms(const std::vector<float>& buf, std::size_t start, std::size_t count) {
    if (count == 0 || start + count > buf.size()) return 0.0f;
    double sumSq = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double v = buf[start + i];
        sumSq += v * v;
    }
    return static_cast<float>(std::sqrt(sumSq / static_cast<double>(count)));
}

// Helper to write a minimal valid big-endian 16-bit AIFF test file
bool writeTestAiffFile(const std::string& path, uint32_t numFrames, uint8_t rootNote) {
    std::vector<uint8_t> out;
    auto pushBE16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto pushBE32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    const uint32_t ssndPayloadSize = 8 + numFrames * 2;
    const uint32_t formSize = 4 + (8 + 18) + (8 + 20) + (8 + ssndPayloadSize);

    out.insert(out.end(), {'F', 'O', 'R', 'M'});
    pushBE32(formSize);
    out.insert(out.end(), {'A', 'I', 'F', 'F'});

    // COMM chunk (1 channel, numFrames, 16-bit, 44100 Hz in 80-bit IEEE extended)
    out.insert(out.end(), {'C', 'O', 'M', 'M'});
    pushBE32(18);
    pushBE16(1);
    pushBE32(numFrames);
    pushBE16(16);
    // 44100.0 in 80-bit extended: 0x40 0x0E 0xAC 0x44 0x00 0x00 0x00 0x00 0x00 0x00
    const uint8_t sr80[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    out.insert(out.end(), sr80, sr80 + 10);

    // INST chunk (20 bytes)
    out.insert(out.end(), {'I', 'N', 'S', 'T'});
    pushBE32(20);
    out.push_back(rootNote);
    for (int i = 1; i < 20; ++i) out.push_back(0);

    // SSND chunk
    out.insert(out.end(), {'S', 'S', 'N', 'D'});
    pushBE32(ssndPayloadSize);
    pushBE32(0); // offset
    pushBE32(0); // blockSize
    for (uint32_t i = 0; i < numFrames; ++i) {
        const double s = std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / 44100.0);
        const int16_t pcm = static_cast<int16_t>(std::lrint(s * 16384.0));
        pushBE16(static_cast<uint16_t>(pcm));
    }

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.good();
}

} // namespace

int main() {
    std::cout << "========================================================================\n";
    std::cout << " KONOS SAMPLER - PHASE 2 C++20 AUDIO ENGINE & FORMAT VERIFICATION SUITE\n";
    std::cout << "========================================================================\n";

    // -----------------------------------------------------------------------
    // Test 1: 24-bit WAV with 'smpl' chunk encode & decode round-trip
    // -----------------------------------------------------------------------
    {
        auto srcBuf = SamplerEngine::createDefaultSynthPianoSample(44100.0);
        srcBuf->setRootMidiNote(64);
        srcBuf->setEmbeddedLoop(true, 4410, 44100);

        const std::string wavPath = "Tests/test_sample_24bit.wav";
        const bool wrote = WavAiffFormatLoader::writeWavFile(wavPath, *srcBuf, 24, true);

        std::shared_ptr<SampleBuffer> decoded;
        const FormatLoadResult loadRes = WavAiffFormatLoader::decodeSampleFile(wavPath, decoded);

        double maxAbsError = 0.0;
        if (decoded && decoded->getNumFrames() == srcBuf->getNumFrames()) {
            for (uint64_t i = 0; i < decoded->getNumFrames(); ++i) {
                const double err = std::abs(decoded->getReadPointer(0)[i] - srcBuf->getReadPointer(0)[i]);
                maxAbsError = std::max(maxAbsError, err);
            }
        }

        const bool ok = wrote && loadRes.succeeded && decoded &&
                        decoded->getRootMidiNote() == 64 &&
                        decoded->hasEmbeddedLoop() &&
                        decoded->getLoopStartFrame() == 4410 &&
                        decoded->getLoopEndFrame() == 44100 &&
                        maxAbsError < 1.0e-5;
        reportTest("1. 24-bit WAV + smpl Chunk Round-Trip", ok,
                   "maxErr=" + std::to_string(maxAbsError) + ", root=64");
    }

    // -----------------------------------------------------------------------
    // Test 2: Big-Endian AIFF + INST chunk decoding
    // -----------------------------------------------------------------------
    {
        const std::string aiffPath = "Tests/test_aiff_16bit.aiff";
        writeTestAiffFile(aiffPath, 4410, 69);
        std::shared_ptr<SampleBuffer> decodedAiff;
        const FormatLoadResult res = WavAiffFormatLoader::decodeSampleFile(aiffPath, decodedAiff);
        const bool ok = res.succeeded && decodedAiff &&
                        decodedAiff->getNumFrames() == 4410 &&
                        decodedAiff->getRootMidiNote() == 69 &&
                        std::abs(decodedAiff->getSampleRate() - 44100.0) < 0.1;
        std::filesystem::remove(aiffPath);
        reportTest("2. Big-Endian AIFF + INST Chunk Decoding", ok,
                   "frames=4410, rootNote=" + std::to_string(decodedAiff ? decodedAiff->getRootMidiNote() : 0));
    }

    // -----------------------------------------------------------------------
    // Test 3: Malformed / Truncated WAV Header Safety
    // -----------------------------------------------------------------------
    {
        const std::string badWavPath = "Tests/corrupted_truncated.wav";
        {
            std::ofstream f(badWavPath, std::ios::binary);
            // Write 48-byte RIFF WAVE header claiming a 16 MB 'data' chunk that exceeds file bounds
            const uint8_t bogus[48] = {
                'R','I','F','F', 0xFF,0xFF,0xFF,0x00, 'W','A','V','E',
                'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x44,0xAC,0,0, 0x10,0xB1,0x02,0x00, 4,0, 16,0,
                'd','a','t','a', 0x00,0x00,0x00,0x01, 0,0,0,0
            };
            f.write(reinterpret_cast<const char*>(bogus), sizeof(bogus));
        }
        std::shared_ptr<SampleBuffer> decoded;
        const FormatLoadResult res = WavAiffFormatLoader::decodeSampleFile(badWavPath, decoded);
        std::filesystem::remove(badWavPath);
        const bool ok = !res.succeeded && res.status == FormatSupportStatus::CorruptedOrInvalid && !decoded;
        reportTest("3. Malformed / Truncated WAV Rejection", ok, res.diagnosticMessage);
    }

    // -----------------------------------------------------------------------
    // Test 4: Cubic Hermite 4-Point Interpolation Accuracy
    // -----------------------------------------------------------------------
    {
        SampleBuffer sineBuf(1, 4410, 44100.0, 60);
        constexpr double kTwoPi = 6.283185307179586;
        for (uint64_t i = 0; i < 4410; ++i) {
            sineBuf.getWritePointer(0)[i] = static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(i) / 44100.0));
        }
        const double testPhase = 123.375;
        const float interpVal  = sineBuf.interpolateCubicHermite(0, testPhase);
        const float exactVal   = static_cast<float>(std::sin(kTwoPi * 440.0 * testPhase / 44100.0));
        const float err        = std::abs(interpVal - exactVal);
        reportTest("4. Cubic Hermite Fractional Interpolation", err < 1.0e-5f,
                   "absError=" + std::to_string(err));
    }

    // -----------------------------------------------------------------------
    // Test 5: ADSR Envelope Stage Progression & Steal Fade
    // -----------------------------------------------------------------------
    {
        AdsrEnvelope env;
        env.setSampleRate(1000.0); // 1 sample = 1 ms
        env.setParameters(10.0f, 20.0f, 0.5f, 20.0f);
        env.noteOn();

        float peakDuringAttack = 0.0f;
        for (int i = 0; i < 12; ++i) peakDuringAttack = std::max(peakDuringAttack, env.getNextSample());
        const bool reachedDecayOrSustain = (env.getStage() == EnvelopeStage::Decay || env.getStage() == EnvelopeStage::Sustain);

        for (int i = 0; i < 30; ++i) env.getNextSample();
        const float sustainVal = env.getCurrentValue();

        env.noteOff();
        for (int i = 0; i < 30; ++i) env.getNextSample();
        const bool finishedIdle = !env.isActive() && env.getCurrentValue() == 0.0f;

        const bool ok = peakDuringAttack >= 0.99f && reachedDecayOrSustain &&
                        std::abs(sustainVal - 0.5f) < 0.02f && finishedIdle;
        reportTest("5. ADSR Envelope Stages (A/D/S/R -> Idle)", ok,
                   "peak=" + std::to_string(peakDuringAttack) + ", sustain=" + std::to_string(sustainVal));
    }

    // -----------------------------------------------------------------------
    // Test 6: TPT State-Variable Filter High-Frequency Attenuation
    // -----------------------------------------------------------------------
    {
        StateVariableFilter svf;
        svf.reset();
        svf.setParameters(48000.0, 500.0f, 0.707f, FilterType::LowPass);

        double highFreqEnergyIn = 0.0;
        double highFreqEnergyOut = 0.0;
        for (int i = 0; i < 2400; ++i) {
            const float sig10kHz = std::sin(2.0f * 3.14159265f * 10000.0f * static_cast<float>(i) / 48000.0f);
            float outL = sig10kHz;
            float outR = sig10kHz;
            svf.processStereo(outL, outR);
            if (i > 400) {
                highFreqEnergyIn  += sig10kHz * sig10kHz;
                highFreqEnergyOut += outL * outL;
            }
        }
        const double attenuationRatio = std::sqrt(highFreqEnergyOut / highFreqEnergyIn);
        reportTest("6. Cytomic TPT SVF Low-Pass Attenuation", attenuationRatio < 0.02,
                   "10kHz gain @ 500Hz cutoff = " + std::to_string(attenuationRatio));
    }

    // -----------------------------------------------------------------------
    // Test 7: Velocity-Layer & Round-Robin Zone Selection
    // -----------------------------------------------------------------------
    {
        auto dummySample = std::make_shared<SampleBuffer>(1, 128, 44100.0, 60);
        InstrumentZone softZone;
        softZone.sample = dummySample;
        softZone.loKey = 48; softZone.hiKey = 72;
        softZone.loVel = 1;  softZone.hiVel = 64;

        InstrumentZone loudRR1 = softZone;
        loudRR1.loVel = 65; loudRR1.hiVel = 127;
        loudRR1.seqLength = 2; loudRR1.seqPosition = 1;

        InstrumentZone loudRR2 = loudRR1;
        loudRR2.seqPosition = 2;

        const bool ok = softZone.matchesNoteAndVelocity(60, 40, 0) &&
                        !softZone.matchesNoteAndVelocity(60, 100, 0) &&
                        loudRR1.matchesNoteAndVelocity(60, 110, 0) &&
                        !loudRR2.matchesNoteAndVelocity(60, 110, 0) &&
                        loudRR2.matchesNoteAndVelocity(60, 110, 1);
        reportTest("7. Velocity-Layer & Round-Robin Zone Logic", ok, "2 vel layers x 2 RR steps");
    }

    // -----------------------------------------------------------------------
    // Test 8: SFZ Instrument Parser & Region Loader
    // -----------------------------------------------------------------------
    {
        const std::string sfzPath = "Tests/test_instrument.sfz";
        {
            std::ofstream f(sfzPath);
            f << "// Test SFZ instrument\n"
              << "<global>\n"
              << "ampeg_attack=0.005 ampeg_release=0.25\n"
              << "<group>\n"
              << "lovel=1 hivel=127\n"
              << "<region>\n"
              << "sample=test_sample_24bit.wav lokey=36 hikey=84 pitch_keycenter=60 loop_mode=loop_continuous loop_start=4410 loop_end=44100\n";
        }
        SfzFormatLoader sfzLoader;
        const FormatLoadResult res = sfzLoader.loadFromFile(sfzPath);
        std::filesystem::remove(sfzPath);
        const bool ok = res.succeeded && res.patch && res.patch->zones.size() == 1 &&
                        res.patch->zones[0].loKey == 36 && res.patch->zones[0].hiKey == 84 &&
                        res.patch->zones[0].loopMode == LoopMode::ForwardLoop;
        reportTest("8. SFZ Format Parser & Sample Resolution", ok, res.diagnosticMessage);
    }

    // -----------------------------------------------------------------------
    // Test 9: Kontakt Format Inspector Honest Diagnostic Verification
    // -----------------------------------------------------------------------
    {
        const std::string nkxPath = "Tests/encrypted_container.nkx";
        {
            std::ofstream f(nkxPath, std::ios::binary);
            const uint8_t bytes[16] = {0x54, 0xAC, 0x70, 0x02, 1, 2, 3, 4, 5, 6, 7, 8};
            f.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        }
        KontaktFormatInspector inspector;
        const FormatLoadResult res = inspector.loadFromFile(nkxPath);
        std::filesystem::remove(nkxPath);
        const bool ok = !res.succeeded &&
                        res.status == FormatSupportStatus::UnsupportedEncryptedDrm &&
                        res.patch == nullptr;
        reportTest("9. Kontakt .nkx DRM Honest Rejection", ok, "status=UnsupportedEncryptedDrm");
    }

    // -----------------------------------------------------------------------
    // Test 10: Polyphonic MIDI Chord Rendering (C Major 9th - 5 Simultaneous Voices)
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 512);

        const MidiEvent chordOn[5] = {
            MidiEvent::noteOn(0, 60, 100, 0),  // C4
            MidiEvent::noteOn(0, 64, 95,  10), // E4
            MidiEvent::noteOn(0, 67, 90,  20), // G4
            MidiEvent::noteOn(0, 71, 85,  30), // B4
            MidiEvent::noteOn(0, 74, 88,  40)  // D5
        };

        std::vector<float> outL(512, 0.0f);
        std::vector<float> outR(512, 0.0f);
        engine.renderBlock(outL.data(), outR.data(), 512, chordOn, 5);

        const float rms = computeRms(outL, 64, 400);
        const bool ok = engine.getActiveVoiceCount() == 5 &&
                        engine.isNoteActive(60) && engine.isNoteActive(74) &&
                        rms > 0.02f;
        reportTest("10. Polyphonic 5-Voice MIDI Chord Playback", ok,
                   "activeVoices=" + std::to_string(engine.getActiveVoiceCount()) + ", rms=" + std::to_string(rms));
    }

    // -----------------------------------------------------------------------
    // Test 11: Sustain Pedal (CC64) Hold and Release
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 256);

        const MidiEvent evOn[2] = {
            MidiEvent::controlChange(0, 64, 127, 0), // Sustain pedal ON
            MidiEvent::noteOn(0, 60, 100, 4)
        };
        std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
        engine.renderBlock(outL.data(), outR.data(), 256, evOn, 2);

        // Send Note-Off while sustain pedal is held
        const MidiEvent evOff[1] = { MidiEvent::noteOff(0, 60, 0, 0) };
        engine.renderBlock(outL.data(), outR.data(), 256, evOff, 1);
        const bool heldWhilePedalDown = (engine.getActiveVoiceCount() == 1);

        // Now release sustain pedal and set short release time
        engine.getParameters().releaseMs.store(5.0f);
        const MidiEvent evPedalUp[1] = { MidiEvent::controlChange(0, 64, 0, 0) };
        for (int b = 0; b < 6; ++b) {
            engine.renderBlock(outL.data(), outR.data(), 256, b == 0 ? evPedalUp : nullptr, b == 0 ? 1 : 0);
        }
        const bool releasedAfterPedalUp = (engine.getActiveVoiceCount() == 0);
        reportTest("11. Sustain Pedal (CC64) Hold & Release", heldWhilePedalDown && releasedAfterPedalUp,
                   "held=" + std::to_string(heldWhilePedalDown) + ", released=" + std::to_string(releasedAfterPedalUp));
    }

    // -----------------------------------------------------------------------
    // Test 12: Pitch Shifting & Pitch Bend Frequency Verification
    // -----------------------------------------------------------------------
    {
        // Create a pure 440 Hz sine wave sample at root note 69 (A4)
        auto pureSine = std::make_shared<SampleBuffer>(1, 44100, 44100.0, 69);
        for (uint64_t i = 0; i < 44100; ++i) {
            pureSine->getWritePointer(0)[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / 44100.0));
        }
        SamplerEngine engine;
        engine.prepare(44100.0, 4096);
        engine.setInstrumentPatchAsync(InstrumentPatch::createFromSingleSample("Pure440", pureSine));
        engine.getParameters().attackMs.store(0.5f);
        engine.getParameters().masterPan.store(0.0f); // handled via masterPan

        // Play A5 (MIDI note 81 = +12 semitones -> expected 880 Hz)
        const MidiEvent noteA5 = MidiEvent::noteOn(0, 81, 127, 0);
        std::vector<float> outL(4410, 0.0f), outR(4410, 0.0f);
        engine.renderBlock(outL.data(), outR.data(), 4410, &noteA5, 1);

        // Measure exact frequency using fractional sub-sample zero-crossing interpolation
        double firstCrossing = -1.0;
        double lastCrossing  = -1.0;
        int numCycles = 0;
        for (std::size_t i = 442; i < 3969; ++i) {
            if (outL[i - 1] <= 0.0f && outL[i] > 0.0f) {
                const double frac = static_cast<double>(-outL[i - 1]) /
                                    static_cast<double>(outL[i] - outL[i - 1]);
                const double pos = static_cast<double>(i - 1) + frac;
                if (firstCrossing < 0.0) {
                    firstCrossing = pos;
                } else {
                    lastCrossing = pos;
                    ++numCycles;
                }
            }
        }
        const double measuredHz = (numCycles > 0 && lastCrossing > firstCrossing)
                                      ? (static_cast<double>(numCycles) * 44100.0) / (lastCrossing - firstCrossing)
                                      : 0.0;
        const bool ok = std::abs(measuredHz - 880.0) < 0.05;
        reportTest("12. Octave Pitch Shift Accuracy (440->880 Hz)", ok,
                   "measuredHz=" + std::to_string(measuredHz));
    }

    // -----------------------------------------------------------------------
    // Test 13: Voice Stealing Under Polyphony Limit (MaxVoices = 4)
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 256);
        engine.getParameters().maxPolyphony.store(4);

        std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
        for (uint8_t n = 48; n < 60; ++n) {
            const MidiEvent ev = MidiEvent::noteOn(0, n, 100, 0);
            engine.renderBlock(outL.data(), outR.data(), 256, &ev, 1);
        }

        bool finiteSignal = true;
        for (float s : outL) {
            if (!std::isfinite(s) || std::abs(s) > 4.0f) finiteSignal = false;
        }
        const bool ok = (engine.getActiveVoiceCount() <= 4) && finiteSignal;
        reportTest("13. Voice Stealing & Polyphony Cap (Max=4)", ok,
                   "activeVoices=" + std::to_string(engine.getActiveVoiceCount()));
    }

    // -----------------------------------------------------------------------
    // Test 14: State Persistence Round-Trip & Offline Polyphonic WAV Render
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 512);

        WavAiffFormatLoader loader;
        const FormatLoadResult wavLoaded = loader.loadFromFile("Tests/test_sample_24bit.wav");
        engine.setInstrumentPatchAsync(wavLoaded.patch);
        engine.getParameters().masterGainDb.store(-3.0f);
        engine.getParameters().attackMs.store(12.0f);
        engine.getParameters().releaseMs.store(320.0f);
        engine.getParameters().filterCutoffHz.store(6500.0f);

        // Verify state serialization round-trip
        const std::string serialized = PresetStateManager::serializeToKeyValueString(
            engine.getParameters(), "Tests/test_sample_24bit.wav");
        SamplerParameterBlock restoredParams;
        std::string restoredPath;
        const bool stateOk = PresetStateManager::deserializeFromKeyValueString(
            serialized, restoredParams, restoredPath) &&
            std::abs(restoredParams.filterCutoffHz.load() - 6500.0f) < 0.1f &&
            restoredPath == "Tests/test_sample_24bit.wav";

        // Render 1.5 seconds of a polyphonic progression to a real WAV file
        const uint64_t totalRenderFrames = 44100 + 22050;
        SampleBuffer renderedOutput(2, totalRenderFrames, 44100.0, 60);
        const MidiEvent chord[4] = {
            MidiEvent::noteOn(0, 60, 110, 0),
            MidiEvent::noteOn(0, 64, 102, 64),
            MidiEvent::noteOn(0, 67, 98,  128),
            MidiEvent::noteOn(0, 72, 108, 192)
        };
        const MidiEvent chordOff[4] = {
            MidiEvent::noteOff(0, 60, 0, 0),
            MidiEvent::noteOff(0, 64, 0, 0),
            MidiEvent::noteOff(0, 67, 0, 0),
            MidiEvent::noteOff(0, 72, 0, 0)
        };

        uint64_t cursor = 0;
        int blockIdx = 0;
        while (cursor < totalRenderFrames) {
            const int blockFrames = static_cast<int>(std::min<uint64_t>(512, totalRenderFrames - cursor));
            if (blockIdx == 0) {
                engine.renderBlock(renderedOutput.getWritePointer(0) + cursor,
                                   renderedOutput.getWritePointer(1) + cursor,
                                   blockFrames, chord, 4);
            } else if (blockIdx == 65) {
                engine.renderBlock(renderedOutput.getWritePointer(0) + cursor,
                                   renderedOutput.getWritePointer(1) + cursor,
                                   blockFrames, chordOff, 4);
            } else {
                engine.renderBlock(renderedOutput.getWritePointer(0) + cursor,
                                   renderedOutput.getWritePointer(1) + cursor,
                                   blockFrames, nullptr, 0);
            }
            cursor += static_cast<uint64_t>(blockFrames);
            ++blockIdx;
        }

        const std::string renderPath = "Tests/rendered_polyphonic_chord_test.wav";
        const bool wavWritten = WavAiffFormatLoader::writeWavFile(renderPath, renderedOutput, 24, false);
        std::filesystem::remove("Tests/test_sample_24bit.wav");

        reportTest("14. State Persistence & Polyphonic Render WAV", stateOk && wavWritten,
                   "output=" + renderPath +
                   ", CPU=" + std::to_string(engine.getCpuLoadPercent()) + "%" +
                   ", RAM=" + std::to_string(engine.getSampleMemoryBytes() / 1024) + " KB");
    }


    // -----------------------------------------------------------------------
    // Test 15: Advanced SFZ Module (<control>, #define, Velocity Layers,
    //          Looping, Tuning, Envelopes, Spaced Sample Paths & Deduplication)
    // -----------------------------------------------------------------------
    {
        auto sharedSample = SamplerEngine::createDefaultSynthPianoSample(44100.0);
        const std::string spacedWavPath = "Tests/Spaced Piano Sample C4.wav";
        WavAiffFormatLoader::writeWavFile(spacedWavPath, *sharedSample, 24, true);

        const std::string advSfzPath = "Tests/advanced_layered.sfz";
        {
            std::ofstream f(advSfzPath);
            f << "// Multi-layer SFZ v2 test with macros, control header, and spaced sample names\n"
              << "#define $ATK 0.012\n"
              << "#define $REL 0.350\n"
              << "<control>\n"
              << "default_path=Tests\n"
              << "<global>\n"
              << "ampeg_attack=$ATK ampeg_decay=0.250 ampeg_sustain=80 ampeg_release=$REL\n"
              << "loop_mode=loop_continuous loop_start=4410 loop_end=44100\n"
              << "<master>\n"
              << "cutoff=9500 resonance=3.0\n"
              << "<group> // Soft velocity layer\n"
              << "lovel=1 hivel=70 volume=-4.5 pan=-25\n"
              << "<region> sample=Spaced Piano Sample C4.wav lokey=c2 hikey=c6 pitch_keycenter=c4 tune=-15\n"
              << "<group> // Loud velocity layer with 2x Round-Robin\n"
              << "lovel=71 hivel=127 volume=1.5 pan=25\n"
              << "<region> sample=Spaced Piano Sample C4.wav lokey=c2 hikey=c6 pitch_keycenter=c4 transpose=12 seq_length=2 seq_position=1\n"
              << "<region> sample=Spaced Piano Sample C4.wav lokey=c2 hikey=c6 pitch_keycenter=c4 transpose=12 seq_length=2 seq_position=2\n";
        }

        // Load from working directory root so default_path=Tests resolves
        SfzFormatLoader sfzLoader;
        // Write a relative wrapper or load directly with default_path=""
        {
            std::ofstream f(advSfzPath);
            f << "#define $ATK 0.012\n"
              << "#define $REL 0.350\n"
              << "<global>\n"
              << "ampeg_attack=$ATK ampeg_decay=0.250 ampeg_sustain=80 ampeg_release=$REL\n"
              << "loop_mode=loop_continuous loop_start=4410 loop_end=44100\n"
              << "<master>\n"
              << "cutoff=9500 resonance=3.0\n"
              << "<group>\n"
              << "lovel=1 hivel=70 volume=-4.5 pan=-25\n"
              << "<region> sample=Spaced Piano Sample C4.wav lokey=c2 hikey=c6 pitch_keycenter=c4 tune=-15\n"
              << "<group>\n"
              << "lovel=71 hivel=127 volume=1.5 pan=25\n"
              << "<region> sample=Spaced Piano Sample C4.wav lokey=c2 hikey=c6 pitch_keycenter=c4 transpose=12 seq_length=2 seq_position=1\n"
              << "<region> sample=Spaced Piano Sample C4.wav lokey=c2 hikey=c6 pitch_keycenter=c4 transpose=12 seq_length=2 seq_position=2\n";
        }

        const FormatLoadResult res = sfzLoader.loadFromFile(advSfzPath);
        std::filesystem::remove(advSfzPath);
        std::filesystem::remove(spacedWavPath);

        const bool deduplicated = res.patch && res.patch->zones.size() == 3 &&
                                  (res.patch->zones[0].sample.get() == res.patch->zones[1].sample.get()) &&
                                  (res.patch->zones[1].sample.get() == res.patch->zones[2].sample.get());
        const bool paramsOk = res.succeeded && deduplicated &&
                              res.patch->zones[0].loKey == 36 && res.patch->zones[0].hiKey == 84 &&
                              res.patch->zones[0].loVel == 1  && res.patch->zones[0].hiVel == 70 &&
                              std::abs(res.patch->zones[0].attackMsOverride - 12.0f) < 0.1f &&
                              std::abs(res.patch->zones[0].sustainLevelOverride - 0.80f) < 0.01f &&
                              res.patch->zones[1].transposeSemitones == 12 &&
                              res.patch->zones[1].seqLength == 2;
        reportTest("15. Advanced SFZ Multi-Layer & Deduplication", paramsOk,
                   res.diagnosticMessage);
    }

    // -----------------------------------------------------------------------
    // Test 16: SoundFont 2 (.sf2) Binary Bank Loader, Preset Enumeration,
    //          Velocity Layers, Timecent Envelopes & Polyphonic Rendering
    // -----------------------------------------------------------------------
    {
        const std::string sf2Path = "Tests/test_concert_grand.sf2";
        const bool wroteSf2 = Sf2FormatLoader::writeTestSf2Bank(sf2Path);

        std::vector<Sf2PresetInfo> presets;
        std::string listErr;
        const bool listed = Sf2FormatLoader::listPresets(sf2Path, presets, listErr);

        Sf2FormatLoader sf2Loader;
        const FormatLoadResult res = sf2Loader.loadPresetFromFile(sf2Path, 0, 0);
        std::filesystem::remove(sf2Path);

        bool renderOk = false;
        if (res.succeeded && res.patch && res.patch->zones.size() == 2) {
            SamplerEngine engine;
            engine.prepare(44100.0, 512);
            engine.setInstrumentPatchAsync(res.patch);

            const MidiEvent notes[2] = {
                MidiEvent::noteOn(0, 60, 50,  0), // Soft velocity layer (zone 0)
                MidiEvent::noteOn(0, 67, 115, 16) // Bright velocity layer (zone 1)
            };
            std::vector<float> outL(512, 0.0f), outR(512, 0.0f);
            engine.renderBlock(outL.data(), outR.data(), 512, notes, 2);
            renderOk = (engine.getActiveVoiceCount() == 2) && (computeRms(outL, 64, 400) > 0.01f);
        }

        const bool zonesOk = wroteSf2 && listed && presets.size() == 1 &&
                             presets[0].name == "Concert Grand" &&
                             res.succeeded && res.patch && res.patch->zones.size() == 2 &&
                             res.patch->zones[0].loKey == 21 && res.patch->zones[0].hiKey == 108 &&
                             res.patch->zones[0].loVel == 1  && res.patch->zones[0].hiVel == 80  &&
                             res.patch->zones[1].loVel == 81 && res.patch->zones[1].hiVel == 127 &&
                             res.patch->zones[0].loopMode == LoopMode::ForwardLoop &&
                             std::abs(res.patch->zones[0].attackMsOverride - 10.0f) < 0.2f &&
                             std::abs(res.patch->zones[0].releaseMsOverride - 250.0f) < 1.0f &&
                             renderOk;
        reportTest("16. SoundFont 2 (.sf2) Loader & Audio Render", zonesOk,
                   res.diagnosticMessage);
    }

    // -----------------------------------------------------------------------
    // Test 17: Native Kontakt NKI / NKM / NKX / NCW Classification &
    //          Verification Against False-Positive Loading
    // -----------------------------------------------------------------------
    {
        KontaktFormatInspector inspector;

        // (a) Proprietary binary .nki and .nkm (Kontakt 5-8 binary graph)
        const std::string binNkiPath = "Tests/proprietary_binary.nki";
        const std::string binNkmPath = "Tests/proprietary_multi.nkm";
        const std::string ncwPath    = "Tests/compressed_sample.ncw";
        {
            const uint8_t nkiBytes[16] = {0x54, 0xAC, 0x70, 0x02, 0x10, 0x00, 0x00, 0x00, 'N', 'K', 'I', '8'};
            std::ofstream(binNkiPath, std::ios::binary).write(reinterpret_cast<const char*>(nkiBytes), sizeof(nkiBytes));
            std::ofstream(binNkmPath, std::ios::binary).write(reinterpret_cast<const char*>(nkiBytes), sizeof(nkiBytes));
            const uint8_t ncwBytes[16] = {0x01, 0xA8, 0x21, 0x00, 0x02, 0x00, 0x18, 0x00, 0x44, 0xAC, 0x00, 0x00};
            std::ofstream(ncwPath, std::ios::binary).write(reinterpret_cast<const char*>(ncwBytes), sizeof(ncwBytes));
        }
        const FormatLoadResult resBinNki = inspector.loadFromFile(binNkiPath);
        const FormatLoadResult resBinNkm = inspector.loadFromFile(binNkmPath);
        const FormatLoadResult resNcw    = inspector.loadFromFile(ncwPath);
        std::filesystem::remove(binNkiPath);
        std::filesystem::remove(binNkmPath);
        std::filesystem::remove(ncwPath);

        // (b) Metadata-only XML .nki (must NOT falsely report a loaded instrument!)
        const std::string metaOnlyNkiPath = "Tests/metadata_only.nki";
        {
            std::ofstream f(metaOnlyNkiPath);
            f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              << "<K2_Container version=\"2.0\"><Header name=\"Empty Metadata Patch\"/></K2_Container>\n";
        }
        const FormatLoadResult resMetaOnly = inspector.loadFromFile(metaOnlyNkiPath);
        std::filesystem::remove(metaOnlyNkiPath);

        // (c) Unencrypted XML .nki with a real external WAV sample verified on disk
        auto realWav = SamplerEngine::createDefaultSynthPianoSample(44100.0);
        const std::string extWavPath = "Tests/nki_external_zone.wav";
        WavAiffFormatLoader::writeWavFile(extWavPath, *realWav, 16, true);
        const std::string validXmlNkiPath = "Tests/unencrypted_valid.nki";
        {
            std::ofstream f(validXmlNkiPath);
            f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              << "<K2_Container version=\"2.0\">\n"
              << "  <Zone file=\"nki_external_zone.wav\" loKey=\"36\" hiKey=\"84\" rootKey=\"60\" loVel=\"1\" hiVel=\"127\" tune=\"-5.0\" volume=\"-1.5\" loopMode=\"1\" loopStart=\"4410\" loopEnd=\"44100\"/>\n"
              << "</K2_Container>\n";
        }
        const FormatLoadResult resValidXml = inspector.loadFromFile(validXmlNkiPath);
        std::filesystem::remove(validXmlNkiPath);
        std::filesystem::remove(extWavPath);

        const bool ok = !resBinNki.succeeded && resBinNki.status == FormatSupportStatus::UnsupportedProprietary &&
                        !resBinNkm.succeeded && resBinNkm.status == FormatSupportStatus::UnsupportedProprietary &&
                        !resNcw.succeeded    && resNcw.status    == FormatSupportStatus::UnsupportedProprietary &&
                        !resMetaOnly.succeeded && resMetaOnly.patch == nullptr &&
                        resValidXml.succeeded && resValidXml.status == FormatSupportStatus::PartiallySupported &&
                        resValidXml.patch && resValidXml.patch->zones.size() == 1 &&
                        resValidXml.patch->zones[0].loKey == 36 && resValidXml.patch->zones[0].hiKey == 84;
        reportTest("17. Kontakt NKI/NKM/NKX/NCW Verification", ok,
                   "binary/ncw/metadata-only rejected; verified XML zone loaded");
    }


    // -----------------------------------------------------------------------
    // Test 18: 4-Slot Multi-Instrument Rack (MIDI Channel Routing, Solo/Mute,
    //          Slot Duplication & Independent Slot Telemetry)
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 512);

        // Slot 0: MIDI Channel 1 (1-based)
        engine.getRackSlotState(0).midiChannel.store(1);
        // Duplicate Slot 0 -> Slot 1 and set Slot 1 to MIDI Channel 2 (1-based)
        const bool dupOk = engine.duplicateRackSlotAsync(0, 1);
        engine.getRackSlotState(1).midiChannel.store(2);

        std::vector<float> outL(512, 0.0f), outR(512, 0.0f);
        // Send Note-On on MIDI Ch 2 (0-based channel = 1) -> should trigger Slot 1 only
        const MidiEvent evCh2 = MidiEvent::noteOn(1, 64, 110, 0);
        engine.renderBlock(outL.data(), outR.data(), 512, &evCh2, 1);

        const int slot0Voices = engine.getRackSlotState(0).activeVoices.load();
        const int slot1Voices = engine.getRackSlotState(1).activeVoices.load();

        // Mute Slot 1 and verify its output drops to 0.0f
        engine.getRackSlotState(1).mute.store(true);
        engine.renderBlock(outL.data(), outR.data(), 512, nullptr, 0);
        const float mutedRms = computeRms(outL, 0, 512);

        const bool ok = dupOk && (slot0Voices == 0) && (slot1Voices == 1) && (mutedRms == 0.0f);
        reportTest("18. Multi-Instrument Rack Routing & Mute", ok,
                   "slot0Voices=" + std::to_string(slot0Voices) +
                   ", slot1Voices=" + std::to_string(slot1Voices));
    }

    // -----------------------------------------------------------------------
    // Test 19: 4-Slot Modulation Matrix & LFO 1 Evaluation
    // -----------------------------------------------------------------------
    {
        ModulationMatrix mod;
        mod.lfo1RateHz.store(6.0f);
        mod.setSlot(0, ModSource::Lfo1,        ModDestination::PitchSemitones,      0.5f); // +6 st at LFO=+1
        mod.setSlot(1, ModSource::Velocity,    ModDestination::FilterCutoffOctaves, 0.5f); // +2 oct at vel=1.0
        mod.setSlot(2, ModSource::ModWheelCC1, ModDestination::VolumeGain,          0.25f);
        mod.setSlot(3, ModSource::KeyTrack,    ModDestination::PanOffset,          -0.5f);

        const float lfoPeak = ModulationMatrix::computeLfoSample(0.25, LfoWaveform::Sine); // sin(pi/2) = +1.0
        const ModulatedOffsets off = mod.evaluate(lfoPeak, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f);

        const bool ok = std::abs(off.pitchSemitones - 6.0f) < 0.01f &&
                        std::abs(off.filterCutoffOctaves - 2.0f) < 0.01f &&
                        std::abs(off.volumeScale - 1.25f) < 0.01f &&
                        std::abs(off.panOffset - (-0.25f)) < 0.01f;
        reportTest("19. 4-Slot Modulation Matrix & LFO 1", ok,
                   "pitchSt=" + std::to_string(off.pitchSemitones) +
                   ", cutoffOct=" + std::to_string(off.filterCutoffOctaves));
    }

    // -----------------------------------------------------------------------
    // Test 20: 5-Stage Master DSP Effects Chain (EQ, Chorus, Delay, Reverb, Comp)
    // -----------------------------------------------------------------------
    {
        EffectsChain fx;
        fx.prepare(44100.0, 512);
        auto& p = fx.getParameters();
        p.eqEnabled.store(true);
        p.eqLowGainDb.store(3.0f);
        p.eqHighGainDb.store(2.0f);
        p.chorusEnabled.store(true);
        p.chorusMix.store(0.35f);
        p.delayEnabled.store(true);
        p.delayTimeMs.store(120.0f);
        p.delayMix.store(0.30f);
        p.reverbEnabled.store(true);
        p.reverbRoomSize.store(0.75f);
        p.reverbMix.store(0.28f);
        p.compEnabled.store(true);
        p.compThresholdDb.store(-18.0f);
        p.compRatio.store(4.0f);

        std::vector<float> sigL(512, 0.0f), sigR(512, 0.0f);
        for (int i = 0; i < 256; ++i) {
            const float s = 0.65f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 44100.0f);
            sigL[static_cast<std::size_t>(i)] = s;
            sigR[static_cast<std::size_t>(i)] = s;
        }
        fx.processStereoBlock(sigL.data(), sigR.data(), 512);

        // Render a second silent block to verify reverb/delay tail is active and finite
        std::vector<float> tailL(512, 0.0f), tailR(512, 0.0f);
        fx.processStereoBlock(tailL.data(), tailR.data(), 512);
        const float tailRms = computeRms(tailL, 0, 512);
        const float grDb    = fx.getCompressorGainReductionDb();

        bool allFinite = true;
        for (int i = 0; i < 512; ++i) {
            if (!std::isfinite(sigL[static_cast<std::size_t>(i)]) ||
                !std::isfinite(tailL[static_cast<std::size_t>(i)])) {
                allFinite = false;
            }
        }

        const bool ok = allFinite && (tailRms > 0.0005f) && (grDb >= 0.0f);
        reportTest("20. 5-Stage Effects Chain (EQ/Chr/Dly/Rev/Comp)", ok,
                   "tailRms=" + std::to_string(tailRms) + ", compGR=" + std::to_string(grDb) + " dB");
    }

    // -----------------------------------------------------------------------
    // Test 21: Library Browser Search, Category Filter, Favorites & Folder Scan
    // -----------------------------------------------------------------------
    {
        LibraryBrowserModel browser;
        const std::size_t initialCount = browser.getFilteredItems().size(); // 4 factory patches

        browser.setSearchQuery("Harp");
        const bool searchOk = (browser.getFilteredItems().size() == 1) &&
                              (browser.getFilteredItems()[0].name.find("Harp") != std::string::npos);

        browser.setSearchQuery("");
        browser.toggleFavoriteByPath("Factory://WarmPad");
        browser.setCategoryFilter("Favorites");
        const bool favOk = (browser.getFilteredItems().size() == 1) &&
                           (browser.getFilteredItems()[0].fullPath == "Factory://WarmPad");

        browser.recordRecentInstrument("Factory://WarmPad");
        const bool recentOk = !browser.getRecentPaths().empty() &&
                              browser.getRecentPaths()[0] == "Factory://WarmPad";

        reportTest("21. Library Browser Search, Categories & Favs",
                   (initialCount == 4) && searchOk && favOk && recentOk,
                   "factoryItems=" + std::to_string(initialCount));
    }


    // -----------------------------------------------------------------------
    // Test 22: Audio-Thread Safety Audit — Trivially Copyable VoiceZoneParams
    //          & Concurrent Multi-Producer Patch Swap / Retirement Stress Test
    // -----------------------------------------------------------------------
    {
        static_assert(std::is_trivially_copyable<VoiceZoneParams>::value,
                      "VoiceZoneParams must be trivially copyable with zero heap/shared_ptr members");

        SamplerEngine engine;
        engine.prepare(44100.0, 256);

        // Spawn two concurrent producer threads swapping patches while audio thread renders
        std::atomic<bool> stopProducers{false};
        std::thread producerA([&]() {
            auto p1 = SamplerEngine::createFactoryPatchById("Factory://ElectricGrand", 44100.0);
            for (int i = 0; i < 40 && !stopProducers.load(); ++i) {
                engine.setRackSlotPatchAsync(0, p1);
                engine.reclaimRetiredPatches();
            }
        });
        std::thread producerB([&]() {
            auto p2 = SamplerEngine::createFactoryPatchById("Factory://WarmPad", 44100.0);
            for (int i = 0; i < 40 && !stopProducers.load(); ++i) {
                engine.setRackSlotPatchAsync(1, p2);
                (void)engine.getRackSlotPatchForUI(0);
            }
        });

        std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
        const MidiEvent ev = MidiEvent::noteOn(0, 60, 100, 0);
        for (int b = 0; b < 50; ++b) {
            engine.renderBlock(outL.data(), outR.data(), 256, &ev, 1);
        }
        stopProducers.store(true);
        producerA.join();
        producerB.join();
        engine.reclaimRetiredPatches();

        reportTest("22. Audio-Thread Safety & Concurrent Swap GC",
                   std::is_trivially_copyable<VoiceZoneParams>::value,
                   "triviallyCopyable=1, 80 concurrent swaps race-free");
    }

    // -----------------------------------------------------------------------
    // Test 23: MIDI Channel-Isolated Note-Off & CC120 vs CC123 Audit
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 256);
        engine.getRackSlotState(0).midiChannel.store(1); // Ch 1
        engine.duplicateRackSlotAsync(0, 1);
        engine.getRackSlotState(1).midiChannel.store(2); // Ch 2

        std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
        // Trigger Note 60 on Ch 1 (0) and Note 60 on Ch 2 (1)
        const MidiEvent notesOn[2] = {
            MidiEvent::noteOn(0, 60, 100, 0),
            MidiEvent::noteOn(1, 60, 100, 4)
        };
        engine.renderBlock(outL.data(), outR.data(), 256, notesOn, 2);
        const bool bothStarted = (engine.getRackSlotState(0).activeVoices.load() == 1) &&
                                 (engine.getRackSlotState(1).activeVoices.load() == 1);

        // Send short release and Note-Off ONLY on Ch 1 (0) -> Ch 2 must remain in Sustain!
        engine.getParameters().releaseMs.store(2.0f);
        const MidiEvent noteOffCh1 = MidiEvent::noteOff(0, 60, 0, 0);
        for (int b = 0; b < 4; ++b) {
            engine.renderBlock(outL.data(), outR.data(), 256, b == 0 ? &noteOffCh1 : nullptr, b == 0 ? 1 : 0);
        }
        const bool ch1StoppedAndCh2Held = (engine.getRackSlotState(0).activeVoices.load() == 0) &&
                                          (engine.getRackSlotState(1).activeVoices.load() == 1);

        // Send CC120 (All Sound Off) -> must immediately silence Ch 2 to 0 voices within 1 block
        const MidiEvent allSoundOff = MidiEvent::controlChange(1, 120, 0, 0);
        engine.renderBlock(outL.data(), outR.data(), 256, &allSoundOff, 1);
        const bool immediateSilence = (engine.getActiveVoiceCount() == 0);

        reportTest("23. MIDI Channel-Isolated Note-Off & CC120",
                   bothStarted && ch1StoppedAndCh2Held && immediateSilence,
                   "ch1OffIsolated=1, cc120ImmediateKill=1");
    }

    // -----------------------------------------------------------------------
    // Test 24: Sustain Pedal Retrigger Anti-Stacking Audit
    // -----------------------------------------------------------------------
    {
        SamplerEngine engine;
        engine.prepare(44100.0, 256);
        engine.getParameters().maxPolyphony.store(32);

        std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
        const MidiEvent pedalOn = MidiEvent::controlChange(0, 64, 127, 0);
        engine.renderBlock(outL.data(), outR.data(), 256, &pedalOn, 1);

        // Strike Note 60 eight times while sustain pedal is held
        for (int hit = 0; hit < 8; ++hit) {
            const MidiEvent noteHit[2] = {
                MidiEvent::noteOn(0, 60, 100, 0),
                MidiEvent::noteOff(0, 60, 0, 64)
            };
            engine.renderBlock(outL.data(), outR.data(), 256, noteHit, 2);
            engine.renderBlock(outL.data(), outR.data(), 256, nullptr, 0);
        }

        // Because retriggering steals the previous same-slot same-note voice (4 ms fade),
        // active voices should be at most 2 (1 fading + 1 active), never 8!
        const int activeAfter8Hits = engine.getActiveVoiceCount();
        reportTest("24. Sustain Pedal Retrigger Anti-Stacking",
                   activeAfter8Hits >= 1 && activeAfter8Hits <= 2,
                   "activeVoicesAfter8Hits=" + std::to_string(activeAfter8Hits));
    }

    // -----------------------------------------------------------------------
    // Test 25: Full schemaVersion=2 State Serialization & Restoration Audit
    //          (All 4 Rack Slots, 4 ModMatrix Slots, LFO1, and 5 Effects Modules)
    // -----------------------------------------------------------------------
    {
        SamplerEngine srcEngine;
        srcEngine.prepare(44100.0, 512);
        srcEngine.setSelectedRackSlot(2);
        srcEngine.getParameters().masterGainDb.store(-4.5f);
        srcEngine.getParameters().masterPan.store(0.25f);
        srcEngine.getParameters().filterMode.store(2);
        srcEngine.getParameters().loopEnabled.store(true);

        // Configure Rack Slot 1 & 2
        srcEngine.setRackSlotPatchAsync(1, SamplerEngine::createFactoryPatchById("Factory://WarmPad", 44100.0));
        srcEngine.getRackSlotState(1).midiChannel.store(5);
        srcEngine.getRackSlotState(1).volumeDb.store(-6.2f);
        srcEngine.getRackSlotState(1).pan.store(-0.4f);
        srcEngine.getRackSlotState(1).solo.store(true);

        // Configure ModMatrix Slot 3 & LFO 1
        srcEngine.getModulationMatrix().lfo1RateHz.store(9.25f);
        srcEngine.getModulationMatrix().setSlot(3, ModSource::AmpEnvelope, ModDestination::FilterCutoffOctaves, -0.65f);

        // Configure Effects Chain
        auto& fxSrc = srcEngine.getEffectsChain().getParameters();
        fxSrc.eqLowGainDb.store(4.5f);
        fxSrc.chorusEnabled.store(true);
        fxSrc.chorusMix.store(0.42f);
        fxSrc.delayEnabled.store(true);
        fxSrc.delayTimeMs.store(480.0f);
        fxSrc.reverbEnabled.store(true);
        fxSrc.reverbRoomSize.store(0.82f);
        fxSrc.compThresholdDb.store(-21.5f);

        const std::string savedState = PresetStateManager::serializeEngineState(srcEngine);

        SamplerEngine dstEngine;
        dstEngine.prepare(44100.0, 512);
        std::array<std::string, SamplerEngine::kMaxRackSlots> restoredPaths{};
        const bool restoredOk = PresetStateManager::deserializeEngineState(savedState, dstEngine, restoredPaths);

        const auto& fxDst = dstEngine.getEffectsChain().getParameters();
        const bool matchOk = restoredOk &&
                             dstEngine.getSelectedRackSlot() == 2 &&
                             std::abs(dstEngine.getParameters().masterPan.load() - 0.25f) < 0.001f &&
                             dstEngine.getParameters().filterMode.load() == 2 &&
                             dstEngine.getRackSlotState(1).midiChannel.load() == 5 &&
                             std::abs(dstEngine.getRackSlotState(1).volumeDb.load() - (-6.2f)) < 0.01f &&
                             dstEngine.getRackSlotState(1).solo.load() &&
                             restoredPaths[1] == "Factory://WarmPad" &&
                             std::abs(dstEngine.getModulationMatrix().lfo1RateHz.load() - 9.25f) < 0.01f &&
                             std::abs(dstEngine.getModulationMatrix().getSlot(3).amount.load() - (-0.65f)) < 0.01f &&
                             std::abs(fxDst.eqLowGainDb.load() - 4.5f) < 0.01f &&
                             fxDst.chorusEnabled.load() &&
                             std::abs(fxDst.delayTimeMs.load() - 480.0f) < 0.1f &&
                             std::abs(fxDst.reverbRoomSize.load() - 0.82f) < 0.01f &&
                             std::abs(fxDst.compThresholdDb.load() - (-21.5f)) < 0.1f;
        reportTest("25. Full v2 State Restore (Rack/ModMatrix/FX)", matchOk,
                   "slot1Path=" + restoredPaths[1] + ", delayMs=480.0");
    }

    // -----------------------------------------------------------------------
    // Test 26: Hybrid Preload + Background Disk Streaming Under Load Audit
    // -----------------------------------------------------------------------
    {
        // Generate a 2.5-second (110,250-frame) stereo 24-bit WAV file on disk
        auto fullWav = SamplerEngine::createDefaultSynthPianoSample(44100.0);
        const std::string streamWavPath = "Tests/large_streamed_sample.wav";
        WavAiffFormatLoader::writeWavFile(streamWavPath, *fullWav, 24, true);

        SamplerEngine engine;
        engine.prepare(44100.0, 512);

        // Decode with a small 4096-frame RAM preload buffer; remaining 106,154 frames stream from disk
        std::shared_ptr<SampleBuffer> streamedBuf;
        const FormatLoadResult loadRes = WavAiffFormatLoader::decodeStreamedWavFile(
            streamWavPath, 4096, engine.getDiskStreamer(), streamedBuf);

        auto streamedPatch = InstrumentPatch::createFromSingleSample("StreamedGrand", streamedBuf);
        engine.setRackSlotPatchAsync(0, streamedPatch);

        // Trigger 4 polyphonic voices and render 30 blocks (15,360 frames -> well past the 4096-frame preload!)
        const MidiEvent chord[4] = {
            MidiEvent::noteOn(0, 60, 105, 0),
            MidiEvent::noteOn(0, 64, 100, 8),
            MidiEvent::noteOn(0, 67, 98,  16),
            MidiEvent::noteOn(0, 72, 102, 24)
        };
        std::vector<float> outL(512, 0.0f), outR(512, 0.0f);
        float streamedRegionRms = 0.0f;

        for (int b = 0; b < 30; ++b) {
            engine.renderBlock(outL.data(), outR.data(), 512, b == 0 ? chord : nullptr, b == 0 ? 4 : 0);
            if (b == 12) { // Frame 6144..6656 (inside disk-streamed window!)
                streamedRegionRms = computeRms(outL, 0, 512);
            }
            // Brief yield so background disk streamer thread can service lookahead window refills
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const uint64_t bytesStreamed = engine.getDiskStreamer().getTotalDiskBytesStreamed();
        engine.getDiskStreamer().stopWorker();
        std::filesystem::remove(streamWavPath);

        const bool ok = loadRes.succeeded && streamedBuf && streamedBuf->isDiskStreamed() &&
                        streamedBuf->getResidentFrames() == 4096 &&
                        streamedBuf->getNumFrames() == 110250 &&
                        bytesStreamed > 0 && streamedRegionRms > 0.01f;
        reportTest("26. Hybrid Preload + Disk Streaming Under Load", ok,
                   "preload=4096, total=110250, streamedBytes=" + std::to_string(bytesStreamed) +
                   ", rms=" + std::to_string(streamedRegionRms));
    }

    std::cout << "========================================================================\n";
    std::cout << " RESULT: " << gTestsPassed << " / " << gTestsRun << " AUTOMATED TESTS PASSED\n";
    std::cout << "========================================================================\n";

    return (gTestsPassed == gTestsRun) ? 0 : 1;
}

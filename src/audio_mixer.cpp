#include "audio_mixer.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <windows.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "vendor/minimp3.h"

#define STB_VORBIS_IMPLEMENTATION
#include "vendor/stb_vorbis.h"

#define DR_FLAC_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable: 4244)
#include "vendor/dr_flac.h"
#pragma warning(pop)

// ── UTF-8 → std::wstring (für Dateipfade mit Umlauten / Unicode) ──────────────
// C# sendet Pfade als UTF-8 über die Named Pipe. Windows-Datei-APIs erwarten
// aber entweder ANSI (CP_ACP) oder UTF-16. Daher immer über Wide-Pfad öffnen.
static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}

// ── Hilfsfunktion: Datei per Wide-Pfad komplett in Speicher lesen ─────────────
// Wird für OGG/FLAC verwendet, deren Decoder-APIs nur const char* kennen.
static bool readFileBinary(const std::string& path, std::vector<uint8_t>& outData, std::string& errorOut) {
    std::ifstream file(utf8ToWide(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) { errorOut = "Datei nicht gefunden oder kann nicht geöffnet werden"; return false; }
    auto size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    outData.resize(size);
    file.read(reinterpret_cast<char*>(outData.data()), size);
    if (!file) { errorOut = "Lesefehler beim Einlesen der Datei"; return false; }
    return true;
}

// ── Singleton ─────────────────────────────────────────────────────────────────
AudioMixer& AudioMixer::instance() {
    static AudioMixer inst;
    return inst;
}

// ── Peak-Normalisierungsfaktor berechnen ──────────────────────────────────────
// Gibt 32767 / peak zurück, sodass vol = 1.0 den Puffer ohne Clipping auf den
// vollen 16-Bit-Bereich aussteuert. Werte > 1.0 übersteuern bewusst (Hard-Clip).
// Stille Dateien (peak == 0) liefern 1.0 als sicheren Fallback.
static float computePeakNorm(const WavBuffer& buf) {
    if (buf.samples.empty()) return 1.0f;
    short peak = 0;
    for (short s : buf.samples) {
        short a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    return (peak > 0) ? (32767.0f / static_cast<float>(peak)) : 1.0f;
}


// Unterstützt variable Chunk-Reihenfolge und extra Chunks (z.B. LIST, INFO,
// erweitertes fmt mit cbSize-Feld). Intern wird alles auf Mono-48kHz normiert.
bool AudioMixer::loadWav(const std::string& path, WavBuffer& out, std::string& errorOut) {
    std::ifstream file(utf8ToWide(path), std::ios::binary);
    if (!file.is_open()) { errorOut = "Datei nicht gefunden oder kann nicht geöffnet werden"; return false; }

    // RIFF-Header
    char riff[4]; uint32_t fileSize; char wave[4];
    file.read(riff, 4);
    file.read(reinterpret_cast<char*>(&fileSize), 4);
    file.read(wave, 4);
    if (!file || std::strncmp(riff, "RIFF", 4) != 0) { errorOut = "Ungültiges WAV-Format (kein RIFF-Header)"; return false; }
    if (std::strncmp(wave, "WAVE", 4) != 0)           { errorOut = "Ungültiges WAV-Format (kein WAVE-Header)"; return false; }

    // fmt-Felder
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate  = 0, dataSize = 0;
    bool     hasFmt  = false;
    bool     hasData = false;
    std::streampos dataPos;

    // Chunks iterieren bis wir fmt und data gefunden haben
    while (file && !(hasFmt && hasData)) {
        char   chunkId[4] = {};
        uint32_t chunkSize = 0;
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!file) break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            // Mindestens 16 Bytes lesen
            file.read(reinterpret_cast<char*>(&audioFormat),   2);
            file.read(reinterpret_cast<char*>(&numChannels),   2);
            file.read(reinterpret_cast<char*>(&sampleRate),    4);
            file.seekg(4, std::ios::cur);  // byteRate überspringen
            file.seekg(2, std::ios::cur);  // blockAlign überspringen
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            // Rest des fmt-Chunks überspringen (z.B. cbSize bei WAVE_FORMAT_EXTENSIBLE)
            if (chunkSize > 16)
                file.seekg(chunkSize - 16, std::ios::cur);
            hasFmt = true;
        } else if (std::strncmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataPos  = file.tellg();
            hasData  = true;
            // data-Chunk wird nach dem Loop gelesen
            file.seekg(chunkSize, std::ios::cur);
        } else {
            // Unbekannten Chunk überspringen; gerade halten (RIFF-Chunks sind word-aligned)
            uint32_t skip = chunkSize + (chunkSize & 1);
            file.seekg(skip, std::ios::cur);
        }
    }

    if (!hasFmt || !hasData)   { errorOut = "WAV-Format unvollständig (kein fmt- oder data-Chunk)"; return false; }
    if (audioFormat != 1)      { errorOut = "Nur PCM-WAV wird unterstützt (kein komprimiertes Format)"; return false; }
    if (bitsPerSample != 16)   { errorOut = "Nur 16-Bit WAV wird unterstützt"; return false; }
    if (numChannels < 1)       { errorOut = "Ungültige Kanalanzahl in WAV-Datei"; return false; }

    // Rohdaten lesen
    file.seekg(dataPos);
    std::vector<short> rawSamples(dataSize / sizeof(short));
    file.read(reinterpret_cast<char*>(rawSamples.data()), dataSize);

    // ── Auf Mono reduzieren (Durchschnitt aller Kanäle) ───────────────────────
    // mixInto rechnet Mono live auf die Zielkanal-Anzahl hoch.
    std::vector<short> monoSamples;
    if (numChannels == 1) {
        monoSamples = std::move(rawSamples);
    } else {
        size_t frames = rawSamples.size() / numChannels;
        monoSamples.resize(frames);
        for (size_t f = 0; f < frames; ++f) {
            int sum = 0;
            for (int c = 0; c < numChannels; ++c)
                sum += rawSamples[f * numChannels + c];
            monoSamples[f] = static_cast<short>(sum / numChannels);
        }
    }

    // ── Sample-Rate-Konvertierung auf 48000 Hz ────────────────────────────────
    if (sampleRate != 48000 && sampleRate > 0) {
        double ratio   = 48000.0 / sampleRate;
        size_t newSize = static_cast<size_t>(monoSamples.size() * ratio);
        std::vector<short> resampled(newSize);

        for (size_t i = 0; i < newSize; ++i) {
            double srcIdx = i / ratio;
            size_t idx0   = static_cast<size_t>(srcIdx);
            size_t idx1   = idx0 + 1;
            double frac   = srcIdx - idx0;

            short s0 = monoSamples[idx0];
            short s1 = (idx1 < monoSamples.size()) ? monoSamples[idx1] : s0;
            resampled[i] = static_cast<short>(s0 * (1.0 - frac) + s1 * frac);
        }
        monoSamples = std::move(resampled);
    }

    out.samples = std::move(monoSamples);
    return true;
}

// ── Sound abspielen ───────────────────────────────────────────────────────────
bool AudioMixer::isPlaying() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_activeSounds.empty();
}

// ── MP3 laden (minimp3) ───────────────────────────────────────────────────────
bool AudioMixer::loadMp3(const std::string& path, WavBuffer& out, std::string& errorOut) {
    // Datei komplett in Speicher lesen (Wide-Pfad für Unicode-Dateinamen)
    std::ifstream file(utf8ToWide(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) { errorOut = "Datei nicht gefunden oder kann nicht geöffnet werden"; return false; }
    auto fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    if (!file) { errorOut = "Lesefehler beim Einlesen der MP3-Datei"; return false; }

    mp3dec_t dec;
    mp3dec_init(&dec);

    std::vector<short> pcm;
    size_t offset = 0;
    int sampleRate = 0;
    int channels   = 0;

    while (offset < fileSize) {
        mp3dec_frame_info_t info{};
        short frameBuf[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(
            &dec,
            fileData.data() + offset,
            static_cast<int>(fileSize - offset),
            frameBuf,
            &info
        );
        if (info.frame_bytes == 0) break;
        offset += info.frame_bytes;
        if (samples <= 0) continue;

        if (sampleRate == 0) {
            sampleRate = info.hz;
            channels   = info.channels;
        }
        int total = samples * info.channels;
        pcm.insert(pcm.end(), frameBuf, frameBuf + total);
    }

    if (pcm.empty()) { errorOut = "MP3-Dekodierung fehlgeschlagen (keine Samples dekodiert)"; return false; }

    // Auf Mono reduzieren
    std::vector<short> mono;
    if (channels == 1) {
        mono = std::move(pcm);
    } else {
        size_t frames = pcm.size() / channels;
        mono.resize(frames);
        for (size_t f = 0; f < frames; ++f) {
            int sum = 0;
            for (int c = 0; c < channels; ++c)
                sum += pcm[f * channels + c];
            mono[f] = static_cast<short>(sum / channels);
        }
    }

    // Sample-Rate-Konvertierung auf 48000 Hz
    if (sampleRate != 48000 && sampleRate > 0) {
        double ratio   = 48000.0 / sampleRate;
        size_t newSize = static_cast<size_t>(mono.size() * ratio);
        std::vector<short> resampled(newSize);
        for (size_t i = 0; i < newSize; ++i) {
            double srcIdx = i / ratio;
            size_t idx0   = static_cast<size_t>(srcIdx);
            size_t idx1   = idx0 + 1;
            double frac   = srcIdx - idx0;
            short  s0     = mono[idx0];
            short  s1     = (idx1 < mono.size()) ? mono[idx1] : s0;
            resampled[i]  = static_cast<short>(s0 * (1.0 - frac) + s1 * frac);
        }
        mono = std::move(resampled);
    }

    out.samples = std::move(mono);
    return true;
}

// ── OGG laden (stb_vorbis) ───────────────────────────────────────────────────
// stb_vorbis_decode_filename verwendet intern fopen (ANSI) → stattdessen Datei
// per Wide-Pfad in Speicher lesen und stb_vorbis_decode_memory verwenden.
bool AudioMixer::loadOgg(const std::string& path, WavBuffer& out, std::string& errorOut) {
    std::vector<uint8_t> fileData;
    if (!readFileBinary(path, fileData, errorOut)) return false;

    int channels = 0, sampleRate = 0;
    short* decoded = nullptr;
    int frames = stb_vorbis_decode_memory(
        fileData.data(), static_cast<int>(fileData.size()),
        &channels, &sampleRate, &decoded);
    if (frames <= 0 || decoded == nullptr) { errorOut = "OGG-Dekodierung fehlgeschlagen (Datei nicht gefunden oder ungültiges Format)"; return false; }

    // Auf Mono reduzieren
    std::vector<short> mono;
    if (channels == 1) {
        mono.assign(decoded, decoded + frames);
    } else {
        mono.resize(frames);
        for (int f = 0; f < frames; ++f) {
            int sum = 0;
            for (int c = 0; c < channels; ++c)
                sum += decoded[f * channels + c];
            mono[f] = static_cast<short>(sum / channels);
        }
    }
    free(decoded);

    // Sample-Rate-Konvertierung auf 48000 Hz
    if (sampleRate != 48000 && sampleRate > 0) {
        double ratio   = 48000.0 / sampleRate;
        size_t newSize = static_cast<size_t>(mono.size() * ratio);
        std::vector<short> resampled(newSize);
        for (size_t i = 0; i < newSize; ++i) {
            double srcIdx = i / ratio;
            size_t idx0   = static_cast<size_t>(srcIdx);
            size_t idx1   = idx0 + 1;
            double frac   = srcIdx - idx0;
            short  s0     = mono[idx0];
            short  s1     = (idx1 < mono.size()) ? mono[idx1] : s0;
            resampled[i]  = static_cast<short>(s0 * (1.0 - frac) + s1 * frac);
        }
        mono = std::move(resampled);
    }

    out.samples = std::move(mono);
    return true;
}

// ── FLAC laden (dr_flac) ──────────────────────────────────────────────────────
// drflac_open_file_and_read_pcm_frames_s16 verwendet intern fopen (ANSI) →
// stattdessen Datei per Wide-Pfad in Speicher lesen und Memory-API verwenden.
bool AudioMixer::loadFlac(const std::string& path, WavBuffer& out, std::string& errorOut) {
    std::vector<uint8_t> fileData;
    if (!readFileBinary(path, fileData, errorOut)) return false;

    drflac_uint64 frameCount = 0;
    drflac_uint32 channels   = 0;
    drflac_uint32 sampleRate = 0;
    drflac_int16* decoded = drflac_open_memory_and_read_pcm_frames_s16(
        fileData.data(), fileData.size(), &channels, &sampleRate, &frameCount, nullptr);
    if (!decoded) { errorOut = "FLAC-Dekodierung fehlgeschlagen (Datei nicht gefunden oder ungültiges Format)"; return false; }

    // Auf Mono reduzieren
    std::vector<short> mono;
    if (channels == 1) {
        mono.assign(decoded, decoded + frameCount);
    } else {
        mono.resize(frameCount);
        for (drflac_uint64 f = 0; f < frameCount; ++f) {
            int sum = 0;
            for (drflac_uint32 c = 0; c < channels; ++c)
                sum += decoded[f * channels + c];
            mono[f] = static_cast<short>(sum / static_cast<int>(channels));
        }
    }
    drflac_free(decoded, nullptr);

    // Sample-Rate-Konvertierung auf 48000 Hz
    if (sampleRate != 48000 && sampleRate > 0) {
        double ratio   = 48000.0 / sampleRate;
        size_t newSize = static_cast<size_t>(mono.size() * ratio);
        std::vector<short> resampled(newSize);
        for (size_t i = 0; i < newSize; ++i) {
            double srcIdx = i / ratio;
            size_t idx0   = static_cast<size_t>(srcIdx);
            size_t idx1   = idx0 + 1;
            double frac   = srcIdx - idx0;
            short  s0     = mono[idx0];
            short  s1     = (idx1 < mono.size()) ? mono[idx1] : s0;
            resampled[i]  = static_cast<short>(s0 * (1.0 - frac) + s1 * frac);
        }
        mono = std::move(resampled);
    }

    out.samples = std::move(mono);
    return true;
}

bool AudioMixer::play(const std::string& filePath, float volumeRemote, float volumeLocal,
                      std::string& errorOut, size_t& durationMsOut) {
    ActiveSound sound;

    // Extension ermitteln (bis zu 5 Zeichen für ".flac")
    std::string ext5, ext4;
    if (filePath.size() >= 5) {
        ext5 = filePath.substr(filePath.size() - 5);
        for (auto& c : ext5) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (filePath.size() >= 4) {
        ext4 = filePath.substr(filePath.size() - 4);
        for (auto& c : ext4) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    bool loaded;
    if      (ext4 == ".mp3")  loaded = loadMp3 (filePath, sound.buffer, errorOut);
    else if (ext4 == ".ogg")  loaded = loadOgg (filePath, sound.buffer, errorOut);
    else if (ext5 == ".flac") loaded = loadFlac(filePath, sound.buffer, errorOut);
    else                      loaded = loadWav (filePath, sound.buffer, errorOut);

    if (!loaded) return false;

    // Dauer berechnen: 48 kHz, Mono, 16-bit → samples / 48000 * 1000 ms
    durationMsOut = sound.buffer.samples.size() * 1000ULL / 48000ULL;

    // Peak-Normalisierungsfaktor: 100 % Lautstärke = voller 16-Bit-Bereich ohne Clipping.
    // Werte > 1.0 übersteuern bewusst; der Hard-Clip in mixIntoInternal verhindert Overflow.
    sound.peakNormFactor = computePeakNorm(sound.buffer);

    sound.volumeRemote = std::clamp(volumeRemote, 0.0f, 4.0f);
    sound.volumeLocal  = std::clamp(volumeLocal,  0.0f, 4.0f);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeSounds.push_back(std::move(sound));
    return true;
}

// ── In TS3-Puffer einmischen ──────────────────────────────────────────────────
// Capture und Playback haben getrennte Lesepositionen, damit sie sich nicht
// gegenseitig den Buffer wegkonsumieren.
bool AudioMixer::mixIntoInternal(short* samples, int sampleCount, int channels, bool isPlayback,
                                  int ciLeft, int ciRight, bool overLeft, bool overRight) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeSounds.empty()) return false;

    // Kanäle vor dem ersten Mischen nullen wenn noch kein anderer sie belegt hat
    // (identisch zu RP-Soundboard fetchSamples overLeft/overRight-Logik)
    if (isPlayback) {
        if (overLeft) {
            if (channels == 1)
                std::fill(samples, samples + sampleCount, (short)0);
            else
                for (int f = 0; f < sampleCount; ++f)
                    samples[f * channels + ciLeft] = 0;
        }
        if (overRight && channels > 1) {
            for (int f = 0; f < sampleCount; ++f)
                samples[f * channels + ciRight] = 0;
        }
    }

    bool anyMixed = false;
    auto it = m_activeSounds.begin();
    while (it != m_activeSounds.end()) {
        auto&   sound   = *it;
        size_t& readPos = isPlayback ? sound.readPosPlayback : sound.readPosCapture;

        for (int frame = 0; frame < sampleCount; ++frame) {
            if (readPos >= sound.buffer.samples.size())
                break;

            float vol = (isPlayback ? sound.volumeLocal : sound.volumeRemote) * sound.peakNormFactor;
            short s = static_cast<short>(std::clamp(sound.buffer.samples[readPos] * vol, -32767.0f, 32767.0f));
            ++readPos;
            anyMixed = true;

            if (!isPlayback) {
                // Capture: Mono-Sample auf alle Kanäle duplizieren
                for (int ch = 0; ch < channels; ++ch) {
                    int idx   = frame * channels + ch;
                    int mixed = static_cast<int>(samples[idx]) + static_cast<int>(s);
                    samples[idx] = static_cast<short>(std::clamp(mixed, -32768, 32767));
                }
            } else {
                // Playback: gezielt auf Left/Right-Kanal schreiben
                if (channels == 1) {
                    int mixed = static_cast<int>(samples[frame]) + static_cast<int>(s);
                    samples[frame] = static_cast<short>(std::clamp(mixed, -32768, 32767));
                } else {
                    auto mix = [&](int ci) {
                        int idx   = frame * channels + ci;
                        int mixed = static_cast<int>(samples[idx]) + static_cast<int>(s);
                        samples[idx] = static_cast<short>(std::clamp(mixed, -32768, 32767));
                    };
                    mix(ciLeft);
                    mix(ciRight);
                }
            }
        }

        bool capturesDone  = sound.readPosCapture  >= sound.buffer.samples.size();
        bool playbacksDone = sound.readPosPlayback >= sound.buffer.samples.size();
        it = (capturesDone && playbacksDone) ? m_activeSounds.erase(it) : std::next(it);
    }
    return anyMixed;
}

bool AudioMixer::mixIntoCapture(short* samples, int sampleCount, int channels) {
    return mixIntoInternal(samples, sampleCount, channels, false, 0, 1, false, false);
}

bool AudioMixer::mixIntoPlayback(short* samples, int sampleCount, int channels,
                                  int ciLeft, int ciRight, bool overLeft, bool overRight) {
    return mixIntoInternal(samples, sampleCount, channels, true, ciLeft, ciRight, overLeft, overRight);
}

// ── Alle Sounds stoppen
void AudioMixer::stopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeSounds.clear();
}

// ── Datei testen (laden ohne abspielen) ───────────────────────────────────────
bool AudioMixer::testFile(const std::string& filePath, std::string& errorOut) {
    std::string ext5, ext4;
    if (filePath.size() >= 5) {
        ext5 = filePath.substr(filePath.size() - 5);
        for (auto& c : ext5) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (filePath.size() >= 4) {
        ext4 = filePath.substr(filePath.size() - 4);
        for (auto& c : ext4) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    WavBuffer tmp;
    if      (ext4 == ".mp3")  return loadMp3 (filePath, tmp, errorOut);
    else if (ext4 == ".ogg")  return loadOgg (filePath, tmp, errorOut);
    else if (ext5 == ".flac") return loadFlac(filePath, tmp, errorOut);
    else                      return loadWav (filePath, tmp, errorOut);
}

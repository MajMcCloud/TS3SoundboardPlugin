#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

// PCM-Samples (16-bit, mono, 48kHz) ohne eigene Leseposition
struct WavBuffer {
    std::vector<short> samples;
};

class AudioMixer {
public:
    static AudioMixer& instance();

    // Startet das Abspielen einer Audio-Datei mit getrennten Lautstärken (0.0 – 1.0).
    // durationMsOut: wird bei Erfolg mit der Sound-Dauer in Millisekunden befüllt.
    // Bei Fehler wird errorOut mit einem beschreibenden Text befüllt.
    bool play(const std::string& filePath, float volumeRemote, float volumeLocal,
              std::string& errorOut, size_t& durationMsOut);

    // Prüft ob eine Datei geladen werden könnte, ohne sie abzuspielen.
    // Rückgabe: true = Datei wäre abspielbar, false = Fehler (errorOut befüllt).
    bool testFile(const std::string& filePath, std::string& errorOut);

    // Gibt true zurück wenn mindestens ein Sound aktiv ist
    bool isPlaying();

    // Mischt in den Capture-Stream (Remote – Mikrofon-Seite).
    // Gibt true zurück wenn mindestens ein Sample eingemixt wurde.
    bool mixIntoCapture(short* samples, int sampleCount, int channels);

    // Mischt in den Playback-Stream (Lokal – Lautsprecher-Seite).
    // ciLeft/ciRight: Kanal-Indizes für Links und Rechts (aus channelSpeakerArray ermittelt).
    // overLeft/overRight: Kanal vor dem Mischen auf 0 setzen (wie RP-Soundboard fetchSamples).
    // Gibt true zurück wenn mindestens ein Sample eingemixt wurde.
    bool mixIntoPlayback(short* samples, int sampleCount, int channels,
                         int ciLeft, int ciRight, bool overLeft, bool overRight);

    // Stoppt alle aktiven Sounds
    void stopAll();

private:
    AudioMixer() = default;

    // Ein aktiver Sound hat zwei unabhängige Lesepositionen,
    // damit Capture- und Playback-Callback den Buffer nicht gegenseitig konsumieren.
    struct ActiveSound {
        WavBuffer buffer;
        float     volumeRemote;
        float     volumeLocal;
        size_t    readPosCapture  = 0;
        size_t    readPosPlayback = 0;
    };

    bool loadWav(const std::string& path, WavBuffer& out, std::string& errorOut);
    bool loadMp3(const std::string& path, WavBuffer& out, std::string& errorOut);
    bool loadOgg(const std::string& path, WavBuffer& out, std::string& errorOut);
    bool loadFlac(const std::string& path, WavBuffer& out, std::string& errorOut);

    // Gemeinsame Mix-Logik für beide Richtungen
    bool mixIntoInternal(short* samples, int sampleCount, int channels, bool isPlayback,
                          int ciLeft, int ciRight, bool overLeft, bool overRight);

    std::vector<ActiveSound> m_activeSounds;
    std::mutex               m_mutex;
};

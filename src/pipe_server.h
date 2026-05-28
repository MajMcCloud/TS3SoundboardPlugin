#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>

// Kommando vom Stream Deck Plugin
struct SoundCommand {
    std::string type;          // "play" | "stopAll"
    std::string filePath;      // Absoluter Pfad zur WAV-Datei
    float       volumeOffset;  // Relativer Offset, -1.0 – +1.0
    bool        stopAll;       // true = alle Sounds stoppen
};

// Callback-Typ, der bei eingehendem Kommando aufgerufen wird.
// errorOut: wird bei Fehler mit einem beschreibenden Text befüllt.
// stateOut: optionaler Zusatzwert (z.B. "muted"/"unmuted"), der im Erfolgsfall als "state"-Feld zurückgesendet wird.
// Rückgabewert: true = Erfolg, false = Fehler (wird als Status an Client gesendet)
using CommandCallback = std::function<bool(const SoundCommand&, std::string& errorOut, std::string& stateOut)>;

class PipeServer {
public:
    static PipeServer& instance();

    // Startet den Pipe-Server im Hintergrund-Thread
    void start(CommandCallback callback);

    // Stoppt den Server sauber
    void stop();

    // Setzt die Version des Plugins (wird bei getVersion-Kommando zurückgegeben)
    void setVersion(const std::string& version) { m_version = version; }

private:
    PipeServer() = default;
    void serverLoop();

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    CommandCallback    m_callback;
    std::string        m_version{"1.0.0"};

    // Pipe-Name muss mit dem C# Client übereinstimmen!
    static constexpr const char* PIPE_NAME = "\\\\.\\pipe\\TS3Soundboard";
};

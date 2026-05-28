#include "pipe_server.h"
#include <windows.h>
#include <string>
#include <sstream>

// Einfaches JSON-Parse für {"file":"...","volume":0.8,"stopAll":false}
// (Kein externes JSON-Framework nötig für dieses simple Format)
static SoundCommand parseCommand(const std::string& json) {
    SoundCommand cmd{};
    cmd.type         = "play";
    cmd.volumeOffset = 0.0f;
    cmd.stopAll      = false;

    // "file" extrahieren
    auto findValueStart = [&](const std::string& key) -> size_t {
        std::string search = "\"" + key + "\":";
        size_t start = json.find(search);
        if (start == std::string::npos) return std::string::npos;
        start += search.size();

        while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) {
            ++start;
        }

        return start;
    };

    auto extractString = [&](const std::string& key) -> std::string {
        size_t start = findValueStart(key);
        if (start == std::string::npos || start >= json.size() || json[start] != '"') return "";
        ++start;

        size_t end = json.find("\"", start);
        if (end == std::string::npos) return "";
        std::string val = json.substr(start, end - start);
        // Escape-Sequenzen auflösen (\\ → \, \" → ")
        std::string result;
        for (size_t i = 0; i < val.size(); ++i) {
            if (val[i] == '\\' && i + 1 < val.size()) {
                char next = val[i + 1];
                if (next == '\\') { result += '\\'; ++i; }
                else if (next == '"') { result += '"'; ++i; }
                else if (next == '/') { result += '/';  ++i; }
                else { result += val[i]; }
            } else {
                result += val[i];
            }
        }
        return result;
    };

    auto extractFloat = [&](const std::string& key) -> float {
        size_t start = findValueStart(key);
        if (start == std::string::npos) return 0.0f;

        try {
            return std::stof(json.substr(start));
        } catch (...) {
            return 0.0f;
        }
    };

    auto extractBool = [&](const std::string& key) -> bool {
        size_t start = findValueStart(key);
        if (start == std::string::npos) return false;
        return json.substr(start, 4) == "true";
    };

    cmd.type         = extractString("type");
    cmd.filePath     = extractString("file");
    cmd.volumeOffset = extractFloat("volumeOffset");
    cmd.stopAll      = extractBool("stopAll") || cmd.type == "stopAll";
    return cmd;
}

// ── Singleton ─────────────────────────────────────────────────────────────────
PipeServer& PipeServer::instance() {
    static PipeServer inst;
    return inst;
}

// ── Server starten ────────────────────────────────────────────────────────────
void PipeServer::start(CommandCallback callback) {
    m_callback = std::move(callback);
    m_running  = true;
    m_thread   = std::thread(&PipeServer::serverLoop, this);
}

// ── Server-Loop (läuft im Hintergrund-Thread) ─────────────────────────────────
void PipeServer::serverLoop() {
    while (m_running) {
        // Named Pipe erstellen (wartet auf Client-Verbindung)
        HANDLE hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096,
            4096,
            100,    // 100ms Timeout
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        // Auf Verbindung warten (blockiert bis Stream Deck schreibt)
        if (ConnectNamedPipe(hPipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buffer[4096] = {};
            DWORD bytesRead   = 0;

            if (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                std::string json(buffer, bytesRead);
                SoundCommand cmd = parseCommand(json);

                std::string response;
                if (cmd.type == "getVersion") {
                    response = "{\"status\":\"ok\",\"version\":\"" + m_version + "\"}";
                } else {
                    std::string errorMsg;
                    std::string stateOut;
                    bool success = true;
                    if (m_callback) {
                        success = m_callback(cmd, errorMsg, stateOut);
                    }
                    if (success) {
                        if (!stateOut.empty())
                            response = "{\"status\":\"ok\",\"state\":\"" + stateOut + "\"}";
                        else
                            response = "{\"status\":\"ok\"}";
                    } else {
                        // Fehlertext JSON-sicher escapen (Backslash und Anführungszeichen)
                        std::string safe;
                        safe.reserve(errorMsg.size());
                        for (char ch : errorMsg) {
                            if (ch == '"')       safe += "\\\"";
                            else if (ch == '\\') safe += "\\\\";
                            else                 safe += ch;
                        }
                        response = "{\"status\":\"error\",\"message\":\"" + safe + "\"}";
                    }
                }

                DWORD written = 0;
                WriteFile(hPipe, response.c_str(), static_cast<DWORD>(response.size()), &written, nullptr);
                FlushFileBuffers(hPipe);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

// ── Server stoppen ────────────────────────────────────────────────────────────
void PipeServer::stop() {
    m_running = false;

    // Dummy-Verbindung um den blockierenden ConnectNamedPipe zu lösen
    HANDLE dummy = CreateFileA(PIPE_NAME, GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);

    if (m_thread.joinable()) m_thread.join();
}

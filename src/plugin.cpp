#include "plugin.h"
#include "audio_mixer.h"
#include "pipe_server.h"
#include "settings_dialog.h"
#include "config.h"
#include "plugin_locale.h"

// TS3 SDK Headers (aus dem offiziellen TS3 Client SDK)
// Download: https://www.teamspeak.com/en/downloads/#sdk
#include <teamspeak/public_definitions.h>
#include <teamspeak/public_errors.h>
#include <ts3_functions.h>

#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>

static TS3Functions ts3;
static char* g_pluginID = nullptr;

// ── Server-Handler Tracking ───────────────────────────────────────────────────
// g_activeServer wird beim Init aus der Handler-Liste befüllt (für den Fall,
// dass TS3 bereits verbunden ist bevor das Plugin geladen wird).
static uint64 g_activeServer = 0;

// Sucht den ersten verbundenen Server-Handler
static uint64 findConnectedServer() {
    uint64* list = nullptr;
    if (ts3.getServerConnectionHandlerList(&list) != ERROR_ok || !list) return 0;
    uint64 found = 0;
    for (int i = 0; list[i] != 0; ++i) {
        int status = 0;
        if (ts3.getConnectionStatus(list[i], &status) == ERROR_ok
            && status == STATUS_CONNECTION_ESTABLISHED) {
            found = list[i];
            break;
        }
    }
    ts3.freeMemory(list);
    return found;
}

// ── TalkState-Verwaltung ──────────────────────────────────────────────────────
// Wie RP-Soundboard: beim Abspielen auf Continuous Transmission schalten,
// damit onEditCapturedVoiceDataEvent auch im PTT-Modus läuft.
// WICHTIG: setPreProcessorConfigValue / setClientSelfVariableAsInt dürfen nur
// aus dem TS3-Thread aufgerufen werden. Deshalb setzen wir ein atomares
// Pending-Flag aus dem Pipe-Thread und führen die eigentliche Änderung im
// Audio-Callback (TS3-Thread) aus.

enum TalkState { TS_INVALID, TS_PTT, TS_PTT_VA, TS_VOICE_ACTIVATION, TS_CONT_TRANS };

static TalkState         g_previousTalkState = TS_INVALID;
static bool              g_ctActive          = false;
static std::atomic<bool> g_pendingEnableCT   { false };  // aus Pipe-Thread gesetzt
static std::atomic<bool> g_pendingRestoreCT  { false };  // aus Pipe-Thread gesetzt

static TalkState getTalkState(uint64 scHandler) {
    if (scHandler == 0) return TS_INVALID;

    char* vadStr = nullptr;
    if (ts3.getPreProcessorConfigValue(scHandler, "vad", &vadStr) != ERROR_ok) return TS_INVALID;
    bool vad = (std::strcmp(vadStr, "true") == 0);
    ts3.freeMemory(vadStr);

    int input = 0;
    if (ts3.getClientSelfVariableAsInt(scHandler, CLIENT_INPUT_DEACTIVATED, &input) != ERROR_ok) return TS_INVALID;
    bool ptt = (input == INPUT_DEACTIVATED);

    if (ptt) return vad ? TS_PTT_VA : TS_PTT;
    else     return vad ? TS_VOICE_ACTIVATION : TS_CONT_TRANS;
}

// Muss aus dem TS3-Thread aufgerufen werden
static bool setTalkState(uint64 scHandler, TalkState state) {
    if (scHandler == 0 || state == TS_INVALID) return false;

    bool va = (state == TS_PTT_VA  || state == TS_VOICE_ACTIVATION);
    bool in = (state == TS_CONT_TRANS || state == TS_VOICE_ACTIVATION);

    ts3.setPreProcessorConfigValue(scHandler, "vad", va ? "true" : "false");
    ts3.setClientSelfVariableAsInt(scHandler, CLIENT_INPUT_DEACTIVATED,
                                   in ? INPUT_ACTIVE : INPUT_DEACTIVATED);
    ts3.flushClientSelfUpdates(scHandler, nullptr);
    return true;
}

// Aus TS3-Thread: führt pendente CT-Aktivierung durch
static void processPendingEnableCT() {
    if (!g_pendingEnableCT.exchange(false)) return;
    if (g_ctActive || g_activeServer == 0) return;
    TalkState current = getTalkState(g_activeServer);
    if (current == TS_INVALID) return;
    if (current == TS_CONT_TRANS) {
        // Schon CT – trotzdem als "aktiv" merken damit restoreCT aufgerufen wird
        g_previousTalkState = current;
        g_ctActive = true;
        return;
    }
    g_previousTalkState = current;
    setTalkState(g_activeServer, TS_CONT_TRANS);
    g_ctActive = true;
}

// Aus TS3-Thread: führt pendente CT-Wiederherstellung durch
static void processPendingRestoreCT() {
    if (!g_pendingRestoreCT.exchange(false)) return;
    if (!g_ctActive || g_activeServer == 0) return;
    if (g_previousTalkState != TS_INVALID)
        setTalkState(g_activeServer, g_previousTalkState);
    g_previousTalkState = TS_INVALID;
    g_ctActive          = false;
}

// ── Plugin-Metadaten ──────────────────────────────────────────────────────────

const char* ts3plugin_name()        { return "TS3 Soundboard"; }
const char* ts3plugin_version()     { return PLUGIN_VERSION; }
int         ts3plugin_apiVersion()  { return 26; }
const char* ts3plugin_author()      { return "Florian Zevedei"; }
const char* ts3plugin_description() { return "Stream Deck Soundboard – spielt WAV-Dateien direkt in den Mikrofonstream ein."; }

// ── Initialisierung ───────────────────────────────────────────────────────────

void ts3plugin_setFunctionPointers(const TS3Functions funcs) {
    ts3 = funcs;
}

int ts3plugin_init() {
    // Locale frühzeitig initialisieren
    Locale::instance().init(reinterpret_cast<const void*>(&ts3plugin_init));

    // Config-Pfad via TS3 API ermitteln und initialisieren (wie RP-Soundboard)
    char ts3ConfigPath[512] = {};
    ts3.getConfigPath(ts3ConfigPath, sizeof(ts3ConfigPath));
    initConfigPath(ts3ConfigPath);

    // Bereits verbundenen Server ermitteln (Plugin kann nach TS3-Start geladen werden)
    g_activeServer = findConnectedServer();

    // Pipe-Server starten und auf Kommandos vom Stream Deck lauschen
    PipeServer::instance().setVersion(PLUGIN_VERSION);
    PipeServer::instance().start([](const SoundCommand& cmd, std::string& errorOut) -> bool {
        if (cmd.stopAll) {
            AudioMixer::instance().stopAll();
            // CT-Restore als Pending setzen – wird im TS3-Thread ausgeführt
            g_pendingRestoreCT.store(true);
            return true;
        }

        if (cmd.type == "testFile") {
            if (cmd.filePath.empty()) {
                    errorOut = Locale::instance().t("err_no_filepath");
                    return false;
                }
            return AudioMixer::instance().testFile(cmd.filePath, errorOut);
        }

        if (!cmd.filePath.empty()) {
            // Basis-Lautstärken aus Config mit prozentualem Offset multiplizieren
            // Beispiel: Basis 40% (0.4) × (1 + (−0.1)) = 0.36 = 36%
            PluginConfig cfg = loadConfig();
            float volRemote = std::clamp(cfg.baseVolumeRemote * (1.0f + cmd.volumeOffset), 0.0f, 1.0f);
            float volLocal  = std::clamp(cfg.baseVolumeLocal  * (1.0f + cmd.volumeOffset), 0.0f, 1.0f);

            if (cfg.enableLogging) {
                char logBuf[512];
                snprintf(logBuf, sizeof(logBuf),
                    "[TS3Soundboard] Sound: %s | "
                    "Offset: %.2f | "
                    "Basis Remote: %.2f | Basis Lokal: %.2f | "
                    "Absolut Remote: %.2f (%.0f%%) | Absolut Lokal: %.2f (%.0f%%)",
                    cmd.filePath.c_str(),
                    cmd.volumeOffset,
                    cfg.baseVolumeRemote, cfg.baseVolumeLocal,
                    volRemote, volRemote * 100.0f,
                    volLocal,  volLocal  * 100.0f);
                ts3.logMessage(logBuf, LogLevel_INFO, "TS3Soundboard", 0);
                ts3.printMessageToCurrentTab(logBuf);
            }

            bool wasPlaying = AudioMixer::instance().isPlaying();
            bool ok = AudioMixer::instance().play(cmd.filePath, volRemote, volLocal, errorOut);
            if (ok && !wasPlaying) {
                // CT-Aktivierung als Pending setzen – wird im TS3-Thread ausgeführt
                g_pendingEnableCT.store(true);
            }
            if (!ok) {
                char errBuf[512];
                snprintf(errBuf, sizeof(errBuf),
                    "[TS3Soundboard] Fehler beim Abspielen von \"%s\": %s",
                    cmd.filePath.c_str(),
                    errorOut.empty() ? "Unbekannter Fehler" : errorOut.c_str());
                ts3.logMessage(errBuf, LogLevel_ERROR, "TS3Soundboard", 0);
                ts3.printMessageToCurrentTab(errBuf);
            }
            return ok;
        }

        errorOut = Locale::instance().t("err_incomplete_cmd");
        ts3.logMessage(("[TS3Soundboard] " + errorOut).c_str(), LogLevel_WARNING, "TS3Soundboard", 0);
        ts3.printMessageToCurrentTab(("[TS3Soundboard] " + errorOut).c_str());
        return false;
    });

    return 0;
}

void ts3plugin_shutdown() {
    PipeServer::instance().stop();
    AudioMixer::instance().stopAll();
    // Direkt aus Shutdown-Thread – hier ist kein Audio-Callback mehr aktiv,
    // daher direkt aufrufen
    if (g_ctActive && g_activeServer != 0 && g_previousTalkState != TS_INVALID)
        setTalkState(g_activeServer, g_previousTalkState);
    g_ctActive = false;
    if (g_pluginID) { free(g_pluginID); g_pluginID = nullptr; }
}

// ── Plugin-ID & Speicherverwaltung (für Menüs erforderlich) ──────────────────

void ts3plugin_registerPluginID(const char* id) {
    if (g_pluginID) free(g_pluginID);
    size_t sz = strlen(id) + 1;
    g_pluginID = (char*)malloc(sz);
    strncpy(g_pluginID, id, sz);
}

void ts3plugin_freeMemory(void* data) {
    free(data);
}

// ── Einstellungs-Button in der Plugin-Liste ───────────────────────────────────
// Wird aufgerufen wenn der User in Extras → Erweiterungen auf "Einstellungen" klickt.

void ts3plugin_configure(void* /*handle*/, void* /*qParentWidget*/) {
    showSettingsDialog(nullptr);
}

// ── Verbindungsstatus ─────────────────────────────────────────────────────────

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int /*errorNumber*/) {
    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        g_activeServer = serverConnectionHandlerID;
    } else if (newStatus == STATUS_DISCONNECTED && serverConnectionHandlerID == g_activeServer) {
        g_ctActive          = false;
        g_previousTalkState = TS_INVALID;
        g_activeServer      = findConnectedServer(); // Fallback auf anderen Server
    }
}

// ── Capture-Callback: Sound in Mikrofon-Stream einmischen (→ Remote) ──────────
// Läuft im TS3-Thread → hier sind TS3 SDK-Aufrufe sicher.

void ts3plugin_onEditCapturedVoiceDataEvent(
    uint64  serverConnectionHandlerID,
    short*  samples,
    int     sampleCount,
    int     channels,
    int*    edited
) {
    (void)serverConnectionHandlerID;

    // Pendente TalkState-Änderungen aus dem Pipe-Thread hier ausführen
    processPendingEnableCT();

    if (AudioMixer::instance().mixIntoCapture(samples, sampleCount, channels)) {
        *edited |= 1;
    } else {
        // Keine aktiven Sounds mehr → CT zurückschalten
        processPendingRestoreCT();
        if (g_ctActive)
            g_pendingRestoreCT.store(true); // nächster Callback räumt auf
    }
}

// ── Playback-Callback: Sound lokal abhören ────────────────────────────────────
// channelSpeakerArray enthält Speaker-Flags pro Kanal-Index.
// channelFillMask: Nur Bits der Kanäle setzen die wir beschreiben, damit
// andere Audioquellen (User-Stimmen) nicht überschrieben werden.

static int findChannelId(unsigned int mask, const unsigned int* arr, int count) {
    for (int i = 0; i < count; ++i)
        if (arr[i] & mask) return i;
    return 0;
}

void ts3plugin_onEditMixedPlaybackVoiceDataEvent(
    uint64              serverConnectionHandlerID,
    short*              samples,
    int                 sampleCount,
    int                 channels,
    const unsigned int* channelSpeakerArray,
    unsigned int*       channelFillMask
) {
    (void)serverConnectionHandlerID;

    const unsigned int bitMaskL = SPEAKER_FRONT_LEFT  | SPEAKER_HEADPHONES_LEFT;
    const unsigned int bitMaskR = SPEAKER_FRONT_RIGHT | SPEAKER_HEADPHONES_RIGHT;

    int ciLeft  = (channelSpeakerArray && channels > 0) ? findChannelId(bitMaskL, channelSpeakerArray, channels) : 0;
    int ciRight = (channelSpeakerArray && channels > 1) ? findChannelId(bitMaskR, channelSpeakerArray, channels) : (channels > 1 ? 1 : 0);

    // overLeft/overRight: Kanal nullen falls noch kein anderer ihn belegt hat
    // (identisch zu RP-Soundboard fetchOutputSamples-Logik)
    bool overLeft  = (*channelFillMask & bitMaskL) == 0;
    bool overRight = (*channelFillMask & bitMaskR) == 0;

    if (AudioMixer::instance().mixIntoPlayback(samples, sampleCount, channels,
                                               ciLeft, ciRight, overLeft, overRight)) {
        *channelFillMask |= (bitMaskL | bitMaskR);
    }
}

// ── Einstellungs-Menü ─────────────────────────────────────────────────────────

#define MENU_ID_SETTINGS 1

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    *menuIcon  = nullptr;
    *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * 2);

    (*menuItems)[0] = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
    (*menuItems)[0]->type = PLUGIN_MENU_TYPE_GLOBAL;
    (*menuItems)[0]->id   = MENU_ID_SETTINGS;
    strncpy((*menuItems)[0]->text, Locale::instance().t("menu_settings").c_str(), PLUGIN_MENU_BUFSZ - 1);
    strncpy((*menuItems)[0]->icon, "menu_icon.png", PLUGIN_MENU_BUFSZ - 1);

    (*menuItems)[1] = nullptr; // Terminator

    // Plugin-Untermenü-Icon (neben dem Plugin-Namen im Plugins-Menü)
    *menuIcon = (char*)malloc(PLUGIN_MENU_BUFSZ * sizeof(char));
    strncpy(*menuIcon, "menu_icon.png", PLUGIN_MENU_BUFSZ - 1);
}

void ts3plugin_onMenuItemEvent(uint64 /*serverConnectionHandlerID*/,
                               enum PluginMenuType type, int menuItemID,
                               uint64 /*selectedItemID*/) {
    if (type == PLUGIN_MENU_TYPE_GLOBAL && menuItemID == MENU_ID_SETTINGS)
        showSettingsDialog(nullptr);
}

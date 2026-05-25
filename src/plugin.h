#pragma once

#ifdef WIN32
    #define PLUGINS_EXPORTDLL __declspec(dllexport)
#else
    #define PLUGINS_EXPORTDLL __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <teamspeak/public_definitions.h>

// ── Pflichtfunktionen (TS3 SDK) ──────────────────────────────────────────────
PLUGINS_EXPORTDLL const char* ts3plugin_name();
PLUGINS_EXPORTDLL const char* ts3plugin_version();
PLUGINS_EXPORTDLL int         ts3plugin_apiVersion();
PLUGINS_EXPORTDLL const char* ts3plugin_author();
PLUGINS_EXPORTDLL const char* ts3plugin_description();
PLUGINS_EXPORTDLL int         ts3plugin_init();
PLUGINS_EXPORTDLL void        ts3plugin_shutdown();
PLUGINS_EXPORTDLL void        ts3plugin_setFunctionPointers(const struct TS3Functions funcs);

// ── Verbindungsstatus ─────────────────────────────────────────────────────────
PLUGINS_EXPORTDLL void ts3plugin_onConnectStatusChangeEvent(
    uint64       serverConnectionHandlerID,
    int          newStatus,
    unsigned int errorNumber
);

// ── Audio-Callbacks ───────────────────────────────────────────────────────────
PLUGINS_EXPORTDLL void ts3plugin_onEditCapturedVoiceDataEvent(
    uint64  serverConnectionHandlerID,
    short*  samples,
    int     sampleCount,
    int     channels,
    int*    edited
);

PLUGINS_EXPORTDLL void ts3plugin_onEditMixedPlaybackVoiceDataEvent(
    uint64              serverConnectionHandlerID,
    short*              samples,
    int                 sampleCount,
    int                 channels,
    const unsigned int* channelSpeakerArray,
    unsigned int*       channelFillMask
);

// ── Menü & Einstellungen ─────────────────────────────────────────────────────
PLUGINS_EXPORTDLL void ts3plugin_registerPluginID(const char* id);
PLUGINS_EXPORTDLL void ts3plugin_freeMemory(void* data);
PLUGINS_EXPORTDLL void ts3plugin_configure(void* handle, void* qParentWidget);
PLUGINS_EXPORTDLL void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon);
PLUGINS_EXPORTDLL void ts3plugin_onMenuItemEvent(
    uint64                serverConnectionHandlerID,
    enum PluginMenuType   type,
    int                   menuItemID,
    uint64                selectedItemID
);

#ifdef __cplusplus
}
#endif

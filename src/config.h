#pragma once
#include <string>

/// Globale Plugin-Konfiguration (gemeinsam von C++ und C#-Plugin genutzt).
/// Wird in <TS3ConfigPath>/ts3soundboard.json gespeichert.
/// Der TS3-Konfigurationspfad wird per ts3Functions.getConfigPath() ermittelt.
struct PluginConfig {
	float baseVolumeRemote = 0.8f;  ///< Basis-Lautstärke Remote (Mikrofon → TS3), 0.0–1.0
	float baseVolumeLocal  = 0.8f;  ///< Basis-Lautstärke Lokal (Lautsprecher), 0.0–1.0
	bool  enableLogging    = false; ///< Logging in TS3-Konsole aktivieren
};

/// Muss einmalig aus ts3plugin_init() aufgerufen werden.
/// Setzt den Basispfad für die Config-Datei (Wert von ts3Functions.getConfigPath()).
void initConfigPath(const char* ts3ConfigPath);

/// Gibt den vollständigen Pfad zur Config-Datei zurück.
std::string getConfigFilePath();

/// Lädt die Config aus der Datei. Gibt Defaults zurück wenn die Datei nicht existiert.
PluginConfig loadConfig();

/// Speichert die Config in die Datei.
bool saveConfig(const PluginConfig& cfg);

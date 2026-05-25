#include "config.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>

// ── Pfad ──────────────────────────────────────────────────────────────────────

static std::string g_configFilePath; ///< Wird in initConfigPath() gesetzt

void initConfigPath(const char* ts3ConfigPath) {
	std::string base = ts3ConfigPath ? ts3ConfigPath : "";
	// Trailing-Slash sicherstellen
	if (!base.empty() && base.back() != '/' && base.back() != '\\')
		base += '\\';
	g_configFilePath = base + "ts3soundboard.ini";
}

std::string getConfigFilePath() {
	return g_configFilePath;
}

// ── INI-Hilfsfunktionen ───────────────────────────────────────────────────────

/// Liest einen Wert aus einer einfachen INI-Datei (Section + Key).
static std::string iniRead(const std::string& path,
							const std::string& section,
							const std::string& key)
{
	std::ifstream f(path);
	if (!f.is_open()) return {};

	std::string currentSection;
	std::string line;
	while (std::getline(f, line)) {
		// Leerzeichen trimmen
		size_t s = line.find_first_not_of(" \t\r\n");
		if (s == std::string::npos) continue;
		line = line.substr(s);

		if (line.front() == '[') {
			size_t e = line.find(']');
			if (e != std::string::npos)
				currentSection = line.substr(1, e - 1);
			continue;
		}
		if (line.front() == ';' || line.front() == '#') continue;

		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;

		std::string k = line.substr(0, eq);
		// Key trimmen
		size_t ke = k.find_last_not_of(" \t");
		if (ke != std::string::npos) k = k.substr(0, ke + 1);

		if (currentSection == section && k == key) {
			std::string v = line.substr(eq + 1);
			size_t vs = v.find_first_not_of(" \t");
			size_t ve = v.find_last_not_of(" \t\r\n");
			if (vs != std::string::npos) return v.substr(vs, ve - vs + 1);
			return {};
		}
	}
	return {};
}

// ── Laden ─────────────────────────────────────────────────────────────────────

PluginConfig loadConfig() {
	PluginConfig cfg;
	const std::string& path = getConfigFilePath();

	auto readFloat = [&](const std::string& key, float def) -> float {
		std::string v = iniRead(path, "Volumes", key);
		if (v.empty()) return def;
		try { return std::clamp(std::stof(v), 0.0f, 1.0f); } catch (...) { return def; }
	};

	cfg.baseVolumeRemote = readFloat("BaseVolumeRemote", 0.8f);
	cfg.baseVolumeLocal  = readFloat("BaseVolumeLocal",  0.8f);

	std::string logVal = iniRead(path, "Logging", "EnableLogging");
	cfg.enableLogging = (logVal == "1" || logVal == "true");

	return cfg;
}

// ── Speichern ─────────────────────────────────────────────────────────────────

bool saveConfig(const PluginConfig& cfg) {
	std::ofstream f(getConfigFilePath());
	if (!f.is_open()) return false;

	char buf[256];
	snprintf(buf, sizeof(buf),
		"[Volumes]\n"
		"BaseVolumeRemote=%.2f\n"
		"BaseVolumeLocal=%.2f\n"
		"\n"
		"[Logging]\n"
		"EnableLogging=%d\n",
		cfg.baseVolumeRemote, cfg.baseVolumeLocal,
		cfg.enableLogging ? 1 : 0);
	f << buf;
	return f.good();
}

#pragma once

// Header-only Locale-Singleton für das TS3 Soundboard Plugin.
// Lädt eine einfache JSON-Datei ({lang}.json) aus dem locales/-Unterordner
// neben der DLL und bietet t() (char) sowie tw() (wchar_t) als Accessor.

// STL-Header müssen VOR windows.h kommen, damit WIN32_LEAN_AND_MEAN
// nicht die benötigten C-Locale-Defines entfernt.
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>

// Windows-Header danach – nur für GetUserDefaultLocaleName, GetModuleHandleExW etc.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Forward-Deklaration (DLL-Pfad wird von settings_dialog.cpp/plugin.cpp via
// GetModuleHandleExW aufgelöst – wir deklarieren den Symbol-Anker extern).
extern "C" void* _locale_anchor();

class Locale {
public:
	static Locale& instance() {
		static Locale inst;
		return inst;
	}

	// Initialisierung: muss einmalig mit einem beliebigen Symbol aus der DLL aufgerufen
	// werden, damit GetModuleHandleExW den Pfad der eigenen DLL ermitteln kann.
	void init(const void* symbolInDll) {
		if (m_initialized) return;
		m_initialized = true;

		// DLL-Verzeichnis ermitteln
		wchar_t dllPath[MAX_PATH] = {};
		HMODULE hMod = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(symbolInDll), &hMod);
		GetModuleFileNameW(hMod, dllPath, MAX_PATH);
		std::wstring dir(dllPath);
		auto slash = dir.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			dir = dir.substr(0, slash + 1);

		// Systemsprache ermitteln (z.B. "de-DE", "en-US", "zh-CN")
		wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
		GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH);
		std::wstring lang(localeName);

		// Versuche erst den vollen Namen, dann nur den Sprach-Prefix (z.B. "de")
		if (!tryLoad(dir + L"ts3_soundboard\\locales\\" + lang + L".json")) {
			auto dash = lang.find(L'-');
			std::wstring prefix = (dash != std::wstring::npos) ? lang.substr(0, dash) : lang;
			if (!tryLoad(dir + L"ts3_soundboard\\locales\\" + prefix + L".json")) {
				tryLoad(dir + L"ts3_soundboard\\locales\\en.json"); // Fallback
			}
		}
	}

	// Gibt den lokalisierten String als std::string (narrow UTF-8/ANSI) zurück.
	// Liefert den Schlüssel selbst zurück, wenn kein Eintrag gefunden wurde.
	const std::string& t(const std::string& key) const {
		auto it = m_map.find(key);
		if (it != m_map.end()) return it->second;
		static thread_local std::string fallback;
		fallback = key;
		return fallback;
	}

	// Gibt den lokalisierten String als std::wstring zurück.
	std::wstring tw(const std::string& key) const {
		const std::string& s = t(key);
		if (s.empty()) return std::wstring(key.begin(), key.end());
		int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
		std::wstring result(len - 1, 0);
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, result.data(), len);
		return result;
	}

private:
	Locale() = default;
	bool m_initialized = false;
	std::unordered_map<std::string, std::string> m_map;

	bool tryLoad(const std::wstring& path) {
		// Pfad in narrow umwandeln für ifstream
		int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string narrowPath(len - 1, 0);
		WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrowPath.data(), len, nullptr, nullptr);

		std::ifstream f(narrowPath);
		if (!f.is_open()) return false;

		std::string content((std::istreambuf_iterator<char>(f)),
							 std::istreambuf_iterator<char>());
		parseJson(content);
		return !m_map.empty();
	}

	// Minimaler JSON-Parser: erkennt nur flache {"key":"value",...}-Strukturen.
	// Werte dürfen \" und \n enthalten, aber keine verschachtelten Objekte.
	void parseJson(const std::string& json) {
		size_t pos = 0;
		auto skipWs = [&]() {
			while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
				   json[pos] == '\r' || json[pos] == '\n')) ++pos;
		};
		auto readString = [&]() -> std::string {
			if (pos >= json.size() || json[pos] != '"') return {};
			++pos; // skip opening "
			std::string result;
			while (pos < json.size()) {
				char c = json[pos++];
				if (c == '"') break;
				if (c == '\\' && pos < json.size()) {
					char esc = json[pos++];
					switch (esc) {
						case '"':  result += '"';  break;
						case '\\': result += '\\'; break;
						case '/':  result += '/';  break;
						case 'n':  result += '\n'; break;
							case 'r':  result += '\r'; break;
							case 't':  result += '\t'; break;
							case 'u': {
								if (pos + 4 <= json.size()) {
									std::string hex = json.substr(pos, 4);
									pos += 4;
									unsigned int cp = 0;
									for (char h : hex) {
										cp <<= 4;
										if (h >= '0' && h <= '9') cp |= (h - '0');
										else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
										else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
									}
									if (cp < 0x80) {
										result += (char)cp;
									} else if (cp < 0x800) {
										result += (char)(0xC0 | (cp >> 6));
										result += (char)(0x80 | (cp & 0x3F));
									} else {
										result += (char)(0xE0 | (cp >> 12));
										result += (char)(0x80 | ((cp >> 6) & 0x3F));
										result += (char)(0x80 | (cp & 0x3F));
									}
								}
								break;
							}
							default:   result += esc;  break;
					}
				} else {
					result += c;
				}
			}
			return result;
		};

		skipWs();
		if (pos < json.size() && json[pos] == '{') ++pos;
		while (pos < json.size()) {
			skipWs();
			if (pos < json.size() && json[pos] == '}') break;
			if (pos < json.size() && json[pos] == ',') { ++pos; continue; }
			std::string key = readString();
			skipWs();
			if (pos < json.size() && json[pos] == ':') ++pos;
			skipWs();
			std::string value = readString();
			if (!key.empty())
				m_map[key] = value;
		}
	}
};

#pragma once
#include "config.h"
#include <windows.h>

/// Öffnet den modalen Einstellungsdialog. Blockiert bis der Benutzer schließt.
/// Liest die aktuelle Config, zeigt sie an und speichert bei OK.
/// hwndParent darf nullptr sein.
void showSettingsDialog(HWND hwndParent);

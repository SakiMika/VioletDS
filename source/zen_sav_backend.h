#pragma once
#include <stdint.h>

namespace ZenSav {
// Ragnarok Violet WIPI file persistence.
//
// The shipped NDS build uses the runner's 128 KiB cartridge EEPROM backend.
// Stock melonDS requires a ROM-list-compatible header code to select that save
// geometry, so the ROM header uses CDLG while RVDS remains the project/port ID.
// The live game names are exactly save0, save1, save2, option and list.
void InitFatFallback();
void EnableNativeProbe();
void Flush();

bool IsPersistentName(const char *name);
int RecordIdForName(const char *name);

bool Exists(const char *name, uint32_t *out_len = nullptr);
int Load(const char *name, uint8_t **out_data, uint32_t *out_len);
int Store(const char *name, const uint8_t *data, uint32_t len);
int Remove(const char *name);

const char *BackendName();
}

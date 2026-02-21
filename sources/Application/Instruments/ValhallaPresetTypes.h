#ifndef _VALHALLA_PRESET_TYPES_H_
#define _VALHALLA_PRESET_TYPES_H_

// Shared types for all Valhalla embedded preset headers.

struct ValhallaEmbeddedPreset {
    const char *name;
    const char *xmlData;
};

struct ValhallaEmbeddedBank {
    const char *bankName;
    const ValhallaEmbeddedPreset *presets;
    int presetCount;
};

#endif // _VALHALLA_PRESET_TYPES_H_

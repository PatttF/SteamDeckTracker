#ifndef _TAL_REVERB4_PRESETS_H_
#define _TAL_REVERB4_PRESETS_H_

// Hardcoded TAL-Reverb-4 factory preset names in the exact order
// the plugin's IUnitInfo/kIsProgramChange parameter expects.
// Extracted from the embedded BinaryData in the native Linux VST3.
// These are fixed factory presets that never change.

#define TAL_REVERB4_PRESET_COUNT 31

static const char *talReverb4PresetNames[TAL_REVERB4_PRESET_COUNT] = {
    "Kick Boom",
    "Detuned Harmonics",
    "Wind Verb2",
    "Large Ducking",
    "Kick Boom II",
    "Harmonic Verb Octave",
    "Slap Back",
    "Wind Verb",
    "Short",
    "Dark Small",
    "Reflection",
    "Short Noisy",
    "Reflection Unisono",
    "Dark Small Unisono Ducking",
    "Dark Small Unisono",
    "Noisy One",
    "Crystal Clear Unisono",
    "Drums Mid-Range",
    "Crystal Clear Harmonic",
    "Crystal Clear Ducking",
    "Drum Ambience",
    "Crystal Clear",
    "Drum Ambience II",
    "Unstable Noise",
    "Wobble Plate",
    "Crystal Clear Big",
    "Dark Big",
    "Crystal Clear Big Octave",
    "Crystal Clear Big Unisono",
    "Crystal Clear Big Ducking",
    "Default"
};

#endif

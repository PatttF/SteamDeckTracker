#ifndef _PROTON_BRIDGE_H_
#define _PROTON_BRIDGE_H_

#include <string>
#include <vector>

// Represents a discovered Proton installation
struct ProtonVersion {
    std::string name;     // e.g. "Proton 9.0-4" or "Proton - Experimental"
    std::string winePath; // Full path to wine binary
    int major;            // Major version (0 for Experimental)
    int minor;            // Minor version
    int patch;            // Patch version
    bool isExperimental;  // True if this is Experimental

    ProtonVersion() : major(0), minor(0), patch(0), isExperimental(false) {}
};

// Discover all available Proton installations
// Returns list sorted by preference: Experimental first, then highest version
std::vector<ProtonVersion> discoverAllProton(const char *homeDir);

// Find the best Proton wine binary path
// Checks config for PROTONVERSION preference, otherwise returns best available
std::string findProtonWine(const char *homeDir);

// Main entry point: bridge Windows VST3 plugins using yabridge + Proton
// Called during application startup
void bridgeWindowsVST3s();

#endif // _PROTON_BRIDGE_H_

#include "ProtonBridge.h"
#include "System/Console/Trace.h"
#include "Application/Model/Config.h"
#include "System/FileSystem/FileSystem.h"

#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Helper: Parse version number from Proton directory name
static void parseProtonVersion(const std::string& name, ProtonVersion& version) {
    version.name = name;
    version.isExperimental = (name.find("Experimental") != std::string::npos);
    
    if (version.isExperimental) {
        version.major = version.minor = version.patch = 0;
        return;
    }
    
    // Parse "Proton X.Y-Z" format
    size_t pos = name.find("Proton ");
    if (pos == std::string::npos) {
        return;
    }
    
    const char* numStart = name.c_str() + pos + 7; // Skip "Proton "
    char* end = nullptr;
    version.major = (int)strtol(numStart, &end, 10);
    
    if (end && *end == '.') {
        version.minor = (int)strtol(end + 1, &end, 10);
    }
    
    if (end && *end == '-') {
        version.patch = (int)strtol(end + 1, &end, 10);
    }
}

// Helper: Check if a wine binary exists at the given path
static bool checkWinePath(const std::string& basePath, const char* subpath, std::string& outWinePath) {
    std::string fullPath = basePath + "/" + subpath;
    struct stat st;
    if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        outWinePath = fullPath;
        return true;
    }
    return false;
}

// Discover all available Proton installations
std::vector<ProtonVersion> discoverAllProton(const char* homeDir) {
    std::vector<ProtonVersion> versions;
    
    std::string steamPath = std::string(homeDir) + "/.steam/steam/steamapps/common";
    DIR* dir = opendir(steamPath.c_str());
    if (!dir) {
        Trace::Log("BRIDGE", "Steam directory not found: %s", steamPath.c_str());
        return versions;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Check if name contains "Proton"
        if (strstr(entry->d_name, "Proton") == nullptr) {
            continue;
        }
        
        std::string protonPath = steamPath + "/" + entry->d_name;
        ProtonVersion version;
        parseProtonVersion(entry->d_name, version);
        
        // Try to find wine binary (newer location first, then older)
        if (checkWinePath(protonPath, "files/bin/wine", version.winePath)) {
            versions.push_back(version);
        } else if (checkWinePath(protonPath, "dist/bin/wine", version.winePath)) {
            versions.push_back(version);
        }
    }
    
    closedir(dir);
    
    // Sort: Experimental first, then by version descending
    std::sort(versions.begin(), versions.end(), [](const ProtonVersion& a, const ProtonVersion& b) {
        if (a.isExperimental != b.isExperimental) {
            return a.isExperimental; // Experimental comes first
        }
        if (a.major != b.major) return a.major > b.major;
        if (a.minor != b.minor) return a.minor > b.minor;
        return a.patch > b.patch;
    });
    
    // Log discovered versions
    Trace::Log("BRIDGE", "Found %d Proton installation(s)", (int)versions.size());
    for (size_t i = 0; i < versions.size(); i++) {
        Trace::Log("BRIDGE", "  [%d] %s -> %s", (int)i, versions[i].name.c_str(), versions[i].winePath.c_str());
    }
    
    return versions;
}

// Find the best Proton wine binary path
std::string findProtonWine(const char* homeDir) {
    // Check for user preference in config
    const char* preferredVersion = Config::GetInstance()->GetValue("PROTONVERSION");
    
    std::vector<ProtonVersion> versions = discoverAllProton(homeDir);
    if (versions.empty()) {
        return "";
    }
    
    // If user specified a preference, try to find it
    if (preferredVersion && preferredVersion[0] != '\0') {
        for (const auto& v : versions) {
            if (v.name.find(preferredVersion) != std::string::npos) {
                Trace::Log("BRIDGE", "Using preferred Proton: %s", v.name.c_str());
                return v.winePath;
            }
        }
        Trace::Log("BRIDGE", "Preferred Proton '%s' not found, using auto-selected version", preferredVersion);
    }
    
    // Return the best (first) version
    Trace::Log("BRIDGE", "Auto-selected Proton: %s", versions[0].name.c_str());
    return versions[0].winePath;
}

// Main entry point: bridge Windows VST3 plugins using yabridge + Proton
void bridgeWindowsVST3s() {
    // Get HOME environment variable
    const char* home = getenv("HOME");
    if (!home) {
        Trace::Log("BRIDGE", "HOME environment variable not set, skipping Windows VST3 bridge");
        return;
    }
    
    // Create ~/.vst3/win/ directory if it doesn't exist
    std::string vst3WinDir = std::string(home) + "/.vst3/win";
    struct stat st;
    if (stat(vst3WinDir.c_str(), &st) != 0) {
        // Create parent directory first if needed
        std::string vst3Dir = std::string(home) + "/.vst3";
        if (stat(vst3Dir.c_str(), &st) != 0) {
            if (mkdir(vst3Dir.c_str(), 0755) != 0) {
                Trace::Log("BRIDGE", "Failed to create ~/.vst3 directory");
                return;
            }
        }
        if (mkdir(vst3WinDir.c_str(), 0755) != 0) {
            Trace::Log("BRIDGE", "Failed to create ~/.vst3/win directory");
            return;
        }
        Trace::Log("BRIDGE", "Created %s for Windows VST3 files", vst3WinDir.c_str());
    }
    
    // Find Proton wine binary
    std::string winePath = findProtonWine(home);
    if (winePath.empty()) {
        Trace::Log("BRIDGE", "No Proton installation found, skipping Windows VST3 bridge");
        return;
    }
    
    // Locate bundled yabridge yabridgectl binary
    const char* candidates[] = {
        "bin:yabridge/yabridgectl",
        "bin:../yabridge/yabridgectl"
    };
    
    Path yabridgectlPath;
    bool found = false;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        Path candidate(candidates[i]);
        if (FileSystem::GetInstance()->GetFileType(candidate.GetPath().c_str()) == FT_FILE) {
            yabridgectlPath = candidate;
            found = true;
            break;
        }
    }
    
    if (!found) {
        Trace::Log("BRIDGE", "yabridge not bundled, skipping Windows VST3 bridge");
        return;
    }
    
    std::string yabridgectlBin = yabridgectlPath.GetCanonicalPath();
    Trace::Log("BRIDGE", "Found yabridgectl: %s", yabridgectlBin.c_str());
    
    // Run yabridgectl add "~/.vst3/win/"
    std::string addCmd = "\"" + yabridgectlBin + "\" add \"" + vst3WinDir + "\" 2>&1";
    Trace::Log("BRIDGE", "Running: %s", addCmd.c_str());
    
    FILE* addPipe = popen(addCmd.c_str(), "r");
    if (addPipe) {
        char line[256];
        while (fgets(line, sizeof(line), addPipe)) {
            // Strip trailing newline
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
            Trace::Log("BRIDGE", "  %s", line);
        }
        int addResult = pclose(addPipe);
        if (addResult != 0) {
            Trace::Log("BRIDGE", "yabridgectl add failed with code %d", addResult);
        }
    } else {
        Trace::Log("BRIDGE", "Failed to run yabridgectl add");
        return;
    }
    
    // Run WINELOADER="..." yabridgectl sync
    std::string syncCmd = "WINELOADER=\"" + winePath + "\" \"" + yabridgectlBin + "\" sync 2>&1";
    Trace::Log("BRIDGE", "Running: %s", syncCmd.c_str());
    
    FILE* syncPipe = popen(syncCmd.c_str(), "r");
    if (syncPipe) {
        char line[256];
        while (fgets(line, sizeof(line), syncPipe)) {
            // Strip trailing newline
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
            Trace::Log("BRIDGE", "  %s", line);
        }
        int syncResult = pclose(syncPipe);
        if (syncResult == 0) {
            Trace::Log("BRIDGE", "Windows VST3 bridge setup completed successfully");
        } else {
            Trace::Log("BRIDGE", "yabridgectl sync failed with code %d", syncResult);
        }
    } else {
        Trace::Log("BRIDGE", "Failed to run yabridgectl sync");
    }
}

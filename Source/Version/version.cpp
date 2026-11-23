//-----------------------------------------------------------------------------
//           Name: version.cpp
//      Developer: Wolfire Games LLC
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
//
//   Copyright 2022 Wolfire Games LLC
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//-----------------------------------------------------------------------------
#include "version.h"

#include <overgrowth_version.h>

#include <algorithm>
#include <array>
#include <vector>
#include <sstream>
#include <string>

extern const int phoenix_build_id;
extern const char* phoenix_platform;
extern const char* phoenix_arch;
extern const char* phoenix_git_version_string;
extern const char* phoenix_build_timestamp;

namespace {
// This number describes the wire format, independently of release/build tags.
constexpr int kOnlineProtocol = 7;

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> result;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(separator, start);
        result.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) return result;
        start = end + 1;
    }
}

bool Digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}

bool Identifier(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') || c == '-';
    });
}

int CompareNumber(const std::string& a, const std::string& b) {
    // Comparing digit strings avoids overflow for remote version identifiers.
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a == b ? 0 : (a < b ? -1 : 1);
}

struct SemanticVersion {
    std::array<std::string, 3> core;
    std::vector<std::string> prerelease;
};

bool ParseSemanticVersion(const std::string& text, SemanticVersion& version) {
    std::string value = text;
    const size_t metadata = value.find('+');
    if (metadata != std::string::npos) {
        for (const auto& id : Split(value.substr(metadata + 1), '.')) {
            if (!Identifier(id)) return false;
        }
        value.resize(metadata);
    }
    const size_t prerelease = value.find('-');
    if (prerelease != std::string::npos) {
        version.prerelease = Split(value.substr(prerelease + 1), '.');
        for (const auto& id : version.prerelease) {
            if (!Identifier(id) || (Digits(id) && id.size() > 1 && id[0] == '0')) return false;
        }
        value.resize(prerelease);
    }
    const auto numbers = Split(value, '.');
    if (numbers.size() != version.core.size()) return false;
    for (size_t i = 0; i < numbers.size(); ++i) {
        if (!Digits(numbers[i]) || (numbers[i].size() > 1 && numbers[i][0] == '0')) return false;
        version.core[i] = numbers[i];
    }
    return true;
}

std::string WithMetadata(const std::string& version, const std::string& metadata) {
    return version + (version.find('+') == std::string::npos ? "+" : ".") + metadata;
}

std::string UpstreamComponent(size_t index) {
    const auto parts = Split(GetShortBuildTag(), '.');
    return index < parts.size() ? parts[index] : std::string();
}
}  // namespace

int GetBuildID() {
    return phoenix_build_id;
}

const char* GetBuildIDString() {
    static bool init = false;
    static std::string buf;

    if (phoenix_build_id == -1)
        return "none";

    if (!init) {
        std::stringstream ss;
        ss << "build-" << phoenix_build_id;
        buf = ss.str();
        init = true;
    }

    return buf.c_str();
}

const char* GetPlatform() {
    return phoenix_platform;
}

const char* GetArch() {
    return phoenix_arch;
}

const char* GetBuildVersion() {
    static const std::string display = WithMetadata(GetForkVersion(),
        "git." + std::string(GetSourceRevision()).substr(0, 12));
    return display.c_str();
}

const char* GetForkVersion() {
    return OVERGROWTH_FORK_VERSION;
}

const char* GetSourceRevision() {
    return OVERGROWTH_SOURCE_REVISION;
}

const char* GetUpstreamBuildVersion() {
    return phoenix_git_version_string;
}

const char* GetAssetCompatibilityVersion() {
    return OVERGROWTH_ASSET_COMPATIBILITY_VERSION;
}

int GetOnlineProtocol() {
    return kOnlineProtocol;
}

const std::string& GetNetworkVersion() {
    static const std::string version = WithMetadata(GetForkVersion(), "local-lobby" + std::to_string(GetOnlineProtocol()));
    return version;
}

bool IsNetworkVersionCompatible(const std::string& remote_version) {
    return remote_version == GetNetworkVersion();
}

int CompareSemanticVersions(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return a == b ? 0 : (a.empty() ? -1 : 1);
    SemanticVersion left, right;
    const bool valid_left = ParseSemanticVersion(a, left);
    const bool valid_right = ParseSemanticVersion(b, right);
    if (valid_left != valid_right) return valid_left ? 1 : -1;
    // Invalid/legacy text has deterministic ordering, but cannot masquerade as
    // a valid release or make letters contribute bogus digits to its version.
    if (!valid_left) return a == b ? 0 : (a < b ? -1 : 1);
    for (size_t i = 0; i < left.core.size(); ++i) {
        const int order = CompareNumber(left.core[i], right.core[i]);
        if (order != 0) return order;
    }
    if (left.prerelease.empty() || right.prerelease.empty()) {
        if (left.prerelease.empty() == right.prerelease.empty()) return 0;
        return left.prerelease.empty() ? 1 : -1;
    }
    const size_t common = std::min(left.prerelease.size(), right.prerelease.size());
    for (size_t i = 0; i < common; ++i) {
        const auto& x = left.prerelease[i];
        const auto& y = right.prerelease[i];
        const bool nx = Digits(x), ny = Digits(y);
        if (nx != ny) return nx ? -1 : 1;
        const int order = nx ? CompareNumber(x, y) : (x == y ? 0 : (x < y ? -1 : 1));
        if (order != 0) return order;
    }
    return left.prerelease.size() == right.prerelease.size() ? 0 :
           (left.prerelease.size() < right.prerelease.size() ? -1 : 1);
}

const char* GetBuildTimestamp() {
    return phoenix_build_timestamp;
}

std::string GetShortBuildTag() {
    return GetAssetCompatibilityVersion();
}

std::string GetVersionMajor() {
    return UpstreamComponent(0);
}

std::string GetVersionMinor() {
    return UpstreamComponent(1);
}

std::string GetVersionPatch() {
    return UpstreamComponent(2);
}

std::string GetShortBuildTimestamp() {
    const std::string timestamp(GetBuildTimestamp());
    return timestamp.substr(0, timestamp.find(' '));
}

const std::string& GetFullBuildString() {
    static bool init = false;
    static std::string buf;

    if (!init) {
        std::stringstream ss;

        ss << GetBuildVersion() << "_source-" << GetSourceRevision() << "_git-describe-" << GetUpstreamBuildVersion()
           << "_asset-api-" << GetAssetCompatibilityVersion()
           << "_" << phoenix_arch << "_" << phoenix_platform << "_" << phoenix_build_id << "_" << phoenix_build_timestamp;
        init = true;
        buf = ss.str();
    }

    return buf;
}

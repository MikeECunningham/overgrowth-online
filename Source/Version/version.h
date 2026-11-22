//-----------------------------------------------------------------------------
//           Name: version.h
//      Developer: Wolfire Games LLC
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
//
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
#pragma once

#include <string>

const char* GetBuildIDString();
int GetBuildID();
const char* GetPlatform();
const char* GetArch();
const char* GetBuildVersion();
const char* GetBuildTimestamp();

// The release is independent of an optional data mod and the checkout's Git tag.
const char* GetForkVersion();
const char* GetSourceRevision();
const char* GetUpstreamBuildVersion();
const char* GetAssetCompatibilityVersion();
int GetOnlineProtocol();
const std::string& GetNetworkVersion();
bool IsNetworkVersionCompatible(const std::string& remote_version);
int CompareSemanticVersions(const std::string& a, const std::string& b);

// Legacy asset compatibility helpers use the fixed upstream asset API version.
std::string GetShortBuildTag();
std::string GetVersionMajor();
std::string GetVersionMinor();
std::string GetVersionPatch();
std::string GetShortBuildTimestamp();

const std::string& GetFullBuildString();

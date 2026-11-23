#include <Online/online_utility.h>
#include <Version/version.h>

#include <tut/tut.hpp>

#include <string>
#include <vector>

namespace tut {
struct dataversion {};
typedef test_group<dataversion> tg_version;
tg_version test_group_version("Version");
typedef tg_version::object version;

template <> template <>
void version::test<1>() {
    set_test_name("the native release exists without initializing or enabling a data mod");
    ensure("canonical release is nonempty", std::string(GetForkVersion()).size() > 0);
    ensure_equals(OnlineUtility::GetOvergrowthOnlineVersion(), std::string(GetForkVersion()));
    ensure("canonical release has valid SemVer precedence", CompareSemanticVersions(GetForkVersion(), "invalid") > 0);
    ensure("display contains release", std::string(GetBuildVersion()).find(GetForkVersion()) == 0);
    ensure("display contains source revision", std::string(GetBuildVersion()).find(std::string(GetSourceRevision()).substr(0, 12)) != std::string::npos);
    ensure("diagnostics retain full source revision", GetFullBuildString().find(GetSourceRevision()) != std::string::npos);
}

template <> template <>
void version::test<2>() {
    set_test_name("network compatibility requires the exact release and protocol, including prerelease");
    ensure_equals("campaign scaling protocol", GetOnlineProtocol(), 9);
    ensure("matching peers connect", IsNetworkVersionCompatible(GetNetworkVersion()));
    ensure("unversioned peers rejected", !IsNetworkVersionCompatible(""));
    ensure("old mod-disabled identity rejected", !IsNetworkVersionCompatible("+local-lobby4"));
    ensure("release without protocol rejected", !IsNetworkVersionCompatible(GetForkVersion()));
    ensure("other protocol rejected", !IsNetworkVersionCompatible(std::string(GetForkVersion()) + "+local-lobby6"));
    ensure("old release rejected", !IsNetworkVersionCompatible("0.41.36+local-lobby4"));
}

template <> template <>
void version::test<3>() {
    set_test_name("SemVer prerelease precedence follows numeric and textual identifier rules");
    const std::vector<std::string> sequence = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"
    };
    for (size_t i = 1; i < sequence.size(); ++i) {
        ensure("increasing precedence", OnlineUtility::CompareVersions(sequence[i - 1], sequence[i]) < 0);
        ensure("symmetric comparison", OnlineUtility::CompareVersions(sequence[i], sequence[i - 1]) > 0);
    }
    ensure("alpha.10 is newer than alpha.2", CompareSemanticVersions("0.42.0-alpha.10", "0.42.0-alpha.2") > 0);
    ensure("numeric identifier is before text", CompareSemanticVersions("0.42.0-1", "0.42.0-alpha") < 0);
}

template <> template <>
void version::test<4>() {
    set_test_name("build metadata has no semantic precedence and numeric identifiers cannot overflow");
    ensure_equals(CompareSemanticVersions("0.42.0-alpha.1+local-lobby4", "0.42.0-alpha.1+git.abcdef"), 0);
    ensure_equals(CompareSemanticVersions("1.2.3+build.001", "1.2.3"), 0);
    ensure("numeric core compared numerically", CompareSemanticVersions("1.10.0", "1.9.99") > 0);
    ensure("large core values do not overflow", CompareSemanticVersions("999999999999999999999999.0.0", "99999999999999999999999.9.9") > 0);
    ensure("large prerelease values do not overflow", CompareSemanticVersions("1.0.0-999999999999999999999999", "1.0.0-99999999999999999999999") > 0);
}

template <> template <>
void version::test<5>() {
    set_test_name("invalid or missing versions cannot masquerade as valid releases");
    const std::vector<std::string> invalid = {"", "+local-lobby4", "1.2", "1.2.3.4", "01.2.3", "1.2.3-01", "1.2.3-alpha..1", "1.2.3+", "letters"};
    for (const auto& value : invalid) {
        ensure("invalid release orders before valid release", CompareSemanticVersions(value, "0.0.0") < 0);
        ensure("comparison is symmetric", CompareSemanticVersions("0.0.0", value) > 0);
    }
    ensure_equals(CompareSemanticVersions("", ""), 0);
}

template <> template <>
void version::test<6>() {
    set_test_name("fork release tags cannot change the existing asset compatibility version");
    ensure_equals(std::string(GetAssetCompatibilityVersion()), std::string("0.41.36"));
    ensure_equals(GetShortBuildTag(), std::string(GetAssetCompatibilityVersion()));
    ensure_equals(GetVersionMajor(), std::string("0"));
    ensure_equals(GetVersionMinor(), std::string("41"));
    ensure_equals(GetVersionPatch(), std::string("36"));
}
}  // namespace tut

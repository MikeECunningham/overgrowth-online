// Local viewport and camera namespace regressions. Apache-2.0.
#include <Graphics/local_viewport.h>
#include <Graphics/camera.h>

#include <tut/tut.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace tut {
struct datalocalviewport {};
typedef test_group<datalocalviewport> tg_localviewport;
tg_localviewport test_group_localviewport("LocalViewports");
typedef tg_localviewport::object localviewport;

template <>
template <>
void localviewport::test<1>() {
    set_test_name("two players stack vertically; larger grids grow horizontally first");
    const int counts[] = {1, 2, 3, 4, 5, 6, 7, 9, 10, 12, 13, 16, 17, 20};
    const int columns[] = {1, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5};
    const int rows[] = {1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        LocalViewports::Grid grid = LocalViewports::ForPlayers(counts[i]);
        ensure_equals("columns", grid.columns, columns[i]);
        ensure_equals("rows", grid.rows, rows[i]);
    }
    LocalViewports::Grid largest = LocalViewports::ForPlayers(std::numeric_limits<int>::max());
    ensure_equals("largest count columns without overflow", largest.columns, 46341);
    ensure_equals("largest count rows without overflow", largest.rows, 46341);
}

template <>
template <>
void localviewport::test<2>() {
    set_test_name("players run left to right from the top; spare cells are at the bottom right");
    LocalViewports::Rect top = LocalViewports::ForPlayer(0, 2);
    LocalViewports::Rect bottom = LocalViewports::ForPlayer(1, 2);
    ensure("two-player P1 spans the top half", top.left == 0.0f && top.bottom == 0.5f && top.right == 1.0f && top.top == 1.0f);
    ensure("two-player P2 spans the bottom half", bottom.left == 0.0f && bottom.bottom == 0.0f && bottom.right == 1.0f && bottom.top == 0.5f);
    LocalViewports::Rect p1 = LocalViewports::ForPlayer(0, 3);
    LocalViewports::Rect p2 = LocalViewports::ForPlayer(1, 3);
    LocalViewports::Rect p3 = LocalViewports::ForPlayer(2, 3);
    ensure("P1 top left", p1.left == 0.0f && p1.bottom == 0.5f && p1.right == 0.5f && p1.top == 1.0f);
    ensure("P2 top right", p2.left == 0.5f && p2.bottom == 0.5f && p2.right == 1.0f && p2.top == 1.0f);
    ensure("P3 bottom left", p3.left == 0.0f && p3.bottom == 0.0f && p3.right == 0.5f && p3.top == 0.5f);
    LocalViewports::Rect invalid = LocalViewports::ForPlayer(3, 3);
    ensure("unused cell has no player viewport", invalid.left == invalid.right && invalid.bottom == invalid.top);
}

template <>
template <>
void localviewport::test<3>() {
    set_test_name("odd-sized pixel grids do not overlap or leave internal cracks");
    const int counts[] = {1, 2, 3, 5, 7, 15, 17, 65, 255, 256, 257};
    const int width = 1023, height = 769;
    for (int count : counts) {
        std::vector<unsigned char> covered(width * height, 0);
        bool no_overlap = true;
        for (int player = 0; player < count; ++player) {
            const LocalViewports::Rect r = LocalViewports::ForPlayer(player, count);
            ensure("bounded positive rectangle", r.left >= 0.0f && r.right <= 1.0f && r.bottom >= 0.0f && r.top <= 1.0f && r.left < r.right && r.bottom < r.top);
            const int x0 = static_cast<int>(r.left * width), x1 = static_cast<int>(r.right * width);
            const int y0 = static_cast<int>(r.bottom * height), y1 = static_cast<int>(r.top * height);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    no_overlap = no_overlap && covered[y * width + x] == 0;
                    ++covered[y * width + x];
                }
            }
        }
        ensure("pixel covered only once", no_overlap);
        // The complete top row of cells is occupied whenever count > 1.
        for (int x = 0; x < width; ++x) {
            ensure("top row reaches both window edges without gaps", covered[(height - 1) * width + x] == 1);
        }
    }
}

template <>
template <>
void localviewport::test<4>() {
    set_test_name("local camera 255 does not alias a remote or preview camera");
    ActiveCameras* cameras = ActiveCameras::Instance();
    ActiveCameras::Set(0);
    Camera* primary = ActiveCameras::Get();
    ActiveCameras::Set(256);
    Camera* local = ActiveCameras::GetCamera(255);
    ensure("local beyond former virtual boundary exists", local != nullptr);
    const int remote_id = cameras->CreateVirtualCameraInstance();
    Camera* remote = ActiveCameras::GetCamera(remote_id);
    ensure("remote and local are different", remote && remote != local && remote != primary);
    const int local_count = ActiveCameras::NumCameras();
    ActiveCameras::SetPreview();
    ensure("preview uses its own camera", ActiveCameras::Get() != local && ActiveCameras::Get() != remote);
    ensure_equals("preview does not consume a player slot", ActiveCameras::NumCameras(), local_count);
    cameras->ResetLocalCameras();
    ensure_equals("local cameras reset on level change", ActiveCameras::NumCameras(), 1);
    ensure("remote camera survives level change", ActiveCameras::GetCamera(remote_id) == remote);
    cameras->FreeVirtualCameraInstance(remote_id);
    cameras->Dispose();
    const int fresh_remote = cameras->CreateVirtualCameraInstance();
    ensure("disposing clears stale free indices", ActiveCameras::GetCamera(fresh_remote) != nullptr);
    cameras->Dispose();
    ActiveCameras::Set(0);
}
}  // namespace tut

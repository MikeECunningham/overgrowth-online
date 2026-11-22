// Local split-screen cells, in row-major order starting at the top left.
// Copyright 2026. Licensed under the Apache License, Version 2.0.
#pragma once

namespace LocalViewports {

struct Grid {
    int columns;
    int rows;
};

struct Rect {
    float left;
    float bottom;
    float right;
    float top;
};

inline Grid ForPlayers(int player_count) {
    if (player_count < 1) {
        player_count = 1;
    }
    // Smallest square containing the players, then discard an unused last row.
    // Division avoids overflow for counts near the integer limit.
    int columns = 1;
    while (columns < player_count / columns + (player_count % columns != 0)) {
        ++columns;
    }
    return {columns, player_count / columns + (player_count % columns != 0)};
}

inline Rect ForPlayer(int player_index, int player_count) {
    if (player_index < 0 || player_index >= player_count) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const Grid grid = ForPlayers(player_count);
    const int column = player_index % grid.columns;
    const int row = player_index / grid.columns;
    return {column / static_cast<float>(grid.columns),
            (grid.rows - row - 1) / static_cast<float>(grid.rows),
            (column + 1) / static_cast<float>(grid.columns),
            (grid.rows - row) / static_cast<float>(grid.rows)};
}

}  // namespace LocalViewports

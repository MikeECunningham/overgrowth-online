#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

namespace FontBitmap {

// Canvas text shares cached faces with atlases. Its fractional pen position or
// rotation must never become part of a cached atlas glyph.
inline FT_Error RenderAtlasGlyph(FT_Face face, unsigned long codepoint) {
    FT_Set_Transform(face, nullptr, nullptr);
    FT_Error error = FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT);
    return error ? error : FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
}

inline unsigned char Coverage(const FT_Bitmap& bitmap, int x, int y) {
    const unsigned char* row = bitmap.buffer + y * bitmap.pitch;
    if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        return (row[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
    }
    return row[x];
}

}  // namespace FontBitmap

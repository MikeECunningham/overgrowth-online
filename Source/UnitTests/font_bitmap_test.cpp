#include <Graphics/font_bitmap.h>
#include <tut/tut.hpp>
#include <string>
#include <vector>

namespace tut {
struct datafontbitmap {};
typedef test_group<datafontbitmap> tg_fontbitmap;
tg_fontbitmap test_group_fontbitmap("FontBitmap");
typedef tg_fontbitmap::object fontbitmap;

template <> template <>
void fontbitmap::test<1>() {
    set_test_name("atlas glyphs ignore a canvas face's fractional rotated transform");
    FT_Library library = nullptr;
    ensure_equals("FreeType initializes", FT_Init_FreeType(&library), 0);
    const std::string source = __FILE__;
    const size_t marker = source.find("Source/UnitTests/");
    const std::string path = source.substr(0, marker) + "Libraries/imgui-1.85/misc/fonts/Roboto-Medium.ttf";
    FT_Face face = nullptr;
    const FT_Error loaded = FT_New_Face(library, path.c_str(), 0, &face);
    if (loaded) {
        FT_Done_FreeType(library);
        ensure_equals("repository test font loads", loaded, 0);
    }
    FT_Set_Char_Size(face, 0, 28 * 64, 72, 72);
    auto pixels = [&]() {
        std::vector<unsigned char> result;
        for (unsigned y = 0; y < face->glyph->bitmap.rows; ++y) {
            for (unsigned x = 0; x < face->glyph->bitmap.width; ++x) {
                result.push_back(FontBitmap::Coverage(face->glyph->bitmap, x, y));
            }
        }
        return result;
    };
    FontBitmap::RenderAtlasGlyph(face, 'A');
    const auto expected = pixels();
    const unsigned width = face->glyph->bitmap.width, height = face->glyph->bitmap.rows;
    FT_Matrix rotated = {56756, -32768, 32768, 56756};
    FT_Vector fractional_pen = {29, 41};
    FT_Set_Transform(face, &rotated, &fractional_pen);
    FT_Load_Char(face, 'A', FT_LOAD_DEFAULT);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    const bool dirty_differs = pixels() != expected;
    FontBitmap::RenderAtlasGlyph(face, 'A');
    const bool restored = pixels() == expected && face->glyph->bitmap.width == width && face->glyph->bitmap.rows == height;
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    ensure("test provokes a different transformed glyph", dirty_differs);
    ensure("atlas raster is stable after canvas drawing", restored);
}

template <> template <>
void fontbitmap::test<2>() {
    set_test_name("bitmap row pitch includes padding and may be negative");
    unsigned char bytes[] = {1, 2, 3, 99, 99, 4, 5, 6, 99, 99};
    FT_Bitmap bitmap = {};
    bitmap.width = 3; bitmap.rows = 2; bitmap.pitch = 5;
    bitmap.buffer = bytes; bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
    ensure_equals("next row skips padding", FontBitmap::Coverage(bitmap, 1, 1), static_cast<unsigned char>(5));
    bitmap.buffer = bytes + 5; bitmap.pitch = -5;
    ensure_equals("top row of upward bitmap", FontBitmap::Coverage(bitmap, 1, 0), static_cast<unsigned char>(5));
    ensure_equals("next upward row", FontBitmap::Coverage(bitmap, 1, 1), static_cast<unsigned char>(2));
}

template <> template <>
void fontbitmap::test<3>() {
    set_test_name("monochrome glyph coverage uses bits rather than byte width");
    unsigned char bytes[] = {0x81, 0x80, 99, 99};
    FT_Bitmap bitmap = {};
    bitmap.width = 9; bitmap.rows = 1; bitmap.pitch = 4;
    bitmap.buffer = bytes; bitmap.pixel_mode = FT_PIXEL_MODE_MONO;
    ensure_equals("first set bit", FontBitmap::Coverage(bitmap, 0, 0), static_cast<unsigned char>(255));
    ensure_equals("clear bit", FontBitmap::Coverage(bitmap, 1, 0), static_cast<unsigned char>(0));
    ensure_equals("second byte", FontBitmap::Coverage(bitmap, 8, 0), static_cast<unsigned char>(255));
}

template <> template <>
void fontbitmap::test<4>() {
    set_test_name("auto-hinter shaping preserves a face's pixel size and first glyph raster");
    FT_Library library = nullptr;
    ensure_equals("FreeType initializes", FT_Init_FreeType(&library), 0);
    const std::string source = __FILE__;
    const size_t marker = source.find("Source/UnitTests/");
    const std::string path = source.substr(0, marker) + "Libraries/imgui-1.85/misc/fonts/Roboto-Medium.ttf";
    bool sizes_preserved = true;
    bool bitmaps_stable = true;
    FT_Error error = 0;
    for (int height : {12, 28, 50}) {
        FT_Face face = nullptr;
        error = FT_New_Face(library, path.c_str(), 0, &face);
        if (error) break;
        error = FT_Set_Char_Size(face, 0, height * 64, 72, 72);
        const FT_Size_Metrics expected = face->size->metrics;
        // Roboto has native hints. Force the path used by the unhinted menu
        // font, without depending on a purchased game's font assets.
        error = error ? error : FT_Load_Char(face, 'a', FT_LOAD_FORCE_AUTOHINT | FT_LOAD_RENDER);
        const FT_Size_Metrics actual = face->size->metrics;
        sizes_preserved = sizes_preserved &&
            actual.x_ppem == expected.x_ppem && actual.y_ppem == expected.y_ppem &&
            actual.x_scale == expected.x_scale && actual.y_scale == expected.y_scale &&
            actual.ascender == expected.ascender && actual.descender == expected.descender &&
            actual.height == expected.height && actual.max_advance == expected.max_advance;
        auto raster = [&]() {
            std::vector<unsigned char> pixels;
            const FT_Bitmap& bitmap = face->glyph->bitmap;
            for (unsigned y = 0; y < bitmap.rows; ++y) {
                for (unsigned x = 0; x < bitmap.width; ++x) {
                    pixels.push_back(FontBitmap::Coverage(bitmap, x, y));
                }
            }
            return pixels;
        };
        const auto first = raster();
        const unsigned width = face->glyph->bitmap.width, rows = face->glyph->bitmap.rows;
        const int top = face->glyph->bitmap_top, left = face->glyph->bitmap_left;
        error = error ? error : FT_Load_Char(face, 'a', FT_LOAD_FORCE_AUTOHINT | FT_LOAD_RENDER);
        bitmaps_stable = bitmaps_stable && first == raster() &&
            face->glyph->bitmap.width == width && face->glyph->bitmap.rows == rows &&
            face->glyph->bitmap_top == top && face->glyph->bitmap_left == left;
        FT_Done_Face(face);
        if (error) break;
    }
    FT_Done_FreeType(library);
    ensure_equals("font and glyphs load", error, 0);
    ensure("shaping does not replace the requested size with font-unit scaling", sizes_preserved);
    ensure("first atlas glyph has the same bitmap and bearings on the packing pass", bitmaps_stable);
}
}  // namespace tut

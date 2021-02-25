#include <text_shaper/directwrite_shaper.h>

#include <crispy/logger.h>

#include <algorithm>
#include <string>

// {{{ TODO: replace with libunicode
#include <codecvt>
#include <locale>
// }}}

#include <wrl/client.h>
#include <dwrite.h>
#include <dwrite_3.h>

#include <iostream> // DEBUGGING ONLY

using Microsoft::WRL::ComPtr;

using std::max;
using std::make_unique;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::wstring;

namespace text {
namespace
{
    font_weight dwFontWeight(int _weight)
    {
        switch (_weight)
        {
            case DWRITE_FONT_WEIGHT_THIN: return font_weight::thin;
            case DWRITE_FONT_WEIGHT_EXTRA_LIGHT: return font_weight::extra_light;
            case DWRITE_FONT_WEIGHT_LIGHT: return font_weight::light;
            case DWRITE_FONT_WEIGHT_SEMI_LIGHT: return font_weight::demilight;
            case DWRITE_FONT_WEIGHT_REGULAR: return font_weight::normal;
            // XXX What about font_weight::book (which does exist via fontconfig)?
            case DWRITE_FONT_WEIGHT_MEDIUM: return font_weight::medium;
            case DWRITE_FONT_WEIGHT_DEMI_BOLD: return font_weight::demibold;
            case DWRITE_FONT_WEIGHT_BOLD: return font_weight::bold;
            case DWRITE_FONT_WEIGHT_EXTRA_BOLD: return font_weight::extra_bold;
            case DWRITE_FONT_WEIGHT_BLACK: return font_weight::black;
            case DWRITE_FONT_WEIGHT_EXTRA_BLACK: return font_weight::extra_black;
            default: // TODO: the others
                break;
        }
        return font_weight::normal; // TODO: rename normal to regular
    }

    font_slant dwFontSlant(int _style)
    {
        switch (_style)
        {
            case DWRITE_FONT_STYLE_NORMAL: return font_slant::normal;
            case DWRITE_FONT_STYLE_ITALIC: return font_slant::italic;
            case DWRITE_FONT_STYLE_OBLIQUE: return font_slant::oblique;
        }
        return font_slant::normal;
    }
}

struct FontInfo
{
    font_description description;
    font_size size;

    ComPtr<IDWriteFont3> font;
    ComPtr<IDWriteFontFace5> fontFace;
};

struct directwrite_shaper::Private
{
    ComPtr<IDWriteFactory7> factory;
    vec2 dpi_;
    std::wstring userLocale;
    std::unordered_map<font_key, FontInfo> fonts;

    font_key nextFontKey;

    Private(vec2 _dpi) :
        dpi_{ _dpi }
    {
        auto hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory7),
                                      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        wchar_t locale[LOCALE_NAME_MAX_LENGTH];
        GetUserDefaultLocaleName(locale, sizeof(locale));
        userLocale = locale;
    }

    font_key create_font_key()
    {
        auto result = nextFontKey;
        nextFontKey.value++;
        return result;
    }

    int computeAverageAdvance(font_key _font)
    {
        auto constexpr firstCharIndex = UINT16{32};
        auto constexpr lastCharIndex = UINT16{127};
        auto constexpr charCount = lastCharIndex - firstCharIndex + 1;

        auto const& fontInfo = fonts.at(_font);

        UINT32 codePoints[charCount]{};
        for (UINT16 i = 0; i < charCount; i++)
            codePoints[i] = firstCharIndex + i;
        UINT16 glyphIndices[charCount]{};
        fontInfo.fontFace->GetGlyphIndicesA(codePoints, charCount, glyphIndices);

        DWRITE_GLYPH_METRICS dwGlyphMetrics[charCount]{};
        fontInfo.fontFace->GetDesignGlyphMetrics(glyphIndices, charCount, dwGlyphMetrics);

        DWRITE_FONT_METRICS dwFontMetrics{};
        fontInfo.font->GetMetrics(&dwFontMetrics);

        UINT32 maxAdvance = 0;
        for (int i = 0; i < charCount; i++)
            maxAdvance = max(maxAdvance, dwGlyphMetrics[i].advanceWidth);

        return int(ceilf(float(maxAdvance) / 64.0f));
    }
};

directwrite_shaper::directwrite_shaper(vec2 _dpi) :
    d(new Private(_dpi), [](Private* p) { delete p; })
{
}

optional<font_key> directwrite_shaper::load_font(font_description const& _description, font_size _size)
{
    debuglog().write("Loading font chain for: {}", _description);

    IDWriteFontCollection* fontCollection{};
    d->factory->GetSystemFontCollection(&fontCollection);

    // TODO: use libunicode for that (TODO: create wchar_t/char16_t converters in libunicode)
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wStringConverter;
    std::wstring familyName = wStringConverter.from_bytes(_description.familyName);

    for (UINT32 i = 0, e = fontCollection->GetFontFamilyCount(); i < e; ++i)
    {
        IDWriteFontFamily* family{};
        fontCollection->GetFontFamily(i, &family);

        IDWriteLocalizedStrings* names;
        family->GetFamilyNames(&names);

        BOOL exists = FALSE;
        unsigned index{};
        names->FindLocaleName(d->userLocale.c_str(), &index, &exists);

        wchar_t name[64];
        if (exists)
            names->GetString(index, name, _countof(name));
        else
            // TODO: there was a different way of fallback that I have a tab open on
            // one of my smartphones. Please find me.
            names->GetString(i, name, sizeof(name));

        if (familyName != name)
            continue;

        for (UINT32 k = 0, ke = family->GetFontCount(); k < ke; ++k)
        {
            ComPtr<IDWriteFont> font;
            family->GetFont(k, font.GetAddressOf());

            font_weight weight = dwFontWeight(font->GetWeight());
            if (weight != _description.weight)
                continue;

            font_slant slant = dwFontSlant(font->GetStyle());
            if (weight != _description.weight)
                continue;

            ComPtr<IDWriteFontFace> fontFace;
            font->CreateFontFace(fontFace.GetAddressOf());

            bool monospace = false;
            IDWriteFontFace5* face5;
            HRESULT hr = fontFace->QueryInterface(__uuidof(IDWriteFontFace5), (void **)&face5);
            if (SUCCEEDED(hr))
                if (!face5->IsMonospacedFont())
                    continue;

            IDWriteFont3* font3{};
            hr = font->QueryInterface(__uuidof(IDWriteFont3), (void **)&font3);

            // ComPtr<IDWriteTextFormat> textFormat;
            // d->factory->CreateTextFormat(L"Arial", NULL, DWRITE_FONT_WEIGHT_REGULAR,
            //     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            //     12.0f / 72.0f * 96.0f, L"en-us", &textFormat);

            // ComPtr<IDWriteTextLayout> layout{};
            // d->factory->CreateTextLayout("abcABC", 6, textFormat.Get(),
            //     100.0f, // maxWidth
            //     100.0f, // maxHeight
            //     layout.GetAddressOf());
            // DWRITE_LINE_METRICS lm{};
            // layout->GetLineMetrics();
            // DWRITE_TEXT_METRICS tm{};
            // layout->GetMetrics(&tm);

            auto fontInfo = FontInfo{};
            fontInfo.description = _description;
            fontInfo.size = _size;
            font.As(&fontInfo.font);
            fontFace.As(&fontInfo.fontFace);

            auto key = d->create_font_key();
            d->fonts.emplace(pair{key, move(fontInfo)});

            return key;
        }
    }
    debuglog().write("Font not found.");
    return nullopt;

#if 0
    IDWriteFontFallbackBuilder* ffb{};
    d->factory->CreateFontFallbackBuilder(&ffb);
    IDWriteFontFallback* ff;
    ffb->CreateFontFallback(&ff);

    IDWriteTextAnalyzer* textAnalyzer{};
    d->factory->CreateTextAnalyzer(&textAnalyzer);//?
    textAnalyzer->Release();

    IDWriteTextAnalysisSource *analysisSource;
    UINT32 textPosition;
    UINT32 textLength;
    IDWriteFontCollection *baseFontCollection;
    const wchar_t *baseFamilyName;
    DWRITE_FONT_WEIGHT baseWeight;
    DWRITE_FONT_STYLE baseStyle;
    DWRITE_FONT_STRETCH baseStretch;

    UINT32 mappedLength;
    IDWriteFont *mappedFont;
    FLOAT scale;
    ff->MapCharacters(analysisSource, textPosition, textLength, baseFontCollection,
                      baseFamilyName, baseWeight, baseStyle, baseStretch,
                      &mappedLength, &mappedFont, &scale);

    // DWRITE_FONT_FACE_TYPE fontFaceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    // UINT32 numberOfFiles = 1;
    // IDWriteFontFile *const *fontFiles;
    // UINT32 faceIndex = 0;
    // DWRITE_FONT_SIMULATIONS fontFaceSimulationFlags;
    // IDWriteFontFace *fontFace{};
    // d->factory->CreateFontFace();

    // DWRITE_FONT_FACE_TYPE fontFaceType;
    // UINT32 numberOfFiles;
    // IDWriteFontFile *const *fontFiles;
    // UINT32 faceIndex;
    // DWRITE_FONT_SIMULATIONS fontFaceSimulationFlags;
    // IDWriteFontFace **fontFace = nullptr;
    // d->factory->CreateFontFace(fontFaceType, numberOfFiles, fontFiles, faceIndex, fontFaceSimulationFlags, fontFace);
    printf("done\n");
    return nullopt;
#endif
}

font_metrics directwrite_shaper::metrics(font_key _key) const
{
    FontInfo const& fontInfo = d->fonts.at(_key);
    auto dwMetrics = DWRITE_FONT_METRICS{};
    fontInfo.font->GetMetrics(&dwMetrics);

    auto const lineHeight = dwMetrics.ascent + dwMetrics.descent + dwMetrics.lineGap;
    // FIXME: how to properly convert from designer font units to Pt or DIP?

    auto output = font_metrics{};
    output.line_height = lineHeight >> 6;
    output.ascender = dwMetrics.ascent >> 6;
    output.descender = dwMetrics.descent >> 6;
    output.underline_position = dwMetrics.underlinePosition >> 6;
    output.underline_thickness = dwMetrics.underlineThickness >> 6;
    output.advance = d->computeAverageAdvance(_key);

    return output;
}

void directwrite_shaper::shape(font_key _font,
                               std::u32string_view _text,
                               crispy::span<int> _clusters,
                               unicode::Script _script,
                               shape_result& _result)
{
    // IDWriteTextAnalyzer* analyzer{};
    // d->factory->CreateTextAnalyzer(&analyzer);

    // WCHAR const *textString = L""; // TODO
    // UINT32 textLength; // TODO
    // IDWriteFontFace *fontFace; // TODO: get from hashmap.at(key)
    // BOOL isSideways = FALSE;
    // BOOL isRightToLeft = FALSE;
    // DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis; // TODO: call to AnalyzeScript(...)
    // WCHAR const* localeName; // TODO: current user locale
    // IDWriteNumberSubstitution* numberSubstitution = NULL;

    // auto EnableStdLigatures = DWRITE_FONT_FEATURE{ DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, TRUE };
    // auto feature1 = DWRITE_TYPOGRAPHIC_FEATURES{
    //     { &EnableStdLigatures },
    //     1 // count
    // };
    // DWRITE_TYPOGRAPHIC_FEATURES* features[] = { &feature1 };
    // UINT32 const featureRangeLengths[] = { textLength }; // The length of each feature range, in characters. The sum of all lengths should be equal to textLength.
    // UINT32 featureRanges = 1; // the number of feature ranges

    // auto maxGlyphCount = UINT32{256};
    // auto clusterMap = new UINT16[maxGlyphCount];
    // auto textProps = new DWRITE_SHAPING_TEXT_PROPERTIES[maxGlyphCount]; // needs allocation
    // auto glyphIndices = new UINT16[maxGlyphCount];
    // auto glyphProps = new DWRITE_SHAPING_GLYPH_PROPERTIES[maxGlyphCount];
    // auto actualGlyphCount = UINT32{0};

    // analyzer->GetGlyphs(textString,
    //                     textLength,
    //                     fontFace,
    //                     isSideways,
    //                     isRightToLeft,
    //                     scriptAnalysis,
    //                     localeName, numberSubstitution,
    //                     &features/*FIXME: is this right?*/,
    //                     featureRangeLengths,
    //                     featureRanges,
    //                     maxGlyphCount,
    //                     clusterMap,
    //                     textProps,
    //                     glyphIndices,
    //                     glyphProps,
    //                     &actualGlyphCount);

}

std::optional<rasterized_glyph> directwrite_shaper::rasterize(glyph_key _glyph, render_mode _mode)
{
    // TODO: specialize IDWriteTextRenderer to render to bitmap
    IDWriteBitmapRenderTarget* renderTarget{}; // TODO
    return nullopt;
}

bool directwrite_shaper::has_color(font_key _font) const
{
    // TODO: use internal hash map to font info
    return false;
}

} // end namespace

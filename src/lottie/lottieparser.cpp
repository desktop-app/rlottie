/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "lottieparser.h"

//#define DEBUG_PARSER

// This parser implements JSON token-by-token parsing with an API that is
// more direct; we don't have to create  handler object and
// callbacks. Instead, we retrieve values from the JSON stream by calling
// GetInt(), GetDouble(), GetString() and GetBool(), traverse into structures
// by calling EnterObject() and EnterArray(), and skip over unwanted data by
// calling SkipValue(). As we know the lottie file structure this way will be
// the efficient way of parsing the file.
//
// If you aren't sure of what's next in the JSON data, you can use PeekType()
// and PeekValue() to look ahead to the next object before reading it.
//
// If you call the wrong retrieval method--e.g. GetInt when the next JSON token
// is not an int, EnterObject or EnterArray when there isn't actually an object
// or array to read--the stream parsing will end immediately and no more data
// will be delivered.
//
// After calling EnterObject, you retrieve keys via NextObjectKey() and values
// via the normal getters. When NextObjectKey() returns null, you have exited
// the object, or you can call SkipObject() to skip to the end of the object
// immediately. If you fetch the entire object (i.e. NextObjectKey() returned
// null), you should not call SkipObject().
//
// After calling EnterArray(), you must alternate between calling
// NextArrayValue() to see if the array has more data, and then retrieving
// values via the normal getters. You can call SkipArray() to skip to the end of
// the array immediately. If you fetch the entire array (i.e. NextArrayValue()
// returned null), you should not call SkipArray().
//
// This parser uses in-situ strings, so the JSON buffer will be altered during
// the parse.

#include <array>

#include "lottiemodel.h"
#include <rapidjson/document.h>

RAPIDJSON_DIAG_PUSH
#ifdef __GNUC__
RAPIDJSON_DIAG_OFF(effc++)
#endif

using namespace rapidjson;

class LookaheadParserHandler {
public:
    bool Null()
    {
        st_ = kHasNull;
        v_.SetNull();
        return true;
    }
    bool Bool(bool b)
    {
        st_ = kHasBool;
        v_.SetBool(b);
        return true;
    }
    bool Int(int i)
    {
        st_ = kHasNumber;
        v_.SetInt(i);
        return true;
    }
    bool Uint(unsigned u)
    {
        st_ = kHasNumber;
        v_.SetUint(u);
        return true;
    }
    bool Int64(int64_t i)
    {
        st_ = kHasNumber;
        v_.SetInt64(i);
        return true;
    }
    bool Uint64(int64_t u)
    {
        st_ = kHasNumber;
        v_.SetUint64(u);
        return true;
    }
    bool Double(double d)
    {
        st_ = kHasNumber;
        v_.SetDouble(d);
        return true;
    }
    bool RawNumber(const char *, SizeType, bool) { return false; }
    bool String(const char *str, SizeType length, bool)
    {
        st_ = kHasString;
        v_.SetString(str, length);
        return true;
    }
    bool StartObject()
    {
        st_ = kEnteringObject;
        return true;
    }
    bool Key(const char *str, SizeType length, bool)
    {
        st_ = kHasKey;
        v_.SetString(str, length);
        return true;
    }
    bool EndObject(SizeType)
    {
        st_ = kExitingObject;
        return true;
    }
    bool StartArray()
    {
        st_ = kEnteringArray;
        return true;
    }
    bool EndArray(SizeType)
    {
        st_ = kExitingArray;
        return true;
    }

protected:
    explicit LookaheadParserHandler(char *str);

protected:
    enum LookaheadParsingState {
        kInit,
        kError,
        kHasNull,
        kHasBool,
        kHasNumber,
        kHasString,
        kHasKey,
        kEnteringObject,
        kExitingObject,
        kEnteringArray,
        kExitingArray
    };

    Value                 v_;
    LookaheadParsingState st_;
    Reader                r_;
    InsituStringStream    ss_;

    static const int parseFlags = kParseDefaultFlags | kParseInsituFlag;
};

class LottieParserImpl : public LookaheadParserHandler {
public:
    LottieParserImpl(char *str, const char *dir_path,
                     const std::vector<std::pair<std::uint32_t, std::uint32_t>>
                         &colorReplacements, rlottie::FitzModifier fitzModifier)
        : LookaheadParserHandler(str),
          mColorReplacements(colorReplacements),
          mFitzModifier(fitzModifier),
          mDirPath(dir_path)
    {
    }
    bool VerifyType();
    bool ParseNext();
public:
    bool        EnterObject();
    bool        EnterArray();
    const char *NextObjectKey();
    bool        NextArrayValue();
    int         GetInt();
    double      GetDouble();
    const char *GetString();
    bool        GetBool();
    void        GetNull();

    void   SkipObject();
    void   SkipArray();
    void   SkipValue();
    Value *PeekValue();
    int PeekType() const;
    bool IsValid() { return st_ != kError; }

    void                  Skip(const char *key);
    LottieBlendMode       getBlendMode();
    CapStyle              getLineCap();
    JoinStyle             getLineJoin();
    FillRule              getFillRule();
    LOTTrimData::TrimType getTrimType();
    MatteType             getMatteType();
    LayerType             getLayerType();

    std::shared_ptr<LOTCompositionData> composition() const
    {
        return mComposition;
    }
    void                         parseComposition();
    void                         parseAssets(LOTCompositionData *comp);
    std::shared_ptr<LOTAsset>    parseAsset();
    void                         parseLayers(LOTCompositionData *comp);
    std::shared_ptr<LOTData>     parseLayer(bool record = false);
    void                         parseMaskProperty(LOTLayerData *layer);
    void                         parseShapesAttr(LOTLayerData *layer);
    void                         parseObject(LOTGroupData *parent);
    std::shared_ptr<LOTMaskData> parseMaskObject();
    std::shared_ptr<LOTData>     parseObjectTypeAttr();
    std::shared_ptr<LOTData>     parseGroupObject();
    std::shared_ptr<LOTData>     parseRectObject();
    std::shared_ptr<LOTData>     parseEllipseObject();
    std::shared_ptr<LOTData>     parseShapeObject();
    std::shared_ptr<LOTData>     parsePolystarObject();

    std::shared_ptr<LOTTransformData> parseTransformObject(bool ddd = false);
    std::shared_ptr<LOTData>          parseFillObject();
    std::shared_ptr<LOTData>          parseGFillObject();
    std::shared_ptr<LOTData>          parseStrokeObject();
    std::shared_ptr<LOTData>          parseGStrokeObject();
    std::shared_ptr<LOTData>          parseTrimObject();
    std::shared_ptr<LOTData>          parseReapeaterObject();

    void parseGradientProperty(LOTGradient *gradient, const char *key);

    VPointF parseInperpolatorPoint();

    void getValue(VPointF &pt);
    void getValue(float &fval);
    void getValue(LottieColor &color);
    void getValue(int &ival);
    void getValue(LottieShapeData &shape);
    void getValue(LottieGradient &gradient);
    void getValue(std::vector<VPointF> &v);
    void getValue(LOTRepeaterTransform &);

    template <typename T>
    bool parseKeyFrameValue(const char *key, LOTKeyFrameValue<T> &value);
    template <typename T>
    void parseKeyFrame(LOTAnimInfo<T> &obj);
    template <typename T>
    void parseProperty(LOTAnimatable<T> &obj);
    template <typename T>
    void parsePropertyHelper(LOTAnimatable<T> &obj);

    void parseShapeKeyFrame(LOTAnimInfo<LottieShapeData> &obj);
    void parseShapeProperty(LOTAnimatable<LottieShapeData> &obj);
    void parseDashProperty(LOTDashProperty &dash);
    
    void parseFitzColorReplacements();
    void parseFitzColorReplacement();
    
    std::shared_ptr<VInterpolator> interpolator(VPointF, VPointF, std::string);

    LottieColor toColor(const char *str);
    LottieColor applyReplacements(const LottieColor &color);

    void resolveLayerRefs();

protected:
    std::vector<std::pair<std::uint32_t, std::uint32_t>>
        mColorReplacements;
    const rlottie::FitzModifier                         mFitzModifier;
    std::unordered_map<std::string, std::shared_ptr<VInterpolator>>
                                               mInterpolatorCache;
    std::shared_ptr<LOTCompositionData>        mComposition;
    LOTCompositionData *                       compRef{nullptr};
    LOTLayerData *                             curLayerRef{nullptr};
    std::vector<std::shared_ptr<LOTLayerData>> mLayersToUpdate;
    std::string                                mDirPath;
    std::vector<LayerInfo>                     mLayerInfoList;
    void                                       SkipOut(int depth);
};

LookaheadParserHandler::LookaheadParserHandler(char *str)
    : v_(), st_(kInit), ss_(str)
{
    r_.IterativeParseInit();
}

bool LottieParserImpl::VerifyType()
{
    /* Verify the media type is lottie json.
       Could add more strict check. */
    return ParseNext();
}

bool LottieParserImpl::ParseNext()
{
    if (r_.HasParseError()) {
        st_ = kError;
        return false;
    }

    if (!r_.IterativeParseNext<parseFlags>(ss_, *this)) {
        vCritical << "Lottie file parsing error";
        st_ = kError;
        return false;
    }
    return true;
}

bool LottieParserImpl::EnterObject()
{
    if (st_ != kEnteringObject) {
        st_ = kError;
        //RAPIDJSON_ASSERT(false);
        return false;
    }

    ParseNext();
    return true;
}

bool LottieParserImpl::EnterArray()
{
    if (st_ != kEnteringArray) {
        st_ = kError;
        //RAPIDJSON_ASSERT(false);
        return false;
    }

    ParseNext();
    return true;
}

const char *LottieParserImpl::NextObjectKey()
{
    if (st_ == kHasKey) {
        const char *result = v_.GetString();
        ParseNext();
        return result;
    }

    /* SPECIAL CASE
     * The parser works with a prdefined rule that it will be only
     * while (NextObjectKey()) for each object but in case of our nested group
     * object we can call multiple time NextObjectKey() while exiting the object
     * so ignore those and don't put parser in the error state.
     * */
    if (st_ == kExitingArray || st_ == kEnteringObject) {
        // #ifdef DEBUG_PARSER
        //         vDebug<<"Object: Exiting nested loop";
        // #endif
        return nullptr;
    }

    if (st_ != kExitingObject) {
        //RAPIDJSON_ASSERT(false);
        st_ = kError;
        return nullptr;
    }

    ParseNext();
    return nullptr;
}

bool LottieParserImpl::NextArrayValue()
{
    if (st_ == kExitingArray) {
        ParseNext();
        return false;
    }

    /* SPECIAL CASE
     * same as  NextObjectKey()
     */
    if (st_ == kExitingObject) {
        return false;
    }

    if (st_ == kError || st_ == kHasKey) {
        //RAPIDJSON_ASSERT(false);
        st_ = kError;
        return false;
    }

    return true;
}

int LottieParserImpl::GetInt()
{
    if (st_ != kHasNumber || !v_.IsInt()) {
        st_ = kError;
        //RAPIDJSON_ASSERT(false);
        return 0;
    }

    int result = v_.GetInt();
    ParseNext();
    return result;
}

double LottieParserImpl::GetDouble()
{
    if (st_ != kHasNumber) {
        st_ = kError;
        //RAPIDJSON_ASSERT(false);
        return 0.;
    }

    double result = v_.GetDouble();
    ParseNext();
    return result;
}

bool LottieParserImpl::GetBool()
{
    if (st_ != kHasBool) {
        st_ = kError;
        //RAPIDJSON_ASSERT(false);
        return false;
    }

    bool result = v_.GetBool();
    ParseNext();
    return result;
}

void LottieParserImpl::GetNull()
{
    if (st_ != kHasNull) {
        st_ = kError;
        return;
    }

    ParseNext();
}

const char *LottieParserImpl::GetString()
{
    if (st_ != kHasString) {
        st_ = kError;
        //RAPIDJSON_ASSERT(false);
        return nullptr;
    }

    const char *result = v_.GetString();
    ParseNext();
    return result;
}

void LottieParserImpl::SkipOut(int depth)
{
    do {
        if (st_ == kEnteringArray || st_ == kEnteringObject) {
            ++depth;
        } else if (st_ == kExitingArray || st_ == kExitingObject) {
            --depth;
        } else if (st_ == kError) {
            //RAPIDJSON_ASSERT(false);
            return;
        }

        ParseNext();
    } while (depth > 0);
}

void LottieParserImpl::SkipValue()
{
    SkipOut(0);
}

void LottieParserImpl::SkipArray()
{
    SkipOut(1);
}

void LottieParserImpl::SkipObject()
{
    SkipOut(1);
}

Value *LottieParserImpl::PeekValue()
{
    if (st_ >= kHasNull && st_ <= kHasKey) {
        return &v_;
    }

    return nullptr;
}

// returns a rapidjson::Type, or -1 for no value (at end of
// object/array)
int LottieParserImpl::PeekType() const
{
    if (st_ >= kHasNull && st_ <= kHasKey) {
        return v_.GetType();
    }

    if (st_ == kEnteringArray) {
        return kArrayType;
    }

    if (st_ == kEnteringObject) {
        return kObjectType;
    }

    return -1;
}

void LottieParserImpl::Skip(const char * /*key*/)
{
    if (PeekType() == kArrayType) {
        EnterArray();
        SkipArray();
    } else if (PeekType() == kObjectType) {
        EnterObject();
        SkipObject();
    } else {
        SkipValue();
    }
}

LottieBlendMode LottieParserImpl::getBlendMode()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    LottieBlendMode mode = LottieBlendMode::Normal;

    switch (GetInt()) {
    case 1:
        mode = LottieBlendMode::Multiply;
        break;
    case 2:
        mode = LottieBlendMode::Screen;
        break;
    case 3:
        mode = LottieBlendMode::OverLay;
        break;
    default:
        break;
    }
    return mode;
}

void LottieParserImpl::resolveLayerRefs()
{
    for (const auto &i : mLayersToUpdate) {
        LOTLayerData *layer = i.get();
        auto          search = compRef->mAssets.find(layer->extra()->mPreCompRefId);
        if (search != compRef->mAssets.end()) {
            if (layer->mLayerType == LayerType::Image) {
                layer->extra()->mAsset = search->second;
            } else if (layer->mLayerType == LayerType::Precomp) {
                layer->mChildren = search->second->mLayers;
                layer->setStatic(layer->isStatic() &&
                                 search->second->isStatic());
            }
        }
    }
}

void LottieParserImpl::parseComposition()
{
    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    EnterObject();
    std::shared_ptr<LOTCompositionData> sharedComposition =
        std::make_shared<LOTCompositionData>();
    LOTCompositionData *comp = sharedComposition.get();
    compRef = comp;
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "v")) {
            RAPIDJSON_ASSERT(PeekType() == kStringType);
            comp->mVersion = std::string(GetString());
        } else if (0 == strcmp(key, "w")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            comp->mSize.setWidth(GetInt());
        } else if (0 == strcmp(key, "h")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            comp->mSize.setHeight(GetInt());
        } else if (0 == strcmp(key, "ip")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            comp->mStartFrame = GetDouble();
        } else if (0 == strcmp(key, "op")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            comp->mEndFrame = GetDouble();
        } else if (0 == strcmp(key, "fr")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            comp->mFrameRate = GetDouble();
        } else if (0 == strcmp(key, "assets")) {
            parseAssets(comp);
        } else if (0 == strcmp(key, "layers")) {
            parseLayers(comp);
        } else if (0 == strcmp(key, "fitz")) {
            parseFitzColorReplacements();
        } else {
#ifdef DEBUG_PARSER
            vWarning << "Composition Attribute Skipped : " << key;
#endif
            Skip(key);
        }
    }

    if (comp->mVersion.empty() || !comp->mRootLayer) {
        // don't have a valid bodymovin header
        return;
    }

    resolveLayerRefs();
    comp->setStatic(comp->mRootLayer->isStatic());
    comp->mRootLayer->mInFrame = comp->mStartFrame;
    comp->mRootLayer->mOutFrame = comp->mEndFrame;

    comp->mLayerInfoList = std::move(mLayerInfoList);

    mComposition = sharedComposition;
}

void LottieParserImpl::parseAssets(LOTCompositionData *composition)
{
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        std::shared_ptr<LOTAsset> asset = parseAsset();
        composition->mAssets[asset->mRefId] = asset;
    }
    // update the precomp layers with the actual layer object
}

void LottieParserImpl::parseFitzColorReplacements()
{
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        parseFitzColorReplacement();
    }
}

void LottieParserImpl::parseFitzColorReplacement()
{
    uint32_t original = 0;
    uint32_t type12 = 0;
    uint32_t type3 = 0;
    uint32_t type4 = 0;
    uint32_t type5 = 0;
    uint32_t type6 = 0;
    
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "o")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            original = GetInt();
        } else if (0 == strcmp(key, "f12")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            type12 = GetInt();
        } else if (0 == strcmp(key, "f3")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            type3 = GetInt();
        } else if (0 == strcmp(key, "f4")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            type4 = GetInt();
        } else if (0 == strcmp(key, "f5")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            type5 = GetInt();
        } else if (0 == strcmp(key, "f6")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            type6 = GetInt();
        } else {
            Skip(key);
        }
    }
    
    switch (mFitzModifier) {
        case rlottie::FitzModifier::None:
            break;
        case rlottie::FitzModifier::Type12:
            mColorReplacements.push_back(std::pair<uint32_t, uint32_t>(original, type12));
            break;
        case rlottie::FitzModifier::Type3:
            mColorReplacements.push_back(std::pair<uint32_t, uint32_t>(original, type3));
            break;
        case rlottie::FitzModifier::Type4:
            mColorReplacements.push_back(std::pair<uint32_t, uint32_t>(original, type4));
            break;
        case rlottie::FitzModifier::Type5:
            mColorReplacements.push_back(std::pair<uint32_t, uint32_t>(original, type5));
            break;
        case rlottie::FitzModifier::Type6:
            mColorReplacements.push_back(std::pair<uint32_t, uint32_t>(original, type6));
            break;
    }
}

static constexpr const unsigned char B64index[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  62, 63, 62, 62, 63, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,
    7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 0,  0,  0,  0,  63, 0,  26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
    37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51};

std::string b64decode(const char *data, const size_t len)
{
    auto p = reinterpret_cast<const unsigned char *>(data);
    int            pad = len > 0 && (len % 4 || p[len - 1] == '=');
    const size_t   L = ((len + 3) / 4 - pad) * 4;
    std::string    str(L / 4 * 3 + pad, '\0');

    for (size_t i = 0, j = 0; i < L; i += 4) {
        int n = B64index[p[i]] << 18 | B64index[p[i + 1]] << 12 |
                B64index[p[i + 2]] << 6 | B64index[p[i + 3]];
        str[j++] = n >> 16;
        str[j++] = n >> 8 & 0xFF;
        str[j++] = n & 0xFF;
    }
    if (pad) {
        int n = B64index[p[L]] << 18 | B64index[p[L + 1]] << 12;
        str[str.size() - 1] = n >> 16;

        if (len > L + 2 && p[L + 2] != '=') {
            n |= B64index[p[L + 2]] << 6;
            str.push_back(n >> 8 & 0xFF);
        }
    }
    return str;
}

static std::string convertFromBase64(const std::string &str)
{
    // usual header look like "data:image/png;base64,"
    // so need to skip till ','.
    size_t startIndex = str.find(",", 0);
    startIndex += 1;  // skip ","
    size_t length = str.length() - startIndex;

    const char *b64Data = str.c_str() + startIndex;

    return b64decode(b64Data, length);
}

/*
 *  std::to_string() function is missing in VS2017
 *  so this is workaround for windows build
 */
#include <sstream>
template<class T>
static std::string toString(const T &value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/layers/shape.json
 *
 */
std::shared_ptr<LOTAsset> LottieParserImpl::parseAsset()
{
    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    std::shared_ptr<LOTAsset> sharedAsset = std::make_shared<LOTAsset>();
    LOTAsset *                asset = sharedAsset.get();
    std::string               filename;
    std::string               relativePath;
    bool                      embededResource = false;
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "w")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            asset->mWidth = GetInt();
        } else if (0 == strcmp(key, "h")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            asset->mHeight = GetInt();
        } else if (0 == strcmp(key, "p")) { /* image name */
            asset->mAssetType = LOTAsset::Type::Image;
            RAPIDJSON_ASSERT(PeekType() == kStringType);
            filename = std::string(GetString());
        } else if (0 == strcmp(key, "u")) { /* relative image path */
            RAPIDJSON_ASSERT(PeekType() == kStringType);
            relativePath = std::string(GetString());
        } else if (0 == strcmp(key, "e")) { /* relative image path */
            embededResource = GetInt();
        } else if (0 == strcmp(key, "id")) { /* reference id*/
            if (PeekType() == kStringType) {
                asset->mRefId = std::string(GetString());
            } else {
                RAPIDJSON_ASSERT(PeekType() == kNumberType);
                asset->mRefId = toString(GetInt());
            }
        } else if (0 == strcmp(key, "layers")) {
            asset->mAssetType = LOTAsset::Type::Precomp;
            RAPIDJSON_ASSERT(PeekType() == kArrayType);
            EnterArray();
            bool staticFlag = true;
            while (NextArrayValue()) {
                std::shared_ptr<LOTData> layer = parseLayer();
                if (layer) {
                    staticFlag = staticFlag && layer->isStatic();
                    asset->mLayers.push_back(layer);
                }
            }
            asset->setStatic(staticFlag);
        } else {
#ifdef DEBUG_PARSER
            vWarning << "Asset Attribute Skipped : " << key;
#endif
            Skip(key);
        }
    }

    if (asset->mAssetType == LOTAsset::Type::Image) {
        if (embededResource) {
            // embeder resource should start with "data:"
            if (filename.compare(0, 5, "data:") == 0) {
                asset->loadImageData(convertFromBase64(filename));
            }
        } else {
            asset->loadImagePath(mDirPath + relativePath + filename);
        }
    }

    return sharedAsset;
}

void LottieParserImpl::parseLayers(LOTCompositionData *comp)
{
    comp->mRootLayer = std::make_shared<LOTLayerData>();
    comp->mRootLayer->mLayerType = LayerType::Precomp;
    comp->mRootLayer->mName = std::string("__");
    bool staticFlag = true;
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        std::shared_ptr<LOTData> layer = parseLayer(true);
        if (layer) {
            staticFlag = staticFlag && layer->isStatic();
            comp->mRootLayer->mChildren.push_back(layer);
        }
    }
    comp->mRootLayer->setStatic(staticFlag);
}

LottieColor LottieParserImpl::toColor(const char *str)
{
    LottieColor color;
    auto len = strlen(str);

    // some resource has empty color string
    // return a default color for those cases.
    if (len != 7 || str[0] != '#') return color;

    char tmp[3] = {'\0', '\0', '\0'};
    tmp[0] = str[1];
    tmp[1] = str[2];
    color.r = std::strtol(tmp, nullptr, 16) / 255.0f;

    tmp[0] = str[3];
    tmp[1] = str[4];
    color.g = std::strtol(tmp, nullptr, 16) / 255.0f;

    tmp[0] = str[5];
    tmp[1] = str[6];
    color.b = std::strtol(tmp, nullptr, 16) / 255.0f;

    return applyReplacements(color);
}

LottieColor LottieParserImpl::applyReplacements(const LottieColor &color)
{
    if (mColorReplacements.empty()) {
        return color;
    }
    const auto convert = [](float value) {
        return std::uint32_t(
            std::round(std::min(std::max(value, 0.f), 1.f) * 255.));
    };
    const auto part = [](std::uint32_t value, int shift) {
        return float((value >> shift) & 0xFFU) / 255.f;
    };
    const auto converted =
        convert(color.b) | (convert(color.g) << 8) | (convert(color.r) << 16);
    for (const auto &pair : mColorReplacements) {
        const auto key = pair.first;
        const auto value = pair.second;
        if (key == converted) {
            return LottieColor(part(value, 16), part(value, 8), part(value, 0));
        }
    }
    return color;
}

MatteType LottieParserImpl::getMatteType()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    switch (GetInt()) {
    case 1:
        return MatteType::Alpha;
        break;
    case 2:
        return MatteType::AlphaInv;
        break;
    case 3:
        return MatteType::Luma;
        break;
    case 4:
        return MatteType::LumaInv;
        break;
    default:
        return MatteType::None;
        break;
    }
}

LayerType LottieParserImpl::getLayerType()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    switch (GetInt()) {
    case 0:
        return LayerType::Precomp;
        break;
    case 1:
        return LayerType::Solid;
        break;
    case 2:
        return LayerType::Image;
        break;
    case 3:
        return LayerType::Null;
        break;
    case 4:
        return LayerType::Shape;
        break;
    case 5:
        return LayerType::Text;
        break;
    default:
        return LayerType::Null;
        break;
    }
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/layers/shape.json
 *
 */
std::shared_ptr<LOTData> LottieParserImpl::parseLayer(bool record)
{
    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    std::shared_ptr<LOTLayerData> sharedLayer =
        std::make_shared<LOTLayerData>();
    LOTLayerData *layer = sharedLayer.get();
    curLayerRef = layer;
    bool ddd = true;
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "ty")) { /* Type of layer*/
            layer->mLayerType = getLayerType();
        } else if (0 == strcmp(key, "nm")) { /*Layer name*/
            RAPIDJSON_ASSERT(PeekType() == kStringType);
            layer->mName = GetString();
        } else if (0 == strcmp(key, "ind")) { /*Layer index in AE. Used for
                                                 parenting and expressions.*/
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            layer->mId = GetInt();
        } else if (0 == strcmp(key, "ddd")) { /*3d layer */
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            ddd = GetInt();
        } else if (0 ==
                   strcmp(key,
                          "parent")) { /*Layer Parent. Uses "ind" of parent.*/
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            layer->mParentId = GetInt();
        } else if (0 == strcmp(key, "refId")) { /*preComp Layer reference id*/
            RAPIDJSON_ASSERT(PeekType() == kStringType);
            layer->extra()->mPreCompRefId = std::string(GetString());
            layer->mHasGradient = true;
            mLayersToUpdate.push_back(sharedLayer);
        } else if (0 == strcmp(key, "sr")) {  // "Layer Time Stretching"
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            layer->mTimeStreatch = GetDouble();
        } else if (0 == strcmp(key, "tm")) {  // time remapping
            parseProperty(layer->extra()->mTimeRemap);
        } else if (0 == strcmp(key, "ip")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            layer->mInFrame = std::lround(GetDouble());
        } else if (0 == strcmp(key, "op")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            layer->mOutFrame = std::lround(GetDouble());
        } else if (0 == strcmp(key, "st")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            layer->mStartFrame = GetDouble();
        } else if (0 == strcmp(key, "bm")) {
            layer->mBlendMode = getBlendMode();
        } else if (0 == strcmp(key, "ks")) {
            if (PeekType() == kObjectType) {
                EnterObject();
                layer->mTransform = parseTransformObject(ddd);
            }
        } else if (0 == strcmp(key, "shapes")) {
            parseShapesAttr(layer);
        } else if (0 == strcmp(key, "w")) {
            layer->mLayerSize.setWidth(GetInt());
        } else if (0 == strcmp(key, "h")) {
            layer->mLayerSize.setHeight(GetInt());
        } else if (0 == strcmp(key, "sw")) {
            layer->mLayerSize.setWidth(GetInt());
        } else if (0 == strcmp(key, "sh")) {
            layer->mLayerSize.setHeight(GetInt());
        } else if (0 == strcmp(key, "sc")) {
            layer->extra()->mSolidColor = toColor(GetString());
        } else if (0 == strcmp(key, "tt")) {
            layer->mMatteType = getMatteType();
        } else if (0 == strcmp(key, "hasMask")) {
            layer->mHasMask = GetBool();
        } else if (0 == strcmp(key, "masksProperties")) {
            parseMaskProperty(layer);
        } else if (0 == strcmp(key, "ao")) {
            layer->mAutoOrient = GetInt();
        } else if (0 == strcmp(key, "hd")) {
            layer->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vWarning << "Layer Attribute Skipped : " << key;
#endif
            Skip(key);
        }
    }

    if (!layer->mTransform) {
        // not a valid layer
        return nullptr;
    }

    if (layer->mExtra) layer->mExtra->mCompRef = compRef;

    if (layer->hidden()) {
        // if layer is hidden, only data that is usefull is its
        // transform matrix(when it is a parent of some other layer)
        // so force it to be a Null Layer and release all resource.
        layer->setStatic(layer->mTransform->isStatic());
        layer->mLayerType = LayerType::Null;
        layer->mChildren = {};
        return sharedLayer;
    }

    // update the static property of layer
    bool staticFlag = true;
    for (const auto &child : layer->mChildren) {
        staticFlag &= child.get()->isStatic();
    }

    if (layer->hasMask() && layer->mExtra) {
        for (const auto &mask : layer->mExtra->mMasks) {
            staticFlag &= mask->isStatic();
        }
    }

    layer->setStatic(staticFlag && layer->mTransform->isStatic());

    if (record) {
        mLayerInfoList.push_back(
            LayerInfo(layer->mName, layer->mInFrame, layer->mOutFrame));
    }
    return sharedLayer;
}

void LottieParserImpl::parseMaskProperty(LOTLayerData *layer)
{
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        layer->extra()->mMasks.push_back(parseMaskObject());
    }
}

std::shared_ptr<LOTMaskData> LottieParserImpl::parseMaskObject()
{
    std::shared_ptr<LOTMaskData> sharedMask = std::make_shared<LOTMaskData>();
    LOTMaskData *                obj = sharedMask.get();

    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "inv")) {
            obj->mInv = GetBool();
        } else if (0 == strcmp(key, "mode")) {
            const char *str = GetString();
            if (!str) {
                obj->mMode = LOTMaskData::Mode::None;
                continue;
            }
            switch (str[0]) {
            case 'n':
                obj->mMode = LOTMaskData::Mode::None;
                break;
            case 'a':
                obj->mMode = LOTMaskData::Mode::Add;
                break;
            case 's':
                obj->mMode = LOTMaskData::Mode::Substarct;
                break;
            case 'i':
                obj->mMode = LOTMaskData::Mode::Intersect;
                break;
            case 'f':
                obj->mMode = LOTMaskData::Mode::Difference;
                break;
            default:
                obj->mMode = LOTMaskData::Mode::None;
                break;
            }
        } else if (0 == strcmp(key, "pt")) {
            parseShapeProperty(obj->mShape);
        } else if (0 == strcmp(key, "o")) {
            parseProperty(obj->mOpacity);
        } else {
            Skip(key);
        }
    }
    obj->mIsStatic = obj->mShape.isStatic() && obj->mOpacity.isStatic();
    return sharedMask;
}

void LottieParserImpl::parseShapesAttr(LOTLayerData *layer)
{
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        parseObject(layer);
    }
}

std::shared_ptr<LOTData> LottieParserImpl::parseObjectTypeAttr()
{
    RAPIDJSON_ASSERT(PeekType() == kStringType);
    const char *type = GetString();
    if (0 == strcmp(type, "gr")) {
        return parseGroupObject();
    } else if (0 == strcmp(type, "rc")) {
        return parseRectObject();
    } else if (0 == strcmp(type, "el")) {
        return parseEllipseObject();
    } else if (0 == strcmp(type, "tr")) {
        return parseTransformObject();
    } else if (0 == strcmp(type, "fl")) {
        return parseFillObject();
    } else if (0 == strcmp(type, "st")) {
        return parseStrokeObject();
    } else if (0 == strcmp(type, "gf")) {
        curLayerRef->mHasGradient = true;
        return parseGFillObject();
    } else if (0 == strcmp(type, "gs")) {
        curLayerRef->mHasGradient = true;
        return parseGStrokeObject();
    } else if (0 == strcmp(type, "sh")) {
        return parseShapeObject();
    } else if (0 == strcmp(type, "sr")) {
        return parsePolystarObject();
    } else if (0 == strcmp(type, "tm")) {
        curLayerRef->mHasPathOperator = true;
        return parseTrimObject();
    } else if (0 == strcmp(type, "rp")) {
        curLayerRef->mHasRepeater = true;
        return parseReapeaterObject();
    } else if (0 == strcmp(type, "mm")) {
        vWarning << "Merge Path is not supported yet";
        return nullptr;
    } else {
#ifdef DEBUG_PARSER
        vDebug << "The Object Type not yet handled = " << type;
#endif
        return nullptr;
    }
}

void LottieParserImpl::parseObject(LOTGroupData *parent)
{
    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "ty")) {
            auto child = parseObjectTypeAttr();
            if (child && !child->hidden()) parent->mChildren.push_back(child);
        } else {
            Skip(key);
        }
    }
}

std::shared_ptr<LOTData> LottieParserImpl::parseGroupObject()
{
    std::shared_ptr<LOTShapeGroupData> sharedGroup =
        std::make_shared<LOTShapeGroupData>();

    LOTShapeGroupData *group = sharedGroup.get();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            group->mName = GetString();
        } else if (0 == strcmp(key, "it")) {
            RAPIDJSON_ASSERT(PeekType() == kArrayType);
            EnterArray();
            while (NextArrayValue()) {
                RAPIDJSON_ASSERT(PeekType() == kObjectType);
                parseObject(group);
            }
            if (!group->mChildren.empty()
                && group->mChildren.back()->mType == LOTData::Type::Transform) {
                group->mTransform = std::static_pointer_cast<LOTTransformData>(
                    group->mChildren.back());
                group->mChildren.pop_back();
            }
        } else {
            Skip(key);
        }
    }
    bool staticFlag = true;
    for (const auto &child : group->mChildren) {
        staticFlag &= child.get()->isStatic();
    }

    if (group->mTransform) {
        group->setStatic(staticFlag && group->mTransform->isStatic());
    }

    return sharedGroup;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/rect.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseRectObject()
{
    std::shared_ptr<LOTRectData> sharedRect = std::make_shared<LOTRectData>();
    LOTRectData *                obj = sharedRect.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "p")) {
            parseProperty(obj->mPos);
        } else if (0 == strcmp(key, "s")) {
            parseProperty(obj->mSize);
        } else if (0 == strcmp(key, "r")) {
            parseProperty(obj->mRound);
        } else if (0 == strcmp(key, "d")) {
            obj->mDirection = GetInt();
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
            Skip(key);
        }
    }
    obj->setStatic(obj->mPos.isStatic() && obj->mSize.isStatic() &&
                   obj->mRound.isStatic());
    return sharedRect;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/ellipse.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseEllipseObject()
{
    std::shared_ptr<LOTEllipseData> sharedEllipse =
        std::make_shared<LOTEllipseData>();
    LOTEllipseData *obj = sharedEllipse.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "p")) {
            parseProperty(obj->mPos);
        } else if (0 == strcmp(key, "s")) {
            parseProperty(obj->mSize);
        } else if (0 == strcmp(key, "d")) {
            obj->mDirection = GetInt();
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
            Skip(key);
        }
    }
    obj->setStatic(obj->mPos.isStatic() && obj->mSize.isStatic());
    return sharedEllipse;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/shape.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseShapeObject()
{
    std::shared_ptr<LOTShapeData> sharedShape =
        std::make_shared<LOTShapeData>();
    LOTShapeData *obj = sharedShape.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "ks")) {
            parseShapeProperty(obj->mShape);
        } else if (0 == strcmp(key, "d")) {
            obj->mDirection = GetInt();
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vDebug << "Shape property ignored :" << key;
#endif
            Skip(key);
        }
    }
    obj->setStatic(obj->mShape.isStatic());

    return sharedShape;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/star.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parsePolystarObject()
{
    std::shared_ptr<LOTPolystarData> sharedPolystar =
        std::make_shared<LOTPolystarData>();
    LOTPolystarData *obj = sharedPolystar.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "p")) {
            parseProperty(obj->mPos);
        } else if (0 == strcmp(key, "pt")) {
            parseProperty(obj->mPointCount);
        } else if (0 == strcmp(key, "ir")) {
            parseProperty(obj->mInnerRadius);
        } else if (0 == strcmp(key, "is")) {
            parseProperty(obj->mInnerRoundness);
        } else if (0 == strcmp(key, "or")) {
            parseProperty(obj->mOuterRadius);
        } else if (0 == strcmp(key, "os")) {
            parseProperty(obj->mOuterRoundness);
        } else if (0 == strcmp(key, "r")) {
            parseProperty(obj->mRotation);
        } else if (0 == strcmp(key, "sy")) {
            int starType = GetInt();
            if (starType == 1) obj->mPolyType = LOTPolystarData::PolyType::Star;
            if (starType == 2) obj->mPolyType = LOTPolystarData::PolyType::Polygon;
        } else if (0 == strcmp(key, "d")) {
            obj->mDirection = GetInt();
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vDebug << "Polystar property ignored :" << key;
#endif
            Skip(key);
        }
    }
    obj->setStatic(
        obj->mPos.isStatic() && obj->mPointCount.isStatic() &&
        obj->mInnerRadius.isStatic() && obj->mInnerRoundness.isStatic() &&
        obj->mOuterRadius.isStatic() && obj->mOuterRoundness.isStatic() &&
        obj->mRotation.isStatic());

    return sharedPolystar;
}

LOTTrimData::TrimType LottieParserImpl::getTrimType()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    switch (GetInt()) {
    case 1:
        return LOTTrimData::TrimType::Simultaneously;
        break;
    case 2:
        return LOTTrimData::TrimType::Individually;
        break;
    default:
        RAPIDJSON_ASSERT(0);
        return LOTTrimData::TrimType::Simultaneously;
        break;
    }
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/trim.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseTrimObject()
{
    std::shared_ptr<LOTTrimData> sharedTrim = std::make_shared<LOTTrimData>();
    LOTTrimData *                obj = sharedTrim.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "s")) {
            parseProperty(obj->mStart);
        } else if (0 == strcmp(key, "e")) {
            parseProperty(obj->mEnd);
        } else if (0 == strcmp(key, "o")) {
            parseProperty(obj->mOffset);
        } else if (0 == strcmp(key, "m")) {
            obj->mTrimType = getTrimType();
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vDebug << "Trim property ignored :" << key;
#endif
            Skip(key);
        }
    }
    obj->setStatic(obj->mStart.isStatic() && obj->mEnd.isStatic() &&
                   obj->mOffset.isStatic());
    return sharedTrim;
}

void LottieParserImpl::getValue(LOTRepeaterTransform &obj)
{
    EnterObject();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "a")) {
            parseProperty(obj.mAnchor);
        } else if (0 == strcmp(key, "p")) {
            parseProperty(obj.mPosition);
        } else if (0 == strcmp(key, "r")) {
            parseProperty(obj.mRotation);
        } else if (0 == strcmp(key, "s")) {
            parseProperty(obj.mScale);
        } else if (0 == strcmp(key, "so")) {
            parseProperty(obj.mStartOpacity);
        } else if (0 == strcmp(key, "eo")) {
            parseProperty(obj.mEndOpacity);
        } else {
            Skip(key);
        }
    }
}

std::shared_ptr<LOTData> LottieParserImpl::parseReapeaterObject()
{
    std::shared_ptr<LOTRepeaterData> sharedRepeater =
        std::make_shared<LOTRepeaterData>();
    LOTRepeaterData *obj = sharedRepeater.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "c")) {
            parseProperty(obj->mCopies);
            float maxCopy = 0.0;
            if (!obj->mCopies.isStatic()) {
                for (auto &keyFrame : obj->mCopies.animation().mKeyFrames) {
                    if (maxCopy < keyFrame.mValue.mStartValue)
                        maxCopy = keyFrame.mValue.mStartValue;
                    if (maxCopy < keyFrame.mValue.mEndValue)
                        maxCopy = keyFrame.mValue.mEndValue;
                }
            } else {
                maxCopy = obj->mCopies.value();
            }
            obj->mMaxCopies = maxCopy;
        } else if (0 == strcmp(key, "o")) {
            parseProperty(obj->mOffset);
        } else if (0 == strcmp(key, "tr")) {
            getValue(obj->mTransform);
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vDebug << "Repeater property ignored :" << key;
#endif
            Skip(key);
        }
    }
    obj->setStatic(obj->mCopies.isStatic() && obj->mOffset.isStatic() &&
                   obj->mTransform.isStatic());

    return sharedRepeater;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/transform.json
 */
std::shared_ptr<LOTTransformData> LottieParserImpl::parseTransformObject(
    bool ddd)
{
    std::shared_ptr<LOTTransformData> sharedTransform =
        std::make_shared<LOTTransformData>();

    auto obj = std::make_unique<TransformData>();
    if (ddd) {
        obj->createExtraData();
        obj->mExtra->m3DData = true;
    }

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            sharedTransform->mName = GetString();
        } else if (0 == strcmp(key, "a")) {
            parseProperty(obj->mAnchor);
        } else if (0 == strcmp(key, "p")) {
            EnterObject();
            bool separate = false;
            while (const char *key = NextObjectKey()) {
                if (0 == strcmp(key, "k")) {
                    parsePropertyHelper(obj->mPosition);
                } else if (0 == strcmp(key, "s")) {
                    obj->createExtraData();
                    obj->mExtra->mSeparate = GetBool();
                    separate = true;
                } else if (separate && (0 == strcmp(key, "x"))) {
                    parseProperty(obj->mExtra->mSeparateX);
                } else if (separate && (0 == strcmp(key, "y"))) {
                    parseProperty(obj->mExtra->mSeparateY);
                } else {
                    Skip(key);
                }
            }
        } else if (0 == strcmp(key, "r")) {
            parseProperty(obj->mRotation);
        } else if (0 == strcmp(key, "s")) {
            parseProperty(obj->mScale);
        } else if (0 == strcmp(key, "o")) {
            parseProperty(obj->mOpacity);
        } else if (0 == strcmp(key, "hd")) {
            sharedTransform->mHidden = GetBool();
        } else if (0 == strcmp(key, "rx")) {
            if (!obj->mExtra) {
                return sharedTransform;
            }
            parseProperty(obj->mExtra->m3DRx);
        } else if (0 == strcmp(key, "ry")) {
            if (!obj->mExtra) {
                return sharedTransform;
            }
            parseProperty(obj->mExtra->m3DRy);
        } else if (0 == strcmp(key, "rz")) {
            if (!obj->mExtra) {
                return sharedTransform;
            }
            parseProperty(obj->mExtra->m3DRz);
        } else {
            Skip(key);
        }
    }
    bool isStatic = obj->mAnchor.isStatic() && obj->mPosition.isStatic() &&
                    obj->mRotation.isStatic() && obj->mScale.isStatic() &&
                    obj->mOpacity.isStatic();
    if (obj->mExtra) {
        isStatic = isStatic &&
                   obj->mExtra->m3DRx.isStatic() &&
                   obj->mExtra->m3DRy.isStatic() &&
                   obj->mExtra->m3DRz.isStatic() &&
                   obj->mExtra->mSeparateX.isStatic() &&
                   obj->mExtra->mSeparateY.isStatic();
    }

    sharedTransform->set(std::move(obj), isStatic);

    return sharedTransform;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/fill.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseFillObject()
{
    std::shared_ptr<LOTFillData> sharedFill = std::make_shared<LOTFillData>();
    LOTFillData *                obj = sharedFill.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "c")) {
            parseProperty(obj->mColor);
        } else if (0 == strcmp(key, "o")) {
            parseProperty(obj->mOpacity);
        } else if (0 == strcmp(key, "fillEnabled")) {
            obj->mEnabled = GetBool();
        } else if (0 == strcmp(key, "r")) {
            obj->mFillRule = getFillRule();
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vWarning << "Fill property skipped = " << key;
#endif
            Skip(key);
        }
    }
    obj->setStatic(obj->mColor.isStatic() && obj->mOpacity.isStatic());

    return sharedFill;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/helpers/lineCap.json
 */
CapStyle LottieParserImpl::getLineCap()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    switch (GetInt()) {
    case 1:
        return CapStyle::Flat;
        break;
    case 2:
        return CapStyle::Round;
        break;
    default:
        return CapStyle::Square;
        break;
    }
}

FillRule LottieParserImpl::getFillRule()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    switch (GetInt()) {
    case 1:
        return FillRule::Winding;
        break;
    case 2:
        return FillRule::EvenOdd;
        break;
    default:
        return FillRule::Winding;
        break;
    }
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/helpers/lineJoin.json
 */
JoinStyle LottieParserImpl::getLineJoin()
{
    RAPIDJSON_ASSERT(PeekType() == kNumberType);
    switch (GetInt()) {
    case 1:
        return JoinStyle::Miter;
        break;
    case 2:
        return JoinStyle::Round;
        break;
    default:
        return JoinStyle::Bevel;
        break;
    }
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/stroke.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseStrokeObject()
{
    std::shared_ptr<LOTStrokeData> sharedStroke =
        std::make_shared<LOTStrokeData>();
    LOTStrokeData *obj = sharedStroke.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "c")) {
            parseProperty(obj->mColor);
        } else if (0 == strcmp(key, "o")) {
            parseProperty(obj->mOpacity);
        } else if (0 == strcmp(key, "w")) {
            parseProperty(obj->mWidth);
        } else if (0 == strcmp(key, "fillEnabled")) {
            obj->mEnabled = GetBool();
        } else if (0 == strcmp(key, "lc")) {
            obj->mCapStyle = getLineCap();
        } else if (0 == strcmp(key, "lj")) {
            obj->mJoinStyle = getLineJoin();
        } else if (0 == strcmp(key, "ml")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            obj->mMiterLimit = GetDouble();
        } else if (0 == strcmp(key, "d")) {
            parseDashProperty(obj->mDash);
        } else if (0 == strcmp(key, "hd")) {
            obj->mHidden = GetBool();
        } else {
#ifdef DEBUG_PARSER
            vWarning << "Stroke property skipped = " << key;
#endif
            Skip(key);
        }
    }
    obj->setStatic(obj->mColor.isStatic() && obj->mOpacity.isStatic() &&
                   obj->mWidth.isStatic() && obj->mDash.isStatic());
    return sharedStroke;
}

void LottieParserImpl::parseGradientProperty(LOTGradient *obj, const char *key)
{
    if (0 == strcmp(key, "t")) {
        RAPIDJSON_ASSERT(PeekType() == kNumberType);
        obj->mGradientType = GetInt();
    } else if (0 == strcmp(key, "o")) {
        parseProperty(obj->mOpacity);
    } else if (0 == strcmp(key, "s")) {
        parseProperty(obj->mStartPoint);
    } else if (0 == strcmp(key, "e")) {
        parseProperty(obj->mEndPoint);
    } else if (0 == strcmp(key, "h")) {
        parseProperty(obj->mHighlightLength);
    } else if (0 == strcmp(key, "a")) {
        parseProperty(obj->mHighlightAngle);
    } else if (0 == strcmp(key, "g")) {
        EnterObject();
        while (const char *key = NextObjectKey()) {
            if (0 == strcmp(key, "k")) {
                parseProperty(obj->mGradient);
            } else if (0 == strcmp(key, "p")) {
                obj->mColorPoints = GetInt();
            } else {
                Skip(nullptr);
            }
        }
    } else if (0 == strcmp(key, "hd")) {
        obj->mHidden = GetBool();
    } else {
#ifdef DEBUG_PARSER
        vWarning << "Gradient property skipped = " << key;
#endif
        Skip(key);
    }
    obj->setStatic(
        obj->mOpacity.isStatic() && obj->mStartPoint.isStatic() &&
        obj->mEndPoint.isStatic() && obj->mHighlightAngle.isStatic() &&
        obj->mHighlightLength.isStatic() && obj->mGradient.isStatic());
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/gfill.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseGFillObject()
{
    std::shared_ptr<LOTGFillData> sharedGFill =
        std::make_shared<LOTGFillData>();
    LOTGFillData *obj = sharedGFill.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "r")) {
            obj->mFillRule = getFillRule();
        } else {
            parseGradientProperty(obj, key);
        }
    }
    return sharedGFill;
}

void LottieParserImpl::parseDashProperty(LOTDashProperty &dash)
{
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        RAPIDJSON_ASSERT(PeekType() == kObjectType);
        EnterObject();
        while (const char *key = NextObjectKey()) {
            if (0 == strcmp(key, "v")) {
                dash.mData.emplace_back();
                parseProperty(dash.mData.back());
            } else {
                Skip(key);
            }
        }
    }
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/shapes/gstroke.json
 */
std::shared_ptr<LOTData> LottieParserImpl::parseGStrokeObject()
{
    std::shared_ptr<LOTGStrokeData> sharedGStroke =
        std::make_shared<LOTGStrokeData>();
    LOTGStrokeData *obj = sharedGStroke.get();

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "nm")) {
            obj->mName = GetString();
        } else if (0 == strcmp(key, "w")) {
            parseProperty(obj->mWidth);
        } else if (0 == strcmp(key, "lc")) {
            obj->mCapStyle = getLineCap();
        } else if (0 == strcmp(key, "lj")) {
            obj->mJoinStyle = getLineJoin();
        } else if (0 == strcmp(key, "ml")) {
            RAPIDJSON_ASSERT(PeekType() == kNumberType);
            obj->mMiterLimit = GetDouble();
        } else if (0 == strcmp(key, "d")) {
            parseDashProperty(obj->mDash);
        } else {
            parseGradientProperty(obj, key);
        }
    }

    obj->setStatic(obj->isStatic() && obj->mWidth.isStatic() &&
                   obj->mDash.isStatic());
    return sharedGStroke;
}

void LottieParserImpl::getValue(std::vector<VPointF> &v)
{
    RAPIDJSON_ASSERT(PeekType() == kArrayType);
    EnterArray();
    while (NextArrayValue()) {
        RAPIDJSON_ASSERT(PeekType() == kArrayType);
        EnterArray();
        VPointF pt;
        getValue(pt);
        v.push_back(pt);
    }
}

void LottieParserImpl::getValue(VPointF &pt)
{
    float val[4] = {0.f};
    int   i = 0;

    if (PeekType() == kArrayType) EnterArray();

    while (NextArrayValue()) {
        const auto value = GetDouble();
        if (i < 4) {
            val[i++] = value;
        }
    }
    pt.setX(val[0]);
    pt.setY(val[1]);
}

void LottieParserImpl::getValue(float &val)
{
    if (PeekType() == kArrayType) {
        EnterArray();
        if (NextArrayValue()) val = GetDouble();
        // discard rest
        while (NextArrayValue()) {
            GetDouble();
        }
    } else if (PeekType() == kNumberType) {
        val = GetDouble();
    } else {
        RAPIDJSON_ASSERT(0);
    }
}

void LottieParserImpl::getValue(LottieColor &color)
{
    float val[4] = {0.f};
    int   i = 0;
    if (PeekType() == kArrayType) EnterArray();

    while (NextArrayValue()) {
        const auto value = GetDouble();
        if (i < 4) {
            val[i++] = value;
        }
    }
    color = applyReplacements(LottieColor(val[0], val[1], val[2]));
}

void LottieParserImpl::getValue(LottieGradient &grad)
{
    if (PeekType() == kArrayType) EnterArray();

    while (NextArrayValue()) {
        grad.mGradient.push_back(GetDouble());
    }
}

void LottieParserImpl::getValue(int &val)
{
    if (PeekType() == kArrayType) {
        EnterArray();
        while (NextArrayValue()) {
            val = GetInt();
        }
    } else if (PeekType() == kNumberType) {
        val = GetInt();
    } else {
        RAPIDJSON_ASSERT(0);
    }
}

void LottieParserImpl::getValue(LottieShapeData &obj)
{
    std::vector<VPointF> inPoint;  /* "i" */
    std::vector<VPointF> outPoint; /* "o" */
    std::vector<VPointF> vertices; /* "v" */
    std::vector<VPointF> points;
    bool                 closed = false;

    /*
     * The shape object could be wrapped by a array
     * if its part of the keyframe object
     */
    bool arrayWrapper = (PeekType() == kArrayType);
    if (arrayWrapper) EnterArray();

    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "i")) {
            getValue(inPoint);
        } else if (0 == strcmp(key, "o")) {
            getValue(outPoint);
        } else if (0 == strcmp(key, "v")) {
            getValue(vertices);
        } else if (0 == strcmp(key, "c")) {
            closed = GetBool();
        } else {
            RAPIDJSON_ASSERT(0);
            Skip(nullptr);
        }
    }
    // exit properly from the array
    if (arrayWrapper) NextArrayValue();

    // shape data could be empty.
    if (inPoint.empty() || outPoint.empty() || vertices.empty()) return;

    /*
     * Convert the AE shape format to
     * list of bazier curves
     * The final structure will be Move +size*Cubic + Cubic (if the path is
     * closed one)
     */
    if (inPoint.size() != outPoint.size() ||
        inPoint.size() != vertices.size()) {
        vCritical << "The Shape data are corrupted";
        points = std::vector<VPointF>();
    } else {
        auto size = vertices.size();
        points.reserve(3 * size + 4);
        points.push_back(vertices[0]);
        for (size_t i = 1; i < size; i++) {
            points.push_back(vertices[i - 1] +
                             outPoint[i - 1]);  // CP1 = start + outTangent
            points.push_back(vertices[i] +
                             inPoint[i]);   // CP2 = end + inTangent
            points.push_back(vertices[i]);  // end point
        }

        if (closed) {
            points.push_back(vertices[size - 1] +
                             outPoint[size - 1]);  // CP1 = start + outTangent
            points.push_back(vertices[0] +
                             inPoint[0]);   // CP2 = end + inTangent
            points.push_back(vertices[0]);  // end point
        }
    }
    obj.mPoints = std::move(points);
    obj.mClosed = closed;
}

VPointF LottieParserImpl::parseInperpolatorPoint()
{
    VPointF cp;
    RAPIDJSON_ASSERT(PeekType() == kObjectType);
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "x")) {
            getValue(cp.rx());
        }
        if (0 == strcmp(key, "y")) {
            getValue(cp.ry());
        }
    }
    return cp;
}

template <typename T>
bool LottieParserImpl::parseKeyFrameValue(const char *, LOTKeyFrameValue<T> &)
{
    return false;
}

template <>
bool LottieParserImpl::parseKeyFrameValue(const char *               key,
                                          LOTKeyFrameValue<VPointF> &value)
{
    if (0 == strcmp(key, "ti")) {
        value.mPathKeyFrame = true;
        getValue(value.mInTangent);
    } else if (0 == strcmp(key, "to")) {
        value.mPathKeyFrame = true;
        getValue(value.mOutTangent);
    } else {
        return false;
    }
    return true;
}

std::shared_ptr<VInterpolator> LottieParserImpl::interpolator(
    VPointF inTangent, VPointF outTangent, std::string key)
{
    if (key.empty()) {
        std::array<char, 20> temp;
        snprintf(temp.data(), temp.size(), "%.2f_%.2f_%.2f_%.2f", inTangent.x(),
                 inTangent.y(), outTangent.x(), outTangent.y());
        key = temp.data();
    }

    auto search = mInterpolatorCache.find(key);

    if (search != mInterpolatorCache.end()) {
        return search->second;
    }

    auto obj = std::make_shared<VInterpolator>(
        VInterpolator(outTangent, inTangent));
    mInterpolatorCache[std::move(key)] = obj;
    return obj;
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/properties/multiDimensionalKeyframed.json
 */
template <typename T>
void LottieParserImpl::parseKeyFrame(LOTAnimInfo<T> &obj)
{
    struct ParsedField {
        std::string interpolatorKey;
        bool        interpolator{false};
        bool        value{false};
        bool        hold{false};
        bool        noEndValue{true};
    };

    EnterObject();
    ParsedField    parsed;
    LOTKeyFrame<T> keyframe;
    VPointF        inTangent;
    VPointF        outTangent;

    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "i")) {
            parsed.interpolator = true;
            inTangent = parseInperpolatorPoint();
        } else if (0 == strcmp(key, "o")) {
            outTangent = parseInperpolatorPoint();
        } else if (0 == strcmp(key, "t")) {
            keyframe.mStartFrame = GetDouble();
        } else if (0 == strcmp(key, "s")) {
            parsed.value = true;
            getValue(keyframe.mValue.mStartValue);
            continue;
        } else if (0 == strcmp(key, "e")) {
            parsed.noEndValue = false;
            getValue(keyframe.mValue.mEndValue);
            continue;
        } else if (0 == strcmp(key, "n")) {
            if (PeekType() == kStringType) {
                parsed.interpolatorKey = GetString();
            } else {
                RAPIDJSON_ASSERT(PeekType() == kArrayType);
                EnterArray();
                while (NextArrayValue()) {
                    RAPIDJSON_ASSERT(PeekType() == kStringType);
                    if (parsed.interpolatorKey.empty()) {
                        parsed.interpolatorKey = GetString();
                    } else {
                        // skip rest of the string
                        GetString();
                    }
                }
            }
            continue;
        } else if (parseKeyFrameValue(key, keyframe.mValue)) {
            continue;
        } else if (0 == strcmp(key, "h")) {
            parsed.hold = GetInt();
            continue;
        } else {
#ifdef DEBUG_PARSER
            vDebug << "key frame property skipped = " << key;
#endif
            Skip(key);
        }
    }

    if (!obj.mKeyFrames.empty()) {
        // update the endFrame value of current keyframe
        obj.mKeyFrames.back().mEndFrame = keyframe.mStartFrame;
        // if no end value provided, copy start value to previous frame
        if (parsed.value && parsed.noEndValue) {
            obj.mKeyFrames.back().mValue.mEndValue =
                keyframe.mValue.mStartValue;
        }
    }

    if (parsed.hold) {
        keyframe.mValue.mEndValue = keyframe.mValue.mStartValue;
        keyframe.mEndFrame = keyframe.mStartFrame;
        obj.mKeyFrames.push_back(keyframe);
    } else if (parsed.interpolator) {
        keyframe.mInterpolator = interpolator(
            inTangent, outTangent, std::move(parsed.interpolatorKey));
        obj.mKeyFrames.push_back(keyframe);
    } else {
        // its the last frame discard.
    }
}

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/properties/shapeKeyframed.json
 */

/*
 * https://github.com/airbnb/lottie-web/blob/master/docs/json/properties/shape.json
 */
void LottieParserImpl::parseShapeProperty(LOTAnimatable<LottieShapeData> &obj)
{
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "k")) {
            if (PeekType() == kArrayType) {
                EnterArray();
                while (NextArrayValue()) {
                    RAPIDJSON_ASSERT(PeekType() == kObjectType);
                    parseKeyFrame(obj.animation());
                }
            } else {
                getValue(obj.value());
            }
        } else {
#ifdef DEBUG_PARSER
            vDebug << "shape property ignored = " << key;
#endif
            Skip(nullptr);
        }
    }
}

template <typename T>
void LottieParserImpl::parsePropertyHelper(LOTAnimatable<T> &obj)
{
    if (PeekType() == kNumberType) {
        /*single value property with no animation*/
        getValue(obj.value());
    } else {
        RAPIDJSON_ASSERT(PeekType() == kArrayType);
        EnterArray();
        while (NextArrayValue()) {
            /* property with keyframe info*/
            if (PeekType() == kObjectType) {
                parseKeyFrame(obj.animation());
            } else {
                /* Read before modifying.
                 * as there is no way of knowing if the
                 * value of the array is either array of numbers
                 * or array of object without entering the array
                 * thats why this hack is there
                 */
                RAPIDJSON_ASSERT(PeekType() == kNumberType);
                /*multi value property with no animation*/
                getValue(obj.value());
                /*break here as we already reached end of array*/
                break;
            }
        }
    }
}

/*
 * https://github.com/airbnb/lottie-web/tree/master/docs/json/properties
 */
template <typename T>
void LottieParserImpl::parseProperty(LOTAnimatable<T> &obj)
{
    EnterObject();
    while (const char *key = NextObjectKey()) {
        if (0 == strcmp(key, "k")) {
            parsePropertyHelper(obj);
        } else {
            Skip(key);
        }
    }
}

#ifdef LOTTIE_DUMP_TREE_SUPPORT

class LOTDataInspector {
public:
    void visit(LOTCompositionData *obj, std::string level)
    {
        vDebug << " { " << level << "Composition:: a: " << !obj->isStatic()
               << ", v: " << obj->mVersion << ", stFm: " << obj->startFrame()
               << ", endFm: " << obj->endFrame()
               << ", W: " << obj->size().width()
               << ", H: " << obj->size().height() << "\n";
        level.append("\t");
        visit(obj->mRootLayer.get(), level);
        level.erase(level.end() - 1, level.end());
        vDebug << " } " << level << "Composition End\n";
    }
    void visit(LOTLayerData *obj, std::string level)
    {
        vDebug << level << "{ " << layerType(obj->mLayerType)
               << ", name: " << obj->name() << ", id:" << obj->mId
               << " Pid:" << obj->mParentId << ", a:" << !obj->isStatic()
               << ", " << matteType(obj->mMatteType)
               << ", mask:" << obj->hasMask() << ", inFm:" << obj->mInFrame
               << ", outFm:" << obj->mOutFrame << ", stFm:" << obj->mStartFrame
               << ", ts:" << obj->mTimeStreatch << ", ao:" << obj->autoOrient()
               << ", W:" << obj->layerSize().width()
               << ", H:" << obj->layerSize().height();

        if (obj->mLayerType == LayerType::Image)
            vDebug << level << "\t{ "
                   << "ImageInfo:"
                   << " W :" << obj->extra()->mAsset->mWidth
                   << ", H :" << obj->extra()->mAsset->mHeight << " }"
                   << "\n";
        else {
            vDebug << level;
        }
        visitChildren(static_cast<LOTGroupData *>(obj), level);
        vDebug << level << "} " << layerType(obj->mLayerType).c_str()
               << ", id: " << obj->mId << "\n";
    }
    void visitChildren(LOTGroupData *obj, std::string level)
    {
        level.append("\t");
        for (const auto &child : obj->mChildren) visit(child.get(), level);
        if (obj->mTransform) visit(obj->mTransform.get(), level);
    }

    void visit(LOTData *obj, std::string level)
    {
        switch (obj->mType) {
        case LOTData::Type::Repeater: {
            auto r = static_cast<LOTRepeaterData *>(obj);
            vDebug << level << "{ Repeater: name: " << obj->name()
                   << " , a:" << !obj->isStatic()
                   << ", copies:" << r->maxCopies()
                   << ", offset:" << r->offset(0);
            visitChildren(r->mContent.get(), level);
            vDebug << level << "} Repeater";
            break;
        }
        case LOTData::Type::ShapeGroup: {
            vDebug << level << "{ ShapeGroup: name: " << obj->name()
                   << " , a:" << !obj->isStatic();
            visitChildren(static_cast<LOTGroupData *>(obj), level);
            vDebug << level << "} ShapeGroup";
            break;
        }
        case LOTData::Type::Layer: {
            visit(static_cast<LOTLayerData *>(obj), level);
            break;
        }
        case LOTData::Type::Trim: {
            vDebug << level << "{ Trim: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Rect: {
            vDebug << level << "{ Rect: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Ellipse: {
            vDebug << level << "{ Ellipse: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Shape: {
            vDebug << level << "{ Shape: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Polystar: {
            vDebug << level << "{ Polystar: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Transform: {
            vDebug << level << "{ Transform: name: " << obj->name()
                   << " , a: " << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Stroke: {
            vDebug << level << "{ Stroke: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::GStroke: {
            vDebug << level << "{ GStroke: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::Fill: {
            vDebug << level << "{ Fill: name: " << obj->name()
                   << " , a:" << !obj->isStatic() << " }";
            break;
        }
        case LOTData::Type::GFill: {
            auto f = static_cast<LOTGFillData *>(obj);
            vDebug << level << "{ GFill: name: " << obj->name()
                   << " , a:" << !f->isStatic() << ", ty:" << f->mGradientType
                   << ", s:" << f->mStartPoint.value(0)
                   << ", e:" << f->mEndPoint.value(0) << " }";
            break;
        }
        default:
            break;
        }
    }

    std::string matteType(MatteType type)
    {
        switch (type) {
        case MatteType::None:
            return "Matte::None";
            break;
        case MatteType::Alpha:
            return "Matte::Alpha";
            break;
        case MatteType::AlphaInv:
            return "Matte::AlphaInv";
            break;
        case MatteType::Luma:
            return "Matte::Luma";
            break;
        case MatteType::LumaInv:
            return "Matte::LumaInv";
            break;
        default:
            return "Matte::Unknown";
            break;
        }
    }
    std::string layerType(LayerType type)
    {
        switch (type) {
        case LayerType::Precomp:
            return "Layer::Precomp";
            break;
        case LayerType::Null:
            return "Layer::Null";
            break;
        case LayerType::Shape:
            return "Layer::Shape";
            break;
        case LayerType::Solid:
            return "Layer::Solid";
            break;
        case LayerType::Image:
            return "Layer::Image";
            break;
        case LayerType::Text:
            return "Layer::Text";
            break;
        default:
            return "Layer::Unknown";
            break;
        }
    }
};

#endif

LottieParser::~LottieParser() = default;
LottieParser::LottieParser(
    char *str, const char *dir_path,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>
        &colorReplacements, rlottie::FitzModifier fitzModifier)
    : d(std::make_unique<LottieParserImpl>(str, dir_path, colorReplacements, fitzModifier))
{
    if (d->VerifyType())
        d->parseComposition();
    else
        vWarning << "Input data is not Lottie format!";
}

std::shared_ptr<LOTModel> LottieParser::model()
{
    if (!d->composition()) return nullptr;

    std::shared_ptr<LOTModel> model = std::make_shared<LOTModel>();
    model->mRoot = d->composition();
    model->mRoot->processRepeaterObjects();

#ifdef LOTTIE_DUMP_TREE_SUPPORT
    LOTDataInspector inspector;
    inspector.visit(model->mRoot.get(), "");
#endif

    return model;
}

RAPIDJSON_DIAG_POP

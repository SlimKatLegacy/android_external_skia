/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkSVGDevice.h"

#include "SkBitmap.h"
#include "SkDraw.h"
#include "SkPaint.h"
#include "SkParsePath.h"
#include "SkShader.h"
#include "SkStream.h"
#include "SkXMLWriter.h"

namespace {

static SkString svg_color(SkColor color) {
    SkString colorStr;
    colorStr.printf("rgb(%u,%u,%u)",
                     SkColorGetR(color),
                     SkColorGetG(color),
                     SkColorGetB(color));
    return colorStr;
}

static SkScalar svg_opacity(SkColor color) {
    return SkIntToScalar(SkColorGetA(color)) / SK_AlphaOPAQUE;
}

struct Resources {
    Resources(const SkPaint& paint)
        : fPaintServer(svg_color(paint.getColor())) {}

    SkString fPaintServer;
};

}

// For now all this does is serve unique serial IDs, but it will eventually evolve to track
// and deduplicate resources.
class SkSVGDevice::ResourceBucket : ::SkNoncopyable {
public:
    ResourceBucket() : fGradientCount(0) {}

    SkString addLinearGradient() {
        SkString id;
        id.printf("gradient_%d", fGradientCount++);
        return id;
    }

private:
    uint32_t fGradientCount;
};

class SkSVGDevice::AutoElement : ::SkNoncopyable {
public:
    AutoElement(const char name[], SkXMLWriter* writer)
        : fWriter(writer)
        , fResourceBucket(NULL) {
        fWriter->startElement(name);
    }

    AutoElement(const char name[], SkXMLWriter* writer, ResourceBucket* bucket,
                const SkDraw& draw, const SkPaint& paint)
        : fWriter(writer)
        , fResourceBucket(bucket) {

        Resources res = this->addResources(paint);

        fWriter->startElement(name);

        this->addPaint(paint, res);
        this->addTransform(*draw.fMatrix);
    }

    ~AutoElement() {
        fWriter->endElement();
    }

    void addAttribute(const char name[], const char val[]) {
        fWriter->addAttribute(name, val);
    }

    void addAttribute(const char name[], const SkString& val) {
        fWriter->addAttribute(name, val.c_str());
    }

    void addAttribute(const char name[], int32_t val) {
        fWriter->addS32Attribute(name, val);
    }

    void addAttribute(const char name[], SkScalar val) {
        fWriter->addScalarAttribute(name, val);
    }

private:
    Resources addResources(const SkPaint& paint);
    void addResourceDefs(const SkPaint& paint, Resources* resources);

    void addPaint(const SkPaint& paint, const Resources& resources);
    void addTransform(const SkMatrix& transform, const char name[] = "transform");

    SkString addLinearGradientDef(const SkShader::GradientInfo& info, const SkShader* shader);

    SkXMLWriter*    fWriter;
    ResourceBucket* fResourceBucket;
};

void SkSVGDevice::AutoElement::addPaint(const SkPaint& paint, const Resources& resources) {
    SkPaint::Style style = paint.getStyle();
    if (style == SkPaint::kFill_Style || style == SkPaint::kStrokeAndFill_Style) {
        this->addAttribute("fill", resources.fPaintServer);
    } else {
        this->addAttribute("fill", "none");
    }

    if (style == SkPaint::kStroke_Style || style == SkPaint::kStrokeAndFill_Style) {
        this->addAttribute("stroke", resources.fPaintServer);
        this->addAttribute("stroke-width", paint.getStrokeWidth());
    } else {
        this->addAttribute("stroke", "none");
    }

    if (SK_AlphaOPAQUE != SkColorGetA(paint.getColor())) {
        this->addAttribute("opacity", svg_opacity(paint.getColor()));
    }
}

void SkSVGDevice::AutoElement::addTransform(const SkMatrix& t, const char name[]) {
    if (t.isIdentity()) {
        return;
    }

    SkString tstr;
    switch (t.getType()) {
    case SkMatrix::kPerspective_Mask:
        SkDebugf("Can't handle perspective matrices.");
        break;
    case SkMatrix::kTranslate_Mask:
        tstr.printf("translate(%g %g)",
                     SkScalarToFloat(t.getTranslateX()),
                     SkScalarToFloat(t.getTranslateY()));
        break;
    case SkMatrix::kScale_Mask:
        tstr.printf("scale(%g %g)",
                     SkScalarToFloat(t.getScaleX()),
                     SkScalarToFloat(t.getScaleY()));
        break;
    default:
        tstr.printf("matrix(%g %g %g %g %g %g)",
                     SkScalarToFloat(t.getScaleX()), SkScalarToFloat(t.getSkewY()),
                     SkScalarToFloat(t.getSkewX()), SkScalarToFloat(t.getScaleY()),
                     SkScalarToFloat(t.getTranslateX()), SkScalarToFloat(t.getTranslateY()));
        break;
    }

    fWriter->addAttribute(name, tstr.c_str());
}

Resources SkSVGDevice::AutoElement::addResources(const SkPaint& paint) {
    Resources resources(paint);
    this->addResourceDefs(paint, &resources);
    return resources;
}

void SkSVGDevice::AutoElement::addResourceDefs(const SkPaint& paint, Resources* resources) {
    const SkShader* shader = paint.getShader();
    if (!SkToBool(shader)) {
        // TODO: clip support
        return;
    }

    SkShader::GradientInfo grInfo;
    grInfo.fColorCount = 0;
    if (SkShader::kLinear_GradientType != shader->asAGradient(&grInfo)) {
        // TODO: non-linear gradient support
        SkDebugf("unsupported shader type\n");
        return;
    }

    {
        AutoElement defs("defs", fWriter);

        SkAutoSTArray<16, SkColor>  grColors(grInfo.fColorCount);
        SkAutoSTArray<16, SkScalar> grOffsets(grInfo.fColorCount);
        grInfo.fColors = grColors.get();
        grInfo.fColorOffsets = grOffsets.get();

        // One more call to get the actual colors/offsets.
        shader->asAGradient(&grInfo);
        SkASSERT(grInfo.fColorCount <= grColors.count());
        SkASSERT(grInfo.fColorCount <= grOffsets.count());

        resources->fPaintServer.printf("url(#%s)", addLinearGradientDef(grInfo, shader).c_str());
    }
}

SkString SkSVGDevice::AutoElement::addLinearGradientDef(const SkShader::GradientInfo& info,
                                                        const SkShader* shader) {
    SkASSERT(fResourceBucket);
    SkString id = fResourceBucket->addLinearGradient();

    {
        AutoElement gradient("linearGradient", fWriter);

        gradient.addAttribute("id", id);
        gradient.addAttribute("gradientUnits", "userSpaceOnUse");
        gradient.addAttribute("x1", info.fPoint[0].x());
        gradient.addAttribute("y1", info.fPoint[0].y());
        gradient.addAttribute("x2", info.fPoint[1].x());
        gradient.addAttribute("y2", info.fPoint[1].y());
        gradient.addTransform(shader->getLocalMatrix(), "gradientTransform");

        SkASSERT(info.fColorCount >= 2);
        for (int i = 0; i < info.fColorCount; ++i) {
            SkColor color = info.fColors[i];
            SkString colorStr(svg_color(color));

            {
                AutoElement stop("stop", fWriter);
                stop.addAttribute("offset", info.fColorOffsets[i]);
                stop.addAttribute("stop-color", colorStr.c_str());

                if (SK_AlphaOPAQUE != SkColorGetA(color)) {
                    stop.addAttribute("stop-opacity", svg_opacity(color));
                }
            }
        }
    }

    return id;
}

SkBaseDevice* SkSVGDevice::Create(const SkISize& size, SkWStream* wstream) {
    if (!SkToBool(wstream)) {
        return NULL;
    }

    return SkNEW_ARGS(SkSVGDevice, (size, wstream));
}

SkSVGDevice::SkSVGDevice(const SkISize& size, SkWStream* wstream)
    : fWriter(SkNEW_ARGS(SkXMLStreamWriter, (wstream)))
    , fResourceBucket(SkNEW(ResourceBucket)) {

    fLegacyBitmap.setInfo(SkImageInfo::MakeUnknown(size.width(), size.height()));

    fWriter->writeHeader();

    // The root <svg> tag gets closed by the destructor.
    fRootElement.reset(SkNEW_ARGS(AutoElement, ("svg", fWriter)));

    fRootElement->addAttribute("xmlns", "http://www.w3.org/2000/svg");
    fRootElement->addAttribute("xmlns:xlink", "http://www.w3.org/1999/xlink");
    fRootElement->addAttribute("width", size.width());
    fRootElement->addAttribute("height", size.height());
}

SkSVGDevice::~SkSVGDevice() {
}

SkImageInfo SkSVGDevice::imageInfo() const {
    return fLegacyBitmap.info();
}

const SkBitmap& SkSVGDevice::onAccessBitmap() {
    return fLegacyBitmap;
}

void SkSVGDevice::drawPaint(const SkDraw& draw, const SkPaint& paint) {
    AutoElement rect("rect", fWriter, fResourceBucket, draw, paint);
    rect.addAttribute("x", 0);
    rect.addAttribute("y", 0);
    rect.addAttribute("width", this->width());
    rect.addAttribute("height", this->height());
}

void SkSVGDevice::drawPoints(const SkDraw&, SkCanvas::PointMode mode, size_t count,
                             const SkPoint[], const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawPoints()");
}

void SkSVGDevice::drawRect(const SkDraw& draw, const SkRect& r, const SkPaint& paint) {
    AutoElement rect("rect", fWriter, fResourceBucket, draw, paint);
    rect.addAttribute("x", r.fLeft);
    rect.addAttribute("y", r.fTop);
    rect.addAttribute("width", r.width());
    rect.addAttribute("height", r.height());
}

void SkSVGDevice::drawOval(const SkDraw& draw, const SkRect& oval, const SkPaint& paint) {
    AutoElement ellipse("ellipse", fWriter, fResourceBucket, draw, paint);
    ellipse.addAttribute("cx", oval.centerX());
    ellipse.addAttribute("cy", oval.centerY());
    ellipse.addAttribute("rx", oval.width() / 2);
    ellipse.addAttribute("ry", oval.height() / 2);
}

void SkSVGDevice::drawRRect(const SkDraw&, const SkRRect& rr, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawRRect()");
}

void SkSVGDevice::drawPath(const SkDraw& draw, const SkPath& path, const SkPaint& paint,
                           const SkMatrix* prePathMatrix, bool pathIsMutable) {
    AutoElement elem("path", fWriter, fResourceBucket, draw, paint);

    SkString pathStr;
    SkParsePath::ToSVGString(path, &pathStr);
    elem.addAttribute("d", pathStr.c_str());
}

void SkSVGDevice::drawBitmap(const SkDraw&, const SkBitmap& bitmap,
                             const SkMatrix& matrix, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawBitmap()");
}

void SkSVGDevice::drawSprite(const SkDraw&, const SkBitmap& bitmap,
                             int x, int y, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawSprite()");
}

void SkSVGDevice::drawBitmapRect(const SkDraw&, const SkBitmap&, const SkRect* srcOrNull,
                                 const SkRect& dst, const SkPaint& paint,
                                 SkCanvas::DrawBitmapRectFlags flags) {
    // todo
    SkDebugf("unsupported operation: drawBitmapRect()");
}

void SkSVGDevice::drawText(const SkDraw&, const void* text, size_t len,
                           SkScalar x, SkScalar y, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawText()");
}

void SkSVGDevice::drawPosText(const SkDraw&, const void* text, size_t len,const SkScalar pos[],
                              int scalarsPerPos, const SkPoint& offset, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawPosText()");
}

void SkSVGDevice::drawTextOnPath(const SkDraw&, const void* text, size_t len, const SkPath& path,
                                 const SkMatrix* matrix, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawTextOnPath()");
}

void SkSVGDevice::drawVertices(const SkDraw&, SkCanvas::VertexMode, int vertexCount,
                               const SkPoint verts[], const SkPoint texs[],
                               const SkColor colors[], SkXfermode* xmode,
                               const uint16_t indices[], int indexCount,
                               const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawVertices()");
}

void SkSVGDevice::drawDevice(const SkDraw&, SkBaseDevice*, int x, int y,
                             const SkPaint&) {
    // todo
    SkDebugf("unsupported operation: drawDevice()");
}

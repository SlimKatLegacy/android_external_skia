/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "DMPDFRasterizeTask.h"
#include "DMExpectationsTask.h"
#include "DMUtil.h"
#include "DMWriteTask.h"
#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkStream.h"

namespace DM {

PDFRasterizeTask::PDFRasterizeTask(const Task& parent,
                                   SkData* pdf,
                                   const Expectations& expectations,
                                   RasterizePdfProc proc)
    : CpuTask(parent)
    , fName(UnderJoin(parent.name().c_str(), "rasterize"))
    , fPdf(SkRef(pdf))
    , fExpectations(expectations)
    , fRasterize(proc) {}

void PDFRasterizeTask::draw() {
    SkMemoryStream pdfStream(fPdf.get());
    SkBitmap bitmap;

    if (!fRasterize(&pdfStream, &bitmap)) {
        this->fail();
    }
    if (!fExpectations.check(*this, bitmap)) {
        this->fail();
        this->spawnChild(SkNEW_ARGS(WriteTask, (*this, bitmap)));
    }
}

}  // namespace DM

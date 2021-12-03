// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_adaptive_pooling.h"
#include "ie_parallel.hpp"
#include <cpu/x64/cpu_isa_traits.hpp>
#include <math.h>
#include <mkldnn.hpp>
#include <mkldnn_extension_utils.h>
#include <mkldnn_selective_build.h>
#include <mkldnn_types.h>
#include <ngraph/opsets/opset8.hpp>
#include <string>
#include <utils/bfloat16.hpp>
#include <utils/general_utils.h>
#include <vector>

using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace mkldnn;
using namespace mkldnn::impl::cpu::x64;

bool MKLDNNAdaptivePoolingNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        if (one_of(op->get_type_info(), ngraph::op::v8::AdaptiveAvgPool::get_type_info_static())) {
            auto adaPool = std::dynamic_pointer_cast<const ngraph::opset8::AdaptiveAvgPool>(op);
            if (!adaPool) {
                errorMessage = "Only opset8 AdaptiveAvgPooling operation is supported";
                return false;
            }
        } else if (one_of(op->get_type_info(), ngraph::op::v8::AdaptiveMaxPool::get_type_info_static())) {
            auto adaPool = std::dynamic_pointer_cast<const ngraph::opset8::AdaptiveMaxPool>(op);
            if (!adaPool) {
                errorMessage = "Only opset8 AdaptiveMaxPooling operation is supported";
                return false;
            }
        } else {
            errorMessage = "Unsupported Adaptive pooling mode";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

MKLDNNAdaptivePoolingNode::MKLDNNAdaptivePoolingNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng,
                                           MKLDNNWeightsSharing::Ptr &cache) : MKLDNNNode(op, eng, cache) {
    std::string errorMessage;
    if (isSupportedOperation(op, errorMessage)) {
      errorPrefix = "Adaptive Pooling layer with name '" + getName() + "' ";
    } else {
      IE_THROW(NotImplemented) << errorMessage;
    }
    if (one_of(op->get_type_info(), ngraph::op::v8::AdaptiveAvgPool::get_type_info_static())) {
        algorithm = Algorithm::AdaptivePoolingAvg;
    } else if (one_of(op->get_type_info(), ngraph::op::v8::AdaptiveMaxPool::get_type_info_static())) {
        algorithm = Algorithm::AdaptivePoolingMax;
    }
    spatialDimsCount = getInputShapeAtPort(0).getRank() - 2;
    spatialDimsValue.resize(spatialDimsCount);
}

void MKLDNNAdaptivePoolingNode::getSupportedDescriptors() {
    if (!descs.empty())
        return;

    if (getParentEdges().size() != 2)
        IE_THROW() << errorPrefix << "has incorrect number of input edges: " << getParentEdges().size();
    if (getChildEdges().size() != (algorithm == AdaptivePoolingMax ? 2 : 1))
        IE_THROW() << errorPrefix << "has incorrect number of output edges: " << getParentEdges().size();

    auto srcRank = getInputShapeAtPort(0).getRank();
    if (!one_of(spatialDimsCount, 1, 2, 3)) {
        IE_THROW() << errorPrefix << "doesn't support 0th input with rank: " << srcRank;
    }

    if (getInputShapeAtPort(1).getRank() != 1) {
        IE_THROW() << errorPrefix << "doesn't support 1st input with rank: " << getInputShapeAtPort(1).getRank();
    }

    if (getOutputShapeAtPort(0).getRank() != getInputShapeAtPort(0).getRank()) {
        IE_THROW() << errorPrefix << "must keep data rank";
    }
}

bool MKLDNNAdaptivePoolingNode::needShapeInfer() const {
    const auto newSpatialDimsPtr = reinterpret_cast<int32_t *>(getParentEdgesAtPort(1)[0]->getMemoryPtr()->GetPtr());
    for (size_t i = 0; i < spatialDimsCount; i++) {
        if (spatialDimsValue[i] != newSpatialDimsPtr[i])
            return true;
    }
    return MKLDNNNode::needShapeInfer();
}

std::vector<VectorDims> MKLDNNAdaptivePoolingNode::shapeInfer() const {
    const auto inputDims = getParentEdgesAtPort(0)[0]->getMemory().GetShape().getStaticDims();
    const auto spatialDims = getParentEdgesAtPort(1)[0]->getMemory().GetShape().getStaticDims();
    const auto inputRank = inputDims.size();
    const auto spatialDimsSize = spatialDims[0];

    VectorDims outputDims(inputRank);
    outputDims[0] = inputDims[0];
    outputDims[1] = inputDims[1];
    auto newSpatialDimsPtr = reinterpret_cast<int32_t *>(getParentEdgesAtPort(1)[0]->getMemoryPtr()->GetPtr());
    for (size_t i = 0; i < spatialDimsSize; i++) {
        outputDims[i + 2] = newSpatialDimsPtr[i];
        spatialDimsValue[i] = newSpatialDimsPtr[i];
    }

    std::vector<VectorDims> result(outputShapes.size(), outputDims);
    return result;
}

void MKLDNNAdaptivePoolingNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    // we supports only fp32 currently
    precision = Precision::FP32;

    InferenceEngine::LayerConfig config;
    config.dynBatchSupport = false;
    config.inConfs.resize(2);
    config.outConfs.resize((algorithm == Algorithm::AdaptivePoolingAvg ? 1 : 2));

    std::vector<LayoutType> dataFormats{ LayoutType::ncsp };
    const auto &inDims = getInputShapeAtPort(0).getDims();
    if (inDims[1] != Shape::UNDEFINED_DIM && inDims[1] != 1) {
        dataFormats.push_back(LayoutType::nspc);
        dataFormats.push_back(LayoutType::nCsp16c);
        dataFormats.push_back(LayoutType::nCsp8c);
    }
    for (const auto &df : dataFormats) {
        if (algorithm == Algorithm::AdaptivePoolingAvg) {
            addSupportedPrimDesc({{df, precision}, {LayoutType::ncsp, Precision::I32}},
                                 {{df, precision}},
                                 impl_desc_type::unknown);
        } else {
            addSupportedPrimDesc({{df, precision}, {LayoutType::ncsp, Precision::I32}},
                                 {{df, precision}, {LayoutType::ncsp, Precision::I32}},
                                 impl_desc_type::unknown);
        }
    }
}

void MKLDNNAdaptivePoolingNode::execute(mkldnn::stream strm) {
    auto inputPrec = getParentEdgeAt(0)->getMemory().GetDataType();
    auto outputPrec = getChildEdgeAt(0)->getMemory().GetDataType();
    if (!(inputPrec == mkldnn_f32 && outputPrec == mkldnn_f32))
        IE_THROW() << errorPrefix << "doesn't support demanded precisions";

    auto &srcMemory0 = getParentEdgeAt(0)->getMemory();
    auto &srcMemory1 = getParentEdgeAt(1)->getMemory();
    int *indexDst = nullptr;

    if (algorithm == Algorithm::AdaptivePoolingMax) {
        indexDst = reinterpret_cast<int *>(getChildEdgeAt(1)->getMemoryPtr()->GetPtr());
    }

    auto isPlainFmt = srcMemory0.getDesc().hasLayoutType(LayoutType::ncsp);
    auto isTailCFmt = srcMemory0.getDesc().hasLayoutType(LayoutType::nspc);
    auto isBlkFmt = srcMemory0.getDesc().hasLayoutType(LayoutType::nCsp16c) || srcMemory0.getDesc().hasLayoutType(LayoutType::nCsp8c);

    auto srcBlockDesc = srcMemory0.GetDescWithType<BlockedMemoryDesc>();
    int blockSize = isBlkFmt ? srcBlockDesc->getBlockDims().back() : 1;

    const auto *src = reinterpret_cast<const float *>(getParentEdgeAt(0)->getMemoryPtr()->GetPtr());
    const auto *srcPooledSpatialShapes = reinterpret_cast<const int *>(getParentEdgeAt(1)->getMemoryPtr()->GetPtr());
    auto *dst = reinterpret_cast<float *>(getChildEdgeAt(0)->getMemoryPtr()->GetPtr());

    if (srcMemory1.GetShape().getElementsCount() != spatialDimsCount)
        IE_THROW() << errorPrefix << "has input spatial dimension (" << srcMemory1.GetShape().getElementsCount()
                   << ") inconsistent with pooling vector size (" << spatialDimsCount << ")";

    auto inputDimVector = srcMemory0.getStaticDims();
    const int N = static_cast<int>(inputDimVector[0]);
    const int C = static_cast<int>(inputDimVector[1]);
    const int ID = static_cast<int>(spatialDimsCount == 3 ? inputDimVector[2] : 1);
    const int IH = static_cast<int>(spatialDimsCount >= 2 ? inputDimVector[spatialDimsCount] : 1);
    const int IW = static_cast<int>(inputDimVector[spatialDimsCount + 1]);

    const int OD = static_cast<int>(spatialDimsCount == 3 ? srcPooledSpatialShapes[0] : 1);
    const int OH = static_cast<int>(spatialDimsCount >= 2 ? srcPooledSpatialShapes[spatialDimsCount - 2] : 1);
    const int OW = static_cast<int>(srcPooledSpatialShapes[spatialDimsCount - 1]);

    const int iHW = IH * IW;
    const int oDHW = OD * OH * OW, oHW = OH * OW;

    const int chPadding = blockSize * (isBlkFmt ? srcBlockDesc->getBlockDims()[1] : srcMemory0.GetShape().getStaticDims()[1]);
    const int blockCount = (isTailCFmt ? 1 :  chPadding / blockSize);
    auto selectedPrimitiveDescriptor = getSelectedPrimitiveDescriptor();
    if (!selectedPrimitiveDescriptor)
        IE_THROW() << errorPrefix << "doesn't have primitive descriptors.";
    auto config = selectedPrimitiveDescriptor->getConfig();
    auto srcStrides = srcBlockDesc->getStrides();
    auto dstStrides = getChildEdgesAtPort(0)[0]->getMemory().GetDescWithType<BlockedMemoryDesc>()->getStrides();

    // unified strides array
    const size_t tailDimsOffset = (isTailCFmt ? -1 : 0);
    const size_t inStrides[5] = {
            srcStrides[0],
            (isTailCFmt ? 1 : srcStrides[1]),
            (spatialDimsCount == 3 ? srcStrides[2 + tailDimsOffset] : 0),
            (spatialDimsCount >= 2 ? srcStrides[spatialDimsCount + tailDimsOffset] : 0),
            srcStrides[spatialDimsCount + 1 + tailDimsOffset] };
    const size_t outStrides[5] = {
            dstStrides[0],
            (isTailCFmt ? 1 : dstStrides[1]),
            (spatialDimsCount == 3 ? dstStrides[2 + tailDimsOffset] : 0),
            (spatialDimsCount >= 2 ? dstStrides[spatialDimsCount + tailDimsOffset] : 0),
            dstStrides[spatialDimsCount + 1 + tailDimsOffset] };

    std::function<void(const float *, float *, int, int, int, size_t)> pool;
    auto poolMax = [&] (const float *srcData, float *dstData, int od, int oh, int ow, size_t spatIndOff) {
        size_t dStart, dEnd, hStart, hEnd, wStart, wEnd;
        setBinBorders(&dStart, &dEnd, od, ID, OD);
        setBinBorders(&hStart, &hEnd, oh, IH, OH);
        setBinBorders(&wStart, &wEnd, ow, IW, OW);
        float res = srcData[dStart * inStrides[2] + hStart * inStrides[3] + wStart * inStrides[4]];  // initial max value
        int resIndex = dStart * iHW + hStart * IW + wStart;  // initial max index
        for (size_t pixD = dStart; pixD < dEnd; pixD++) {
            for (size_t pixH = hStart; pixH < hEnd; pixH++) {
                for (size_t pixW = wStart; pixW < wEnd; pixW++) {
                    float curr = srcData[pixD * inStrides[2] + pixH * inStrides[3] + pixW * inStrides[4]];
                    resIndex = (res < curr ? pixD * iHW + pixH * IW + pixW : resIndex);
                    res = std::max(res, curr);
                }
            }
        }
        *dstData = res;
        indexDst[spatIndOff * oDHW + od * oHW + oh * OW + ow] = resIndex;
    };
    auto poolAvg = [&] (const float *srcData, float *dstData, int od, int oh, int ow, size_t spatIndOff) {
        size_t dStart, dEnd, hStart, hEnd, wStart, wEnd;
        setBinBorders(&dStart, &dEnd, od, ID, OD);
        setBinBorders(&hStart, &hEnd, oh, IH, OH);
        setBinBorders(&wStart, &wEnd, ow, IW, OW);
        auto binSize = (dEnd - dStart) * (hEnd - hStart) * (wEnd - wStart);
        if (binSize == 0)
            IE_THROW() << errorPrefix << "has empty bin";
        float sum = 0;
        for (size_t pixD = dStart; pixD < dEnd; pixD++) {
            for (size_t pixH = hStart; pixH < hEnd; pixH++) {
                for (size_t pixW = wStart; pixW < wEnd; pixW++) {
                    float curr = srcData[pixD * inStrides[2] + pixH * inStrides[3] + pixW * inStrides[4]];
                    sum = sum + curr;
                }
            }
        }
        *dstData = sum / binSize;
    };

    if (algorithm == Algorithm::AdaptivePoolingMax) {
        pool = poolMax;
    } else {
        pool = poolAvg;
    }

    parallel_for5d(N, blockCount, OD, OH, OW,
        [&](int n, int blkIdx, int od, int oh, int ow) {
        auto srcData = src + n * inStrides[0] + blkIdx * inStrides[1];
        auto dstData = dst + n * outStrides[0] + blkIdx * outStrides[1] +
                       od * outStrides[2] + oh * outStrides[3] + ow * outStrides[4];
        int cStart = 0, cEnd = C, inResidual = 0, outResidual = 0;
        if (!isTailCFmt) {
            cStart = blkIdx * blockSize;
            cEnd = (blkIdx == blockCount - 1 ? C : cStart + blockSize);
        }
        for (int c = cStart; c < cEnd; c++) {
            if (isTailCFmt) {
                inResidual = c * inStrides[1];
                outResidual = c * outStrides[1];
            } else if (!isPlainFmt) {
                inResidual = outResidual = c % blockSize;
            }
            pool(srcData + inResidual, dstData + outResidual, od, oh, ow, n * C + c);
        }});
}

bool MKLDNNAdaptivePoolingNode::created() const {
    return getType() == AdaptivePooling;
}

void MKLDNNAdaptivePoolingNode::createPrimitive() {}

inline void MKLDNNAdaptivePoolingNode::setBinBorders(size_t *startPtr, size_t *endPtr, size_t idx, size_t inputLength, size_t outputLength) {
    *(startPtr) = idx * inputLength / outputLength;
    *(endPtr) = ceil(static_cast<float>((idx + 1) * inputLength) / outputLength);
}

REG_MKLDNN_PRIM_FOR(MKLDNNAdaptivePoolingNode, AdaptivePooling)

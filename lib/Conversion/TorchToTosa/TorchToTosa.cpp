//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "torch-mlir/Conversion/TorchToTosa/TorchToTosa.h"
#include "torch-mlir/Conversion/TorchToTosa/TosaLegalizeCommon.h"
#include "torch-mlir/Conversion/TorchToTosa/TosaLegalizeUtils.h"

#include "../PassDetail.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/Dialect/Traits.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Transforms/DialectConversion.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/IR/TorchTypes.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include "torch-mlir/Dialect/TorchConversion/IR/TorchConversionDialect.h"
#include "torch-mlir/Dialect/TorchConversion/Transforms/BackendTypeConversion.h"

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

namespace {

// These legalizations are for unary ops with only for floating point datatypes.
// There is no supported quantized integer mode for these.
template <typename AtenOpT, typename TosaOpT>
class ConvertAtenUnaryFPOnlyOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value self = adaptor.getSelf();
    auto selfTy = self.getType().cast<TensorType>();

    if (!selfTy)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    if (selfTy.getElementType().isa<mlir::FloatType>()) {
      rewriter.replaceOpWithNewOp<TosaOpT>(
          op,
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              op.getType()),
          self);
      return success();
    } else {
      return rewriter.notifyMatchFailure(
          op, "Only floating-point datatype legalization supported");
    }
  }
};

// These unary op legalizations are identical for floating-point
// or quantized types
template <typename AtenOpT, typename TosaOpT>
class ConvertAtenUnaryOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<TosaOpT>(
        op,
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()),
        adaptor.getSelf());
    return success();
  }
};

// These binary op legalizations are identical for floating-point
// or quantized types
template <typename AtenOpT, typename TosaOpT>
class ConvertAtenBinaryOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().cast<TensorType>();
    Value rhs = adaptor.getOther();
    auto rhsTy = rhs.getType().cast<TensorType>();

    if (!lhsTy || !rhsTy)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    auto lhsElemTy = lhsTy.getElementType();
    auto rhsElemTy = rhsTy.getElementType();

    if (lhsElemTy != rhsElemTy)
      return rewriter.notifyMatchFailure(op, "Input datatypes mismatched");

    rewriter.replaceOpWithNewOp<TosaOpT>(
        op,
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()),
        lhs, rhs);
    return success();
  }
};

template <typename T>
static bool isInValidRange(bool isFloat, const double &doubleValue, bool isInt,
                           const int64_t &intValue) {
  if (isFloat) {
    // Do a round-trip check here instead of numeric limits due to
    // compiler warnings around double <-> int conversion.
    return (doubleValue == static_cast<double>(static_cast<T>(doubleValue)));
  } else {
    assert(isInt);
    return (intValue >= std::numeric_limits<T>::min()) &&
           (intValue <= std::numeric_limits<T>::max());
  }
  return true;
}

// FIXME: This will eventually go into a Tosa*Utils file.
LogicalResult torchScalarToTosaTensor(ConversionPatternRewriter &rewriter,
                                      Operation *op, Value torchScalarValue,
                                      Value &tosaTensor, Type dtype,
                                      llvm::ArrayRef<int64_t> dshape) {
  // Retrieve a const float or int value but create the out Tensor with dtype.
  double doubleValue;
  auto isFloat =
      matchPattern(torchScalarValue, m_TorchConstantFloat(&doubleValue));

  int64_t intValue;
  auto isInt = matchPattern(torchScalarValue, m_TorchConstantInt(&intValue));

  if (!isFloat && !isInt)
    return rewriter.notifyMatchFailure(op,
                                       "Unable to extract the scalar constant");

  if (dtype.isa<mlir::FloatType>()) {
    tosaTensor = tosa::getConstTensor<float>(
                     rewriter, op, (isFloat ? doubleValue : intValue), dshape)
                     .value();
  } else if (auto intType = dtype.dyn_cast<mlir::IntegerType>()) {
    auto w = intType.getWidth();
    if (w != 32 && w != 64)
      return rewriter.notifyMatchFailure(op, [&](Diagnostic &diag) {
        diag << "Unsupported integer type: " << intType;
      });

    if (w == 32) {
      if (!isInValidRange<int32_t>(isFloat, doubleValue, isInt, intValue)) {
        return rewriter.notifyMatchFailure(
            op, "Supplied value of scalar constant exceeds limits "
                "of destination type");
      }
      int32_t d = isFloat ? static_cast<int32_t>(doubleValue)
                          : static_cast<int32_t>(intValue);
      tosaTensor =
          tosa::getConstTensor<int32_t>(rewriter, op, {d}, dshape).value();
    } else if (w == 64) {
      if (!isInValidRange<int64_t>(isFloat, doubleValue, isInt, intValue)) {
        return rewriter.notifyMatchFailure(
            op, "Supplied value of scalar constant exceeds limits "
                "of destination type");
      }
      int64_t d = (isFloat ? static_cast<int64_t>(doubleValue) : intValue);
      tosaTensor =
          tosa::getConstTensor<int64_t>(rewriter, op, {d}, dshape).value();
    }
  } else {
    return rewriter.notifyMatchFailure(op, "Usupported element type");
  }

  return success();
}

LogicalResult torchAlphaToTosaTensor(ConversionPatternRewriter &rewriter,
                                     Operation *op, Value alphaScalar,
                                     Value &alphaTensor, Type dtype,
                                     bool checkForUnity) {
  if (succeeded(torchScalarToTosaTensor(rewriter, op, alphaScalar, alphaTensor,
                                        dtype, {})))
    return success();

  // `alpha` has not been specified.
  int64_t alphaValue;
  if (!matchPattern(alphaScalar, m_TorchConstantInt(&alphaValue)))
    return rewriter.notifyMatchFailure(
        op, "Currently only scalar constants are supported for "
            "alpha in TOSA operation");
  // When no alpha has been specified, this must be 1.
  if (checkForUnity && alphaValue != 1)
    return rewriter.notifyMatchFailure(op,
                                       "Unsupported integer value for alpha");

  alphaTensor =
      mlir::tosa::getTosaConstTensorSingleF32(rewriter, op, alphaValue);

  return success();
}

// These binary op legalizations are specific to add/sub which have an
// alpha multiplier.
template <typename AtenOpT, typename TosaOpT>
class ConvertAtenAddSubOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // left  : tensor: tensor<i32/i64/f32>
    // right : scalar: i32/i64/f32
    //         tensor: tensor<i32/i64/f32>
    // alpha : scalar: i32/i64/f32
    // output: tensor: tensor<i32/i64/f32>
    Value lhs = adaptor.getSelf();
    auto lhsType = lhs.getType().dyn_cast<TensorType>();
    Value rhs = adaptor.getOther();
    auto rhsType = rhs.getType().dyn_cast<TensorType>();

    if (!lhsType)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    if (auto lhsElemTy = lhsType.getElementType().dyn_cast<IntegerType>()) {
      if (lhsElemTy.getWidth() > 64)
        return rewriter.notifyMatchFailure(
            op, "Integers with widths greater than 64 are not supported");
    }

    // Get output type: tensor<i32/i64/f32>
    auto outType = OpConversionPattern<AtenOpT>::getTypeConverter()
                       ->convertType(op.getType())
                       .template cast<TensorType>();

    Type outElemTy = outType.getElementType();
    if (!outElemTy.isIntOrFloat()) {
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");
    }

    Type rhsAlphaMulElemType;
    if (outElemTy.isa<mlir::FloatType>()) {
      rhsAlphaMulElemType = outElemTy;
    } else {
      // if output type is 64, input type should also be 32
      rhsAlphaMulElemType = rewriter.getIntegerType(32);
    }

    // if right is scalar, rhgType==None, which need to be manually cast to
    // TensorType else right is tensor, rhsType==tensor<i32/i64/f32>
    Value rhsAsTensor;
    if (!rhsType) {
      if (failed(torchScalarToTosaTensor(rewriter, op, op.getOther(),
                                         rhsAsTensor, rhsAlphaMulElemType, {})))
        return rewriter.notifyMatchFailure(
            op, "Currently only scalar constants are supported for "
                "conversion in TOSA operation");
    } else if (rhsType.getElementType() != rhsAlphaMulElemType) {
      // right is tensor, rhsType == tensor<i32/i64/f32>
      // right must be cast to same type as the alpha, so MulOp success
      rhs = rewriter.create<tosa::CastOp>(
          op->getLoc(),
          RankedTensorType::get(rhsType.getShape(), rhsAlphaMulElemType), rhs);
      // reinitialize right value type to tensor<i32/f32>
      rhsType = rhs.getType().dyn_cast<TensorType>();
    }
    auto rhsTensor = rhsType ? rhs : rhsAsTensor;

    // Handle scalar value alpha.
    // It should be either f32/i32
    Value alphaTensor;
    if (failed(torchAlphaToTosaTensor(rewriter, op.getOperation(),
                                      op.getAlpha(), alphaTensor,
                                      rhsAlphaMulElemType,
                                      /*checkForUnity=*/false))) {
      return rewriter.notifyMatchFailure(
          op, "Currently only scalar constants are supported for "
              "alpha in conversion to TOSA operation");
    }

    // make sure input of MulOp is same datetype, otherwise the lowering to
    // arith dialect will bug
    auto multTensor = rewriter.create<tosa::MulOp>(
        op.getLoc(),
        rhsType ? rhsType : RankedTensorType::get({}, rhsAlphaMulElemType),
        rhsTensor, alphaTensor, /*shift=*/0);

    if (outElemTy.isa<mlir::FloatType>() || outElemTy.isInteger(32)) {
      // if outElemTy tensor<f32>, mulTensor must be tensor<f32>,
      //    left value could be tensor<f32/i32/i64>, cast left value to
      //    tensor<f32> type
      // if outElemTy tensor<i32>, mulTensor must be tensor<i32>,
      //    left value could be tensor<f32/i32/i64>, cast left value to
      //    tensor<i32> type
      if (lhsType.getElementType() != rhsAlphaMulElemType)
        lhs = rewriter.create<tosa::CastOp>(
            op.getLoc(),
            RankedTensorType::get(lhsType.getShape(), rhsAlphaMulElemType),
            lhs);

      rewriter.replaceOpWithNewOp<TosaOpT>(op, outType, lhs, multTensor);

      return success();
    } else if (outElemTy.isInteger(64)) {
      // if outElemTy tensor<i64>, mulTensor must be tensor<i32>,
      //    left value could be tensor<f32/i32/i64> type, cast left value to
      //    tensor<i32> type
      if (lhsType.getElementType() != rhsAlphaMulElemType)
        lhs = rewriter.create<tosa::CastOp>(
            op.getLoc(),
            RankedTensorType::get(lhsType.getShape(), rhsAlphaMulElemType),
            lhs);

      auto tosaOpTOutputTensor = rewriter.create<TosaOpT>(
          op.getLoc(),
          RankedTensorType::get(outType.getShape(), rhsAlphaMulElemType), lhs,
          multTensor);
      // cast tensor<i32> back to tensor<i64>
      rewriter.replaceOpWithNewOp<tosa::CastOp>(op, outType,
                                                tosaOpTOutputTensor);

      return success();
    } else {
      return rewriter.notifyMatchFailure(
          op, "Only floating-point, i32, i64 datatype legalization supported");
    }
  }
}; // namespace

// Binary op legalizations for comparator ops.
template <typename AtenOpT, typename TosaOpT>
class ConvertAtenCompareOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().dyn_cast<TensorType>();
    Value rhs = adaptor.getOther();
    auto rhsTy = rhs.getType().dyn_cast<TensorType>();

    if (!lhsTy)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    auto lhsElemTy = lhsTy.getElementType();
    if (!lhsElemTy.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");

    // For bitwise operators, only integer datatype legalization is supported
    constexpr bool isBitwiseOp =
        std::is_same<AtenOpT, AtenBitwiseAndTensorOp>() ||
        std::is_same<AtenOpT, AtenBitwiseOrTensorOp>() ||
        std::is_same<AtenOpT, AtenBitwiseXorTensorOp>();
    if (lhsElemTy.isa<mlir::FloatType>() && isBitwiseOp) {
      return rewriter.notifyMatchFailure(op,
                                         "For bitwise operators, only integer "
                                         "datatype legalization is supported");
    }

    Value rhsAsTensor;
    if (!rhsTy) {
      if (failed(torchScalarToTosaTensor(rewriter, op, op.getOther(),
                                         rhsAsTensor, lhsElemTy, {})))
        return rewriter.notifyMatchFailure(
            op, "Currently only scalar constants are supported for "
                "conversion in TOSA operation");
    }
    auto rhsTensor = rhsTy ? rhs : rhsAsTensor;
    // There is no Lesser operator in TOSA.
    auto swapLhsRhs = (std::is_same<AtenOpT, AtenLtTensorOp>() ||
                       std::is_same<AtenOpT, AtenLtScalarOp>());

    // Promote lhs and rhs dtypes for bitwise operators.
    TensorType resultTy = OpConversionPattern<AtenOpT>::getTypeConverter()
                              ->convertType(op.getType())
                              .template cast<TensorType>();
    if (isBitwiseOp) {
      lhs = tosa::promoteType(rewriter, lhs, resultTy);
      rhsTensor = tosa::promoteType(rewriter, rhsTensor, resultTy);
    }

    auto resultOp = rewriter.create<TosaOpT>(op.getLoc(), resultTy,
                                             (swapLhsRhs ? rhsTensor : lhs),
                                             (swapLhsRhs ? lhs : rhsTensor));

    // There is no NE operator in TOSA.
    if (std::is_same<AtenOpT, AtenNeTensorOp>() ||
        std::is_same<AtenOpT, AtenNeScalarOp>())
      rewriter.replaceOpWithNewOp<tosa::LogicalNotOp>(op, resultTy,
                                                      resultOp.getResult());
    else
      rewriter.replaceOp(op, resultOp.getResult());

    return success();
  }
};

// Binary op legalizations for Mul variants.
template <typename AtenOpT>
class ConvertAtenMulOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    auto lhsType = lhs.getType().dyn_cast<TensorType>();

    if (!lhsType)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    auto outType = OpConversionPattern<AtenOpT>::getTypeConverter()
                       ->convertType(op.getType())
                       .template cast<TensorType>();

    Type outElemTy = outType.getElementType();
    if (!outElemTy.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");

    Value rhsTensor;
    if (std::is_same<AtenOpT, AtenSquareOp>()) {
      rhsTensor = lhs;
    } else {
      Value rhsAsTensor;
      Value rhs = adaptor.getOther();
      auto rhsType = rhs.getType().dyn_cast<TensorType>();
      if (!rhsType) {
        if (failed(torchScalarToTosaTensor(rewriter, op, op.getOther(),
                                           rhsAsTensor, outElemTy, {}))) {
          return rewriter.notifyMatchFailure(
              op, "Currently only scalar constants are supported for "
                  "conversion in TOSA operation");
        }
      }
      rhsTensor = rhsType ? rhs : rhsAsTensor;
    }

    if (outElemTy.isa<mlir::FloatType>() ||
        outElemTy.isa<mlir::IntegerType>()) {
      if (lhsType.getElementType() != outElemTy)
        lhs = rewriter.create<tosa::CastOp>(op.getLoc(), outType, lhs);

      rewriter.replaceOpWithNewOp<tosa::MulOp>(
          op,
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              op.getType()),
          lhs, rhsTensor,
          /*shift=*/0);
      return success();
    }

    // Quantized multiplication may need to rescale inputs.
    return rewriter.notifyMatchFailure(
        op, "Only floating-point or integer datatype "
            "legalization currently supported");
  }
};

template <typename AtenOpT>
class ConvertAtenDivOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().dyn_cast<TensorType>();
    Value rhs = adaptor.getOther();
    auto rhsTy = rhs.getType().dyn_cast<TensorType>();

    if (!lhsTy)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    auto lhsElemTy = lhsTy.getElementType();
    if (!lhsElemTy.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");

    Value rhsAsTensor;
    if (!rhsTy) {
      if (failed(torchScalarToTosaTensor(rewriter, op, op.getOther(),
                                         rhsAsTensor, lhsElemTy, {})))
        return rewriter.notifyMatchFailure(
            op, "Currently only scalar constants are supported for "
                "conversion in TOSA operation");
    }
    auto rhsTensor = rhsTy ? rhs : rhsAsTensor;

    if (lhsElemTy.isa<mlir::FloatType>()) {
      auto rcpOp = rewriter.create<tosa::ReciprocalOp>(
          op->getLoc(), rhsTy ? rhsTy : RankedTensorType::get({}, lhsElemTy),
          rhsTensor);
      rewriter.replaceOpWithNewOp<tosa::MulOp>(
          op,
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              op.getType()),
          lhs, rcpOp.getResult(), /*shift=*/0);
    } else {
      rewriter.replaceOpWithNewOp<tosa::DivOp>(
          op,
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              op.getType()),
          lhs, rhsTensor);
    }
    return success();
  }
};

// This defines a template to construct ops whose legalizations are
// specialized.
template <typename AtenOpT>
class ConvertAtenOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override;
};

template <>
LogicalResult ConvertAtenOp<AtenTanhOp>::matchAndRewrite(
    AtenTanhOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value self = adaptor.getSelf();
  auto selfTy = self.getType().cast<TensorType>();
  if (selfTy && selfTy.getElementType().isa<mlir::FloatType>()) {
    rewriter.replaceOpWithNewOp<tosa::TanhOp>(
        op, getTypeConverter()->convertType(op.getType()), self);
    return success();
  }
  // Sigmoid legalization in TOSA for quantized element-type uses specialized
  // tosa.table construct.
  return rewriter.notifyMatchFailure(
      op, "Only floating-point datatype legalization currently supported");
}

template <>
LogicalResult ConvertAtenOp<AtenSigmoidOp>::matchAndRewrite(
    AtenSigmoidOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value self = adaptor.getSelf();
  auto selfTy = self.getType().cast<TensorType>();
  if (selfTy && selfTy.getElementType().isa<mlir::FloatType>()) {
    rewriter.replaceOpWithNewOp<tosa::SigmoidOp>(
        op, getTypeConverter()->convertType(op.getType()), self);
    return success();
  }
  // Sigmoid legalization in TOSA for quantized element-type uses
  // specialized tosa.table construct.
  return rewriter.notifyMatchFailure(
      op, "Only floating-point datatype legalization currently supported");
}

template <>
LogicalResult ConvertAtenOp<AtenReluOp>::matchAndRewrite(
    AtenReluOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value self = adaptor.getSelf();
  auto selfTy = self.getType().cast<TensorType>();

  // Maps to tosa.clamp which has both int and fp limits.
  int64_t clampMin = 0;
  Value clampIn = self;
  if (!selfTy) {
    return rewriter.notifyMatchFailure(op,
                                       "Only Tensor types supported in TOSA");
  }

  // Rescale the clampIn for quantized types. TBD
  if (!selfTy.getElementType().isa<mlir::FloatType>()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point datatype legalization currently supported");
  }
  rewriter.replaceOpWithNewOp<tosa::ClampOp>(
      op, getTypeConverter()->convertType(op.getType()), clampIn,
      rewriter.getI64IntegerAttr(clampMin),
      rewriter.getI64IntegerAttr(std::numeric_limits<int32_t>::max()),
      rewriter.getF32FloatAttr(0.0f),
      rewriter.getF32FloatAttr(std::numeric_limits<float>::max()));
  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenLeakyReluOp>::matchAndRewrite(
    AtenLeakyReluOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  Value self = adaptor.getSelf();
  auto selfTy = self.getType().cast<TensorType>();
  if (!selfTy.getElementType().isa<mlir::FloatType>()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point datatype legalization currently supported");
  }

  Value alphaScalar = op.getNegativeSlope();
  Value alphaTensor;
  if (failed(torchScalarToTosaTensor(rewriter, op.getOperation(), alphaScalar,
                                     alphaTensor, selfTy.getElementType(), {})))
    return rewriter.notifyMatchFailure(
        op, "Negative slope needs to be a scalar constant for conversion to "
            "TOSA LeakyReLU operation");

  auto zero = tosa::getConstTensor<float>(rewriter, op, 0, {}).value();
  auto cond = rewriter.create<tosa::GreaterEqualOp>(
      op->getLoc(),
      RankedTensorType::get(selfTy.getShape(), rewriter.getIntegerType(1)),
      self, zero);
  auto mulTensor = rewriter.create<tosa::MulOp>(
      op->getLoc(), getTypeConverter()->convertType(op.getType()), self,
      alphaTensor, /*shift=*/0);

  rewriter.replaceOpWithNewOp<tosa::SelectOp>(
      op, getTypeConverter()->convertType(op.getType()), cond, self, mulTensor);

  return success();
}

using ReductionConvFunc = std::optional<Value> (*)(PatternRewriter &,
                                                   Operation *,
                                                   RankedTensorType, Value,
                                                   ElementsAttr, bool);

// They all constitute a common form invoking the appropriate
// converion function in TosaLegalizeCommon.cpp
template <typename AtenOpT, ReductionConvFunc ConversionFuncT>
class ConvertAtenReductionOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  // Each variant must implement corresponding parameter parsing options
  virtual LogicalResult readReduceDimsAndKeepDims(
      AtenOpT op, OpAdaptor adaptor, ConversionPatternRewriter &rewriter,
      ElementsAttr &reduceDimsAttr, bool &keepDims) const {
    return rewriter.notifyMatchFailure(
        op, "Unimplemented reduce_dims and keep_dims parsing function");
  }

  // Common rewriter for all reduction ops, calls the specific implementation of
  // readReduceDimsAndKeepDims() needed for the op variant.
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value self = adaptor.getSelf();
    auto selfTy = self.getType().cast<TensorType>();

    if (!selfTy)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    auto outputTy = OpConversionPattern<AtenOpT>::getTypeConverter()
                        ->convertType(op.getType())
                        .template cast<RankedTensorType>();
    if (!outputTy)
      return rewriter.notifyMatchFailure(
          op, "Only ranked tensor type outputs permitted for reduce_mean");

    ElementsAttr reduceDimsAttr;
    bool keepDims;

    if (failed(readReduceDimsAndKeepDims(op, adaptor, rewriter, reduceDimsAttr,
                                         keepDims)))
      return failure();

    std::optional<Value> result =
        ConversionFuncT(rewriter, op, outputTy, self, reduceDimsAttr, keepDims);

    if (!result)
      return failure();

    // TBD - support dtype casting.

    rewriter.replaceOp(op, {result.value()});

    return success();
  }
};

// This reduction op legalization template handles op variants that have
// explicit reduce_dims dimensions (provided as a list) and keep_dims
// parameters.
template <typename AtenOpT, ReductionConvFunc ConversionFuncT>
class ConvertAtenMultipleDimsReductionOp
    : public ConvertAtenReductionOp<AtenOpT, ConversionFuncT> {
  using ConvertAtenReductionOp<AtenOpT,
                               ConversionFuncT>::ConvertAtenReductionOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readReduceDimsAndKeepDims(AtenOpT op, OpAdaptor adaptor,
                                          ConversionPatternRewriter &rewriter,
                                          ElementsAttr &reduceDimsAttr,
                                          bool &keepDims) const override {
    SmallVector<int64_t, 4> reduceDims;
    if (!matchPattern(op.getDim(), m_TorchListOfConstantInts(reduceDims)))
      return rewriter.notifyMatchFailure(op,
                                         "non-const dim parameter unsupported");
    int64_t N = reduceDims.size();
    auto reduceDimsType = RankedTensorType::get({N}, rewriter.getI64Type());
    reduceDimsAttr = DenseIntElementsAttr::get(reduceDimsType,
                                               llvm::makeArrayRef(reduceDims));

    keepDims = false;
    if (!matchPattern(op.getKeepdim(), m_TorchConstantBool(&keepDims)))
      return rewriter.notifyMatchFailure(
          op, "non-const keepdim parameter unsupported");

    return success();
  }
};

// This reduction op legalization template handles op variants that reduce in
// only one explicit dim which is provided as a number (rather than a list), and
// a keep_dims parameter.
template <typename AtenOpT, ReductionConvFunc ConversionFuncT>
class ConvertAtenOneDimReductionOp
    : public ConvertAtenReductionOp<AtenOpT, ConversionFuncT> {
  using ConvertAtenReductionOp<AtenOpT,
                               ConversionFuncT>::ConvertAtenReductionOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readReduceDimsAndKeepDims(AtenOpT op, OpAdaptor adaptor,
                                          ConversionPatternRewriter &rewriter,
                                          ElementsAttr &reduceDimsAttr,
                                          bool &keepDims) const override {
    int64_t reduceDim;
    if (!matchPattern(op.getDim(), m_TorchConstantInt(&reduceDim)))
      return rewriter.notifyMatchFailure(op,
                                         "non-const dim parameter unsupported");
    auto reduceDimsType = RankedTensorType::get({1}, rewriter.getI64Type());
    reduceDimsAttr = DenseIntElementsAttr::get(reduceDimsType,
                                               llvm::makeArrayRef({reduceDim}));

    keepDims = false;
    if (!matchPattern(op.getKeepdim(), m_TorchConstantBool(&keepDims)))
      return rewriter.notifyMatchFailure(
          op, "non-const keepdim parameter unsupported");

    return success();
  }
};

// This reduction op legalization template handles op variants that reduce all
// dims does not keep dims.
template <typename AtenOpT, ReductionConvFunc ConversionFuncT>
class ConvertAtenAllDimsReductionOp
    : public ConvertAtenReductionOp<AtenOpT, ConversionFuncT> {
public:
  using ConvertAtenReductionOp<AtenOpT,
                               ConversionFuncT>::ConvertAtenReductionOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readReduceDimsAndKeepDims(AtenOpT op, OpAdaptor adaptor,
                                          ConversionPatternRewriter &rewriter,
                                          ElementsAttr &reduceDimsAttr,
                                          bool &keepDims) const override {
    auto self = adaptor.getSelf();
    auto selfTy = self.getType().template cast<RankedTensorType>();

    // Select all dims to reduce
    SmallVector<int64_t, 4> reduceDims;
    for (int64_t i = 0; i < selfTy.getRank(); i++)
      reduceDims.push_back(i);
    int64_t N = selfTy.getRank();
    auto reduceDimsType = RankedTensorType::get({N}, rewriter.getI64Type());
    reduceDimsAttr = DenseIntElementsAttr::get(reduceDimsType,
                                               llvm::makeArrayRef(reduceDims));
    keepDims = false;

    return success();
  }
};

template <>
LogicalResult ConvertAtenOp<AtenArgmaxOp>::matchAndRewrite(
    AtenArgmaxOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  Value self = adaptor.getSelf();
  auto selfTy = self.getType().template cast<RankedTensorType>();

  if (!selfTy)
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types supported in TOSA argmax");

  int64_t reduceDim;
  if (!matchPattern(op.getDim(), m_TorchConstantInt(&reduceDim))) {
    // NoneType indicates reduce on all dims
    reduceDim = -1;
  }

  bool keepDim = false;
  if (!matchPattern(op.getKeepdim(), m_TorchConstantBool(&keepDim)))
    return rewriter.notifyMatchFailure(
        op, "non-const keepdim parameter unsupported");

  auto resultTy = getTypeConverter()
                      ->convertType(op.getResult().getType())
                      .cast<RankedTensorType>();
  auto outputETy = resultTy.getElementType();

  // Create a single instance of tosa.argmax.
  // Multiple dims require chained construct.
  auto buildArgmax = [&](int64_t reduceDim, Value input) -> Value {
    auto inputTy = input.getType().cast<RankedTensorType>();
    auto inputShape = makeShapeTorchCompatible(inputTy.getShape());
    SmallVector<int64_t> outputShapeArr = {};
    int32_t i = 0;

    for (auto &dim : inputShape) {
      if (i++ != reduceDim) {
        outputShapeArr.push_back(dim);
      } else {
        if (keepDim)
          outputShapeArr.push_back(1);
      }
    }

    // Tosa argmax output is i32, while Torch backend mandates i64.
    auto outputReduceTy = RankedTensorType::get(
        makeShapeLLVMCompatible(ArrayRef<int64_t>(outputShapeArr)),
        rewriter.getI32Type());
    auto reduceDimAttr =
        rewriter.getIntegerAttr(rewriter.getI64Type(), reduceDim);
    return rewriter
        .create<tosa::ArgMaxOp>(op->getLoc(),
                                getTypeConverter()->convertType(outputReduceTy),
                                input, reduceDimAttr)
        .getResult();
  };

  // Convert the final index to i64 for backend finalization, However, i64
  // is not a defined type for tosa.cast, so using arith.extsi instead.
  auto castToInt64 = [&](Value result) -> LogicalResult {
    auto resTy = result.getType().cast<ShapedType>();
    if (!resTy)
      return rewriter.notifyMatchFailure(op,
                                         "Argmax: Result is not a shaped type");

    auto resShape = makeShapeTorchCompatible(resTy.getShape());
    auto outTy =
        RankedTensorType::get(makeShapeLLVMCompatible(resShape), outputETy);

    rewriter.replaceOpWithNewOp<arith::ExtSIOp>(
        op, getTypeConverter()->convertType(outTy), result);

    return success();
  };

  if (reduceDim == -1) { // reducing on all dims
    Value input = self;
    for (int dim = 0; dim < selfTy.getRank(); dim++) {
      // progressively reduce each 0-th dim
      input = buildArgmax(0, input);
    }
    return castToInt64(input);
  } else {
    return castToInt64(buildArgmax(reduceDim, self));
  }

  return success();
}

template <typename AtenOpT>
class ConvertAtenSqueezeOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  // Each variant must implement corresponding parameter parsing options
  virtual LogicalResult
  generateSqueezedShape(AtenOpT op, RankedTensorType selfTy,
                        ConversionPatternRewriter &rewriter,
                        SmallVector<int64_t> &squeezedShape) const {
    return rewriter.notifyMatchFailure(
        op, "Unimplemented dim/dim-list parsing function");
  }

  // Common rewriter for all squeeze ops, calls the specific implementation of
  // generateSqueezedShape() needed for the op variant.
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value self = adaptor.getSelf();
    auto selfTy = self.getType().template cast<RankedTensorType>();

    if (!selfTy)
      return rewriter.notifyMatchFailure(
          op, "Only ranked tensor types supported in TOSA argmax");

    SmallVector<int64_t> newOutputShape;
    if (failed(generateSqueezedShape(op, selfTy, rewriter, newOutputShape)))
      return rewriter.notifyMatchFailure(op,
                                         "Squeeze could not compute new shape");

    auto resultTy = OpConversionPattern<AtenOpT>::getTypeConverter()
                        ->convertType(op.getResult().getType())
                        .template cast<RankedTensorType>();
    auto resultElemTy = resultTy.getElementType();

    auto newOutputTy = RankedTensorType::get(
        makeShapeLLVMCompatible(newOutputShape), resultElemTy);

    auto reshapeOp = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(),
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            newOutputTy),
        self, rewriter.getDenseI64ArrayAttr(newOutputShape));
    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        op,
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            newOutputTy),
        reshapeOp);

    return success();
  }
};

template <typename AtenOpT>
class ConvertAtenSqueezeOneDimOp : public ConvertAtenSqueezeOp<AtenOpT> {
  using ConvertAtenSqueezeOp<AtenOpT>::ConvertAtenSqueezeOp;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  generateSqueezedShape(AtenOpT op, RankedTensorType selfTy,
                        ConversionPatternRewriter &rewriter,
                        SmallVector<int64_t> &squeezedShape) const override {
    int64_t squeezeDim;
    if (!matchPattern(op.getDim(), m_TorchConstantInt(&squeezeDim)))
      return rewriter.notifyMatchFailure(op,
                                         "non-const dim parameter unsupported");

    // Handle negative dim
    if (squeezeDim < 0)
      squeezeDim = squeezeDim + selfTy.getRank();

    auto selfShape = makeShapeTorchCompatible(selfTy.getShape());

    // Only dims statically known to have size=1 are reduced.
    // Dynamic dims are treated as unknowns and will not be squeezed
    // even if dim parameter says it should be.
    uint32_t dimNum = 0;
    for (auto &dim : selfShape) {
      if (dim != 1 || squeezeDim != dimNum)
        squeezedShape.push_back(dim);
      dimNum++;
    }

    return success();
  }
};

template <typename AtenOpT>
class ConvertAtenSqueezeAllDimsOp : public ConvertAtenSqueezeOp<AtenOpT> {
  using ConvertAtenSqueezeOp<AtenOpT>::ConvertAtenSqueezeOp;
  using OpAdaptor = typename AtenOpT::Adaptor;

  LogicalResult
  generateSqueezedShape(AtenOpT op, RankedTensorType selfTy,
                        ConversionPatternRewriter &rewriter,
                        SmallVector<int64_t> &squeezedShape) const override {
    auto selfShape = makeShapeTorchCompatible(selfTy.getShape());

    // Dims that may dynamically resolve to 1 are not reduced here. Only
    // compile-time resolvable dims are handled here.
    for (auto &dim : selfShape) {
      if (dim != 1)
        squeezedShape.push_back(dim);
    }
    return success();
  }
};

template <>
LogicalResult ConvertAtenOp<AtenPowTensorScalarOp>::matchAndRewrite(
    AtenPowTensorScalarOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  Value self = adaptor.getSelf();
  auto selfTy = self.getType().template cast<RankedTensorType>();

  if (!selfTy)
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types supported in TOSA Pow");

  if (!selfTy.getElementType().isa<mlir::FloatType>())
    return rewriter.notifyMatchFailure(
        op, "Only floating-point datatype legalization supported");

  Value expTensor;
  Value expScalar = op.getExponent();
  if (failed(torchScalarToTosaTensor(rewriter, op, expScalar, expTensor,
                                     selfTy.getElementType(), {})))
    return rewriter.notifyMatchFailure(
        op, "Currently only scalar constants are supported for "
            "conversion in TOSA Pow operation");

  rewriter.replaceOpWithNewOp<tosa::PowOp>(
      op, getTypeConverter()->convertType(op.getType()), self, expTensor);

  return success();
}

// Perform the basic n-dim matmul operation encompassing the handling of
// broadcasting and dynamic shape propagation.
// All PyTorch ops that leverage matrix multiplication will derive this and
// implement their specialized input processing (e.g transpose), and output
// processing, e.g. GEMM or fully connected bias handling.
template <typename AtenOpT>
class ConvertAtenMatmulBaseOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  // Each variant must implement corresponding parameter parsing options.
  // Maintain separate input read functions for each variant because it is not
  // necessarily true with all variants that the first two operands are the lhs
  // and rhs.
  virtual LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                         ConversionPatternRewriter &rewriter,
                                         Value &lhs, Value &rhs) const {
    return rewriter.notifyMatchFailure(
        op,
        "Unimplemented matrix multiplication variant input parsing function");
  }
  LogicalResult performMatmul(AtenOpT op, OpAdaptor adaptor,
                              ConversionPatternRewriter &rewriter, Value &lhs,
                              Value &rhs, Value &output) const {

    auto lhsTy = lhs.getType().cast<RankedTensorType>();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    auto lhsRank = lhsTy.getRank();
    auto rhsRank = rhsTy.getRank();

    auto lhsShape = makeShapeTorchCompatible(lhsTy.getShape());
    auto rhsShape = makeShapeTorchCompatible(rhsTy.getShape());

    auto lhsElemTy = lhsTy.getElementType();
    auto rhsElemTy = rhsTy.getElementType();

    if (lhsElemTy != rhsElemTy)
      return rewriter.notifyMatchFailure(op,
                                         "Matmul: input datatypes mismatched");

    // Legalization constructs may offer input shapes but expect output shapes
    // to be inferred, e.g.
    // func @forward(%arg0: !torch.vtensor<[14,19],f32>,
    //               %arg1: !torch.vtensor<[19,28],f32>) ->
    //               !torch.vtensor<[?,?],f32>
    // This is tricky with matmul, since TOSA matmul is on 3D inputs.
    // This means the need to reshape potentially both inputs and outputs,
    // and reshape to unknown shape is undefined.

    auto maxInputRank = lhsRank > rhsRank ? lhsRank : rhsRank;
    // If performing dot product on vectors, the RHS is synthetically transposed
    if (maxInputRank == 1)
      maxInputRank++;

    // Obtaining the rank broadcasted shapes of tensors makes it easier to
    // construct the input and output reshaping logic.
    auto getRankBroadcastedShape = [&](Value tensor,
                                       bool isRHS) -> SmallVector<int64_t> {
      auto tensorTy = tensor.getType().cast<TensorType>();
      auto tensorShape = makeShapeTorchCompatible(tensorTy.getShape());
      auto tensorRank = tensorTy.getRank();

      SmallVector<int64_t> bcastedShape;

      auto bcastDims = maxInputRank - tensorRank;

      if (isRHS && (tensorRank == 1) && bcastDims) {
        // RHS with rank1 is special. It be synthetically transposed to dim[:-2]
        for (int32_t i = 0; i < bcastDims - 1; i++)
          bcastedShape.push_back(1);
        bcastedShape.push_back(tensorShape[0]);
        bcastedShape.push_back(1);
      } else {
        if (bcastDims > 0) { // rank broadcast
          for (uint32_t i = 0; i < bcastDims; i++)
            bcastedShape.push_back(1);
        }
        for (auto &dim : tensorShape)
          bcastedShape.push_back(dim);
      }
      return bcastedShape;
    };

    // Step: Rank broadcast the two inputs.
    auto lhsBroadcastedShape = getRankBroadcastedShape(lhs, false);
    auto lhsBroadcastedTy = RankedTensorType::get(
        makeShapeLLVMCompatible(lhsBroadcastedShape), lhsElemTy);
    auto rhsBroadcastedShape = getRankBroadcastedShape(rhs, true);
    auto rhsBroadcastedTy = RankedTensorType::get(
        makeShapeLLVMCompatible(rhsBroadcastedShape), rhsElemTy);

    auto rankBroadcastedLhs =
        lhsRank == maxInputRank
            ? lhs
            : rewriter.create<tosa::ReshapeOp>(
                  op->getLoc(),
                  OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
                      lhsBroadcastedTy),
                  lhs, rewriter.getDenseI64ArrayAttr(lhsBroadcastedShape));

    auto rankBroadcastedRhs =
        rhsRank == maxInputRank
            ? rhs
            : rewriter.create<tosa::ReshapeOp>(
                  op->getLoc(),
                  OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
                      rhsBroadcastedTy),
                  rhs, rewriter.getDenseI64ArrayAttr(rhsBroadcastedShape));

    // TOSA matmul is performed on two 3D inputs and generates a 3D output.
    // Lower ranked tensors are dim-1 reshaped up to 3D
    auto reshapeUpTo3DTensor = [&](Value tensor) -> Value {
      auto tensorTy = tensor.getType().cast<TensorType>();
      auto rank = tensorTy.getRank();

      assert(rank <= 3 && "reshapeUpTo3D tensor must receive rank <= 3");
      if (rank == 3)
        return tensor;

      auto shape = makeShapeTorchCompatible(tensorTy.getShape());
      SmallVector<int64_t> newShape({1, 1, 1});

      if (rank == 2) { // batchsize = 1
        newShape[1] = shape[0];
        newShape[2] = shape[1];
      } else { // rank 1
        newShape[2] = shape[0];
      }
      auto newType = RankedTensorType::get(makeShapeLLVMCompatible(newShape),
                                           tensorTy.getElementType());

      return rewriter.create<tosa::ReshapeOp>(
          op->getLoc(),
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              newType),
          tensor, rewriter.getDenseI64ArrayAttr(newShape));
    };

    // Where broadcasting is required in one or more batch dims, the following
    // is done.
    // Where all batch dims are involved in broadcasting:
    // Given A: 3x1x5x6 and B: 1x4x6x7
    // 1. Reshape A to 1x15x6 (squeeze all batchdims into dim1)
    // 2. Transpose B to 6x1x4x7, Reshape to 1x6x28
    // 3. tosa.Matmul 1x15x6 1x6x28 = 1x15x28
    // 4. Reshape out to 3x5x4x7, Transpose to 3x4x5x7
    // Where there are batch dimensions that are broadcast and not, the
    // treatment is to have dim0 correspond to product of all non-broadcast
    // dimsizes:
    // Given A: 4x8x16x32 B: 8x32x17
    // 1. Reshape A to 8x64x32 (squeeze all unbroadcasted dims into dim0,
    // broadcasted dims into dim1)
    // 2. No transpose or reshape of B as its batchdims are not broadcast to.
    // 3. tosa.Matmul 8x64x32 8x32x17 = 8x64x17
    // 4. Reshape to 8x4x16x17, Transpose to 4x8x16x17

    // Check if we need to perform the broadcast on batch dim
    // Not needed if max rank < 3, or if maxrank == 3 and dim[0] matches
    auto needsBatchDimBroadcast = [&]() -> bool {
      if (maxInputRank < 3) {
        return false;
      } else {
        if (maxInputRank == 3 &&
            lhsBroadcastedShape[0] == rhsBroadcastedShape[0]) {
          return false;
        }
        return true;
      }
    };

    auto performBatchDimBroadcast = needsBatchDimBroadcast();

    // Inputs to the tosa.matmul
    Value matmulLhs, matmulRhs;

    using TensorShape_t = struct {
      int64_t dim;
      int64_t shape;
    };

    // Transpose needs to done if transposeDims are not non-monotonically
    // increasing. E.g. [0, 1, 2, 3]: No transpose [1, 0, 2, 3]: Transpose dim0
    // and dim1 The order need not be sequential, since one or more dims may
    // have been removed due to broadcasting.
    auto isTransposeRequired = [](SmallVector<int32_t> transposedDims) -> bool {
      int32_t lastDim = -1;
      for (auto &dim : transposedDims) {
        if (lastDim > dim)
          return true;
        lastDim = dim;
      }
      return false;
    };

    SmallVector<TensorShape_t> commonElems, lhsSqueezedElems, rhsSqueezedElems;

    if (!performBatchDimBroadcast) {
      // Simple with no broadcasting artifacts. Just reshape up to 3D
      matmulLhs = reshapeUpTo3DTensor(rankBroadcastedLhs);
      matmulRhs = reshapeUpTo3DTensor(rankBroadcastedRhs);

    } else {
      // In this case, either or both input matrices involve broadcasting on
      // their batch dimensions. For example:
      // 4x5x6, 1x6x7 -> 4x5x7
      // 4x1x5x6, 1x3x6x7 -> 4x3x5x7
      // Though maxInputRank is necessarily >=3 here, individual matrices may be
      // lower rank.
      // E.g. 3x4x5x6, 6 -> 3x4x5

      // These are the accumulated products of the shape of each dim:
      // 1. common dimensions: upper dimensions (dims other than two rightmost)
      // whose shapes are the same for both LHS and RHS.
      // 2. LHS squeezed dimensions: all dimensions of LHS that involve
      // broadcasting in either direction, plus the LHS[-2] shape
      // 3. RHS squeezed dimensions: all dimensions of RHS that involve
      // broadcasting in either direction, plus the RHS[-1] shape
      int64_t commonValue = 1, lhsSqueezedValue = 1, rhsSqueezedValue = 1;

      // For both LHS and RHS, the dimensions are separated into the common,
      // squeezed and remaining dim. E.g. given
      // LHS = 3x4x5x6
      // RHS = 1x4x6x7
      // common = {{dim=1, shape=4}}
      // lhs squeezed = {{dim=0, shape=3},
      //                 {dim=2, shape=5}}
      // rhs squeezed = {{dim=0, shape=1},
      //                 {dim=2, shape=7}}
      // The matmul dim is LHS[-1] and RHS[-2], i.e. 6.
      // Once this is obtained, LHS and RHS are expressed as:
      // LHS = {common, lhs_squeezed, matmul_dim}
      // RHS = {common, matmul_dim, rhs_squeezed}
      // The matmul is then performed to obtain output:
      // matmul_out = {common, lhs_squeezed, rhs_squeezed}
      // Finally, we reshape to 'unsqueeze' the LHS and RHS parts and transpose
      // them back to their correct positions.

      SmallVector<int64_t> transposedLhsShape;
      SmallVector<int32_t> transposedLhsDims;

      // Step: generate the common dim/shape information
      bool hasDynamicDims = false;
      for (uint32_t dim = 0; dim < maxInputRank - 2; dim++) {
        bool isDynamicDim = ShapedType::isDynamic(lhsBroadcastedShape[dim]);
        hasDynamicDims |= isDynamicDim;
        if (isDynamicDim ||
            lhsBroadcastedShape[dim] == rhsBroadcastedShape[dim]) {
          commonValue *= lhsBroadcastedShape[dim];
          commonElems.push_back({dim, lhsBroadcastedShape[dim]});
        }
      }
      commonValue = commonValue < 0 ? kUnknownSize : commonValue;

      // TODO: Handle the case when there are dynamic batch dimensions.
      if (hasDynamicDims)
        commonValue = kUnknownSize;

      // Step: generate the LHS squeezed dim/shape information.
      for (uint32_t dim = 0; dim < maxInputRank - 2; dim++) {
        bool isDynamicDim = ShapedType::isDynamic(lhsBroadcastedShape[dim]);
        if (!isDynamicDim &&
            lhsBroadcastedShape[dim] != rhsBroadcastedShape[dim]) {
          lhsSqueezedValue *= lhsBroadcastedShape[dim];
          lhsSqueezedElems.push_back({dim, lhsBroadcastedShape[dim]});
        }
      }
      // including LHS[-2]
      lhsSqueezedElems.push_back(
          {maxInputRank - 2, lhsBroadcastedShape[maxInputRank - 2]});
      lhsSqueezedValue *= lhsBroadcastedShape[maxInputRank - 2];
      lhsSqueezedValue = lhsSqueezedValue < 0 ? kUnknownSize : lhsSqueezedValue;

      // Step: Create the tosa.transpose array. If this array has a
      // non-monotonic series of dims, perform transpose.
      // First the common_elems
      for (uint32_t i = 0; i < commonElems.size(); i++) {
        transposedLhsShape.push_back(commonElems[i].shape);
        transposedLhsDims.push_back(commonElems[i].dim);
      }
      // then the lhs_squeezed elems
      for (uint32_t i = 0; i < lhsSqueezedElems.size(); i++) {
        transposedLhsShape.push_back(lhsSqueezedElems[i].shape);
        transposedLhsDims.push_back(lhsSqueezedElems[i].dim);
      }
      // then the final dim
      transposedLhsDims.push_back(maxInputRank - 1);
      transposedLhsShape.push_back(lhsBroadcastedShape[maxInputRank - 1]);

      bool lhsNeedsTranspose = isTransposeRequired(transposedLhsDims);

      auto lhsReshapeInput = rankBroadcastedLhs;

      if (lhsNeedsTranspose) {
        auto transposedLhsType = RankedTensorType::get(
            makeShapeLLVMCompatible(transposedLhsShape), rhsElemTy);

        std::optional<Value> transposedLhsDimsConst =
            tosa::getConstTensor<int32_t>(
                rewriter, op,
                /*vec=*/transposedLhsDims,
                /*shape=*/{static_cast<int32_t>(transposedLhsDims.size())});

        lhsReshapeInput =
            rewriter
                .create<tosa::TransposeOp>(
                    op->getLoc(),
                    OpConversionPattern<AtenOpT>::getTypeConverter()
                        ->convertType(transposedLhsType),
                    rankBroadcastedLhs, transposedLhsDimsConst.value())
                .getResult();
      }

      // LHS = {common, lhs_squeezed, matmul_dim}
      SmallVector<int64_t> newLhsShape(
          {1, 1, lhsBroadcastedShape[maxInputRank - 1]});
      newLhsShape[0] = commonValue;
      newLhsShape[1] = hasDynamicDims ? kUnknownSize : lhsSqueezedValue;

      auto newLhsType = RankedTensorType::get(
          makeShapeLLVMCompatible(newLhsShape), lhsElemTy);

      matmulLhs = rewriter.create<tosa::ReshapeOp>(
          op->getLoc(),
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              newLhsType),
          lhsReshapeInput, rewriter.getDenseI64ArrayAttr(newLhsShape));

      SmallVector<int64_t> transposedRhsShape;
      SmallVector<int32_t> transposedRhsDims;

      // Step: Create the RHS transpose sequence
      // RHS = {common, matmul_dim, rhs_squeezed}
      // first the common_dims
      for (uint32_t i = 0; i < commonElems.size(); i++) {
        transposedRhsShape.push_back(commonElems[i].shape);
        transposedRhsDims.push_back(commonElems[i].dim);
      }
      // The matmul_dim of RHS
      transposedRhsDims.push_back(maxInputRank - 2);
      transposedRhsShape.push_back(rhsBroadcastedShape[maxInputRank - 2]);
      // finally all the rhs_squeeze dims
      hasDynamicDims = false;
      for (uint32_t dim = 0; dim < maxInputRank - 2; dim++) {
        bool isDynamicDim = ShapedType::isDynamic(rhsBroadcastedShape[dim]);
        hasDynamicDims |= isDynamicDim;
        if (!isDynamicDim &&
            rhsBroadcastedShape[dim] != lhsBroadcastedShape[dim]) {
          rhsSqueezedElems.push_back({dim, rhsBroadcastedShape[dim]});
          rhsSqueezedValue *= rhsBroadcastedShape[dim];
        }
      }
      rhsSqueezedElems.push_back(
          {maxInputRank - 1, rhsBroadcastedShape[maxInputRank - 1]});
      rhsSqueezedValue *= rhsBroadcastedShape[maxInputRank - 1];
      for (uint32_t i = 0; i < rhsSqueezedElems.size(); i++) {
        transposedRhsShape.push_back(rhsSqueezedElems[i].shape);
        transposedRhsDims.push_back(rhsSqueezedElems[i].dim);
      }

      auto transposedRhsType = RankedTensorType::get(
          makeShapeLLVMCompatible(transposedRhsShape), rhsElemTy);

      if (hasDynamicDims)
        rhsSqueezedValue = kUnknownSize;

      SmallVector<int64_t> newRhsShape(
          {commonValue < 0 ? kUnknownSize : commonValue,
           rhsBroadcastedShape[maxInputRank - 2], rhsSqueezedValue});
      auto newRhsType = RankedTensorType::get(
          makeShapeLLVMCompatible(newRhsShape), rhsElemTy);

      bool rhsNeedsTranspose = isTransposeRequired(transposedRhsDims);

      auto transposedRhsValue = rankBroadcastedRhs;

      if (rhsNeedsTranspose) {
        std::optional<Value> transposedRhsDimsConst =
            tosa::getConstTensor<int32_t>(
                rewriter, op,
                /*vec=*/transposedRhsDims,
                /*shape=*/{static_cast<int32_t>(transposedRhsDims.size())});

        transposedRhsValue =
            rewriter
                .create<tosa::TransposeOp>(
                    op->getLoc(),
                    OpConversionPattern<AtenOpT>::getTypeConverter()
                        ->convertType(transposedRhsType),
                    rankBroadcastedRhs, transposedRhsDimsConst.value())
                .getResult();
      }

      // reshape
      matmulRhs = rewriter.create<tosa::ReshapeOp>(
          op->getLoc(),
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              newRhsType),
          transposedRhsValue, rewriter.getDenseI64ArrayAttr(newRhsShape));
    }

    auto matmulLhsShape = makeShapeTorchCompatible(
        matmulLhs.getType().template cast<RankedTensorType>().getShape());
    auto matmulRhsShape = makeShapeTorchCompatible(
        matmulRhs.getType().template cast<RankedTensorType>().getShape());

    // The reshape/transpose should ensure the tosa.matmul always has same
    // batch size for either matrix. If if shapes are dynamic, they'll be
    // appropriately handled.
    assert(matmulLhsShape[0] == matmulRhsShape[0] &&
           "tosa.matmul needs same batchsize on LHS and RHS");

    SmallVector<int64_t> matmulOutputShape(
        {matmulLhsShape[0], matmulLhsShape[1], matmulRhsShape[2]});
    Type outputElemTy;
    if (lhsElemTy.isa<mlir::FloatType>()) {
      outputElemTy = lhsElemTy;
    } else { // qint8 emits i32 matmul output
      outputElemTy = rewriter.getIntegerType(32);
    }

    auto mmOutputTy = RankedTensorType::get(
        makeShapeLLVMCompatible(matmulOutputShape), outputElemTy);
    auto mmOpResult =
        rewriter
            .create<tosa::MatMulOp>(
                op->getLoc(),
                OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
                    mmOutputTy),
                matmulLhs, matmulRhs)
            .getResult();

    // Perform the reshape to output shape. This is always required unless max
    // input rank=3 and there was no broadcasting, in which case the tosa.matmul
    // output itself is correctly shaped.
    bool performOpReshape = !(maxInputRank == 3 && !performBatchDimBroadcast);

    if (performOpReshape) {
      // Since the output shape may be unknown, we construct it
      // independently and reshape. Otherwise reshape may be expressed for
      // an unknown to-be-inferred output shape. The final tensor.cast
      // reshapes the known shape to the desired output shape.
      auto computeOpShape = [&](SmallVector<int64_t> &reshapedOpShape,
                                SmallVector<int32_t> &transposedOpDims,
                                SmallVector<int64_t> &transposedOpShapes) {
        if (maxInputRank == 1)
          return;

        if (maxInputRank == 2) {
          if (lhsRank == 2)
            reshapedOpShape.push_back(lhsShape[0]);
          if (rhsRank == 2)
            reshapedOpShape.push_back(rhsShape[1]);
          return;
        }

        // Step: Construct the output transpose/reshape information
        // First the common_dims
        for (uint32_t i = 0; i < commonElems.size(); i++) {
          reshapedOpShape.push_back(commonElems[i].shape);
          transposedOpDims.push_back(commonElems[i].dim);
        }

        // Then the LHS squeezed dims
        for (uint32_t i = 0; i < lhsSqueezedElems.size() - 1; i++) {
          // Only dims that don't broadcast - broadcasting ones come from the
          // other input.
          if (lhsSqueezedElems[i].shape != 1) {
            reshapedOpShape.push_back(lhsSqueezedElems[i].shape);
            transposedOpDims.push_back(lhsSqueezedElems[i].dim);
          }
        }
        // The last squeezed dim is lhs[-2] which needs to be
        // checked separately for broadcasting
        if (lhsRank > 1) {
          reshapedOpShape.push_back(lhsBroadcastedShape[maxInputRank - 2]);
          transposedOpDims.push_back(maxInputRank - 2);
        }

        // then the RHS squeezed dims except rhs[-1] which is handled like
        // lhs[-2]
        for (uint32_t i = 0; i < rhsSqueezedElems.size() - 1; i++) {
          if (rhsSqueezedElems[i].shape != 1) {
            reshapedOpShape.push_back(rhsSqueezedElems[i].shape);
            transposedOpDims.push_back(rhsSqueezedElems[i].dim);
          }
        }
        // rhs[-1]
        if (rhsRank > 1) {
          reshapedOpShape.push_back(rhsBroadcastedShape[maxInputRank - 1]);
          transposedOpDims.push_back(maxInputRank - 1);
        }

        // Final transposed output shape construction
        for (uint32_t i = 0; i < maxInputRank - 2; i++) {
          if (lhsBroadcastedTy.isDynamicDim(i)) {
            transposedOpShapes.push_back(kUnknownSize);
          } else {
            if (lhsBroadcastedShape[i] == rhsBroadcastedShape[i]) {
              transposedOpShapes.push_back(lhsBroadcastedShape[i]);
            } else {
              transposedOpShapes.push_back(lhsBroadcastedShape[i] == 1
                                               ? rhsBroadcastedShape[i]
                                               : lhsBroadcastedShape[i]);
            }
          }
        }
        if (lhsRank > 1)
          transposedOpShapes.push_back(lhsBroadcastedShape[maxInputRank - 2]);
        if (rhsRank > 1)
          transposedOpShapes.push_back(rhsBroadcastedShape[maxInputRank - 1]);

        return;
      };

      SmallVector<int64_t> reshapedOpShape, transposedOpShape;
      SmallVector<int32_t> transposedOpDims;

      computeOpShape(reshapedOpShape, transposedOpDims, transposedOpShape);

      bool opNeedsTranspose = isTransposeRequired(transposedOpDims);

      // Perform reshape
      auto reshapedOpType = RankedTensorType::get(
          makeShapeLLVMCompatible(reshapedOpShape), outputElemTy);
      auto reshapedOp = rewriter.create<tosa::ReshapeOp>(
          op->getLoc(),
          OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
              reshapedOpType),
          mmOpResult, rewriter.getDenseI64ArrayAttr(reshapedOpShape));

      if (opNeedsTranspose) {

        std::optional<Value> transposedOpShapeConst =
            tosa::getConstTensor<int32_t>(
                rewriter, op,
                /*vec=*/transposedOpDims,
                /*shape=*/{static_cast<int32_t>(transposedOpDims.size())});

        auto transposedOpType = RankedTensorType::get(
            makeShapeLLVMCompatible(transposedOpShape), outputElemTy);
        output = rewriter
                     .create<tosa::TransposeOp>(
                         op->getLoc(),
                         OpConversionPattern<AtenOpT>::getTypeConverter()
                             ->convertType(transposedOpType),
                         reshapedOp.getResult(), transposedOpShapeConst.value())
                     .getResult();

      } else {
        output = reshapedOp.getResult();
      }
    } else {
      output = mmOpResult;
    }

    return success();
  }
  // The default version just reads two inputs, computes output and returns it.
  // Other versions may add a bias, apply GEMM-style alpha/beta scaling etc.
  virtual LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    Value lhs, rhs;

    if (failed(readMatMulInputs(op, adaptor, rewriter, lhs, rhs)))
      return rewriter.notifyMatchFailure(op, "Failed to read matmul inputs");

    Value output;

    if (failed(performMatmul(op, adaptor, rewriter, lhs, rhs, output)))
      return rewriter.notifyMatchFailure(op,
                                         "Failed to perform matmul operation");

    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        op,
        OpConversionPattern<AtenOpT>::getTypeConverter()
            ->convertType(op.getType())
            .template cast<RankedTensorType>(),
        output);

    return success();
  }
};

// Legalizes the torch.matmul op for general n-dim matmul.
template <typename AtenOpT>
class ConvertAtenMatMulOp : public ConvertAtenMatmulBaseOp<AtenOpT> {
public:
  using ConvertAtenMatmulBaseOp<AtenOpT>::ConvertAtenMatmulBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                 ConversionPatternRewriter &rewriter,
                                 Value &lhs, Value &rhs) const override {
    lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();

    rhs = adaptor.getOther();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    if (!lhsTy || !rhsTy)
      return rewriter.notifyMatchFailure(
          op, "Only ranked tensor types supported in TOSA matmul");

    return success();
  }
};

// Implements handling of aten.mm and aten.bmm ops.
template <typename AtenOpT>
class ConvertAtenMmOp : public ConvertAtenMatmulBaseOp<AtenOpT> {
public:
  using ConvertAtenMatmulBaseOp<AtenOpT>::ConvertAtenMatmulBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                 ConversionPatternRewriter &rewriter,
                                 Value &lhs, Value &rhs) const override {

    lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();

    rhs = adaptor.getMat2();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    if (!lhsTy || !rhsTy)
      return rewriter.notifyMatchFailure(
          op, "Only ranked tensor types supported in TOSA matmul");

    auto lhsRank = lhsTy.getRank();
    auto rhsRank = rhsTy.getRank();

    if (isa<AtenMmOp>(op)) {
      // Mm takes two 2D tensors.
      if (lhsRank != 2 || rhsRank != 2)
        return op.emitError("aten.mm called but matrix rank != 2");
    } else if (isa<AtenBmmOp>(op)) {
      // Bmm takes two 3D tensors.
      if (lhsRank != 3 || rhsRank != 3)
        return op.emitError("aten.bmm called but matrix rank != 3");
    }

    return success();
  }
};

// Implements handling of aten.linear op.
template <typename AtenOpT>
class ConvertAtenLinearOp : public ConvertAtenMatmulBaseOp<AtenOpT> {
public:
  using ConvertAtenMatmulBaseOp<AtenOpT>::ConvertAtenMatmulBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                 ConversionPatternRewriter &rewriter,
                                 Value &lhs, Value &rhs) const override {

    lhs = adaptor.getInput();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();

    rhs = adaptor.getWeight();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    if (!lhsTy || !rhsTy)
      return rewriter.notifyMatchFailure(
          op, "Only ranked tensor types supported in TOSA matmul");

    auto lhsRank = lhsTy.getRank();
    auto rhsRank = rhsTy.getRank();

    if (lhsRank != 2 && lhsRank != 3)
      return op.emitError("aten.Linear called but input rank not 2 or 3");
    if (rhsRank != 2 && rhsRank != 3)
      return op.emitError("aten.Linear called but weight rank not 2 or 3");

    // Protection against crash due to unguarded code in TOSA->LinAlg.
    // TODO: This should be handled in TOSA->LinAlg instead.
    if (!lhsTy.hasStaticShape() || !rhsTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "aten.Linear needs statically shaped input");

    return success();
  }
  // Override the default rewriter to perform RHS transpose and bias addition as
  // well.
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    Value lhs, rhs;

    if (failed(readMatMulInputs(op, adaptor, rewriter, lhs, rhs)))
      return rewriter.notifyMatchFailure(op, "Failed to read matmul inputs");

    // The aten.Linear op has a bias tensor that is added to the matmul output.
    auto bias = adaptor.getBias();
    auto biasTy = bias.getType();

    // TOSA does not mandate that elementwise op tensors need to be ranked.
    if (!biasTy.template isa<Torch::NoneType>() &&
        !biasTy.template isa<TensorType>())
      return rewriter.notifyMatchFailure(
          op, "Only tensor types supported in GEMM to TOSA for bias tensor");

    // RHS must have its last two dims transposed prior to matrix
    // multiplication.
    auto rhsTy = rhs.getType().cast<RankedTensorType>();
    auto rhsRank = rhsTy.getRank();
    auto rhsShape = makeShapeTorchCompatible(rhsTy.getShape());
    auto rhsElemTy = rhsTy.getElementType();

    // Create a non-const shape array to transpose dims.
    SmallVector<int64_t> transposedRhsShape;
    for (auto &shape : rhsShape)
      transposedRhsShape.push_back(shape);
    SmallVector<int32_t> transposedRhsDims;
    for (int32_t i = 0; i < rhsRank; i++)
      transposedRhsDims.push_back(i);

    // Swap the last two dims.
    std::swap(transposedRhsShape[rhsRank - 1], transposedRhsShape[rhsRank - 2]);
    std::swap(transposedRhsDims[rhsRank - 1], transposedRhsDims[rhsRank - 2]);

    std::optional<Value> transposedRhsShapeConst =
        tosa::getConstTensor<int32_t>(
            rewriter, op,
            /*vec=*/transposedRhsDims,
            /*shape=*/{static_cast<int32_t>(transposedRhsDims.size())});

    auto transposedRhsType = RankedTensorType::get(
        makeShapeLLVMCompatible(transposedRhsShape), rhsElemTy);
    rhs = rewriter.create<tosa::TransposeOp>(
        op->getLoc(),
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            transposedRhsType),
        rhs, transposedRhsShapeConst.value());

    Value matmulOutput;
    if (failed(
            this->performMatmul(op, adaptor, rewriter, lhs, rhs, matmulOutput)))
      return rewriter.notifyMatchFailure(op,
                                         "Failed to perform matmul operation");

    Value matmulPlusBias = matmulOutput;
    if (!biasTy.template isa<Torch::NoneType>()) {
      // Bias addition broadcasts to the matmul output shape.
      matmulPlusBias =
          rewriter
              .create<tosa::AddOp>(op->getLoc(), matmulOutput.getType(),
                                   matmulOutput, bias)
              .getResult();
    }

    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        op,
        OpConversionPattern<AtenOpT>::getTypeConverter()
            ->convertType(op.getType())
            .template cast<RankedTensorType>(),
        matmulPlusBias);

    return success();
  }
};

template <>
LogicalResult ConvertAtenOp<AtenRsubScalarOp>::matchAndRewrite(
    AtenRsubScalarOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto self = adaptor.getSelf();
  auto otherScalar = op.getOther();
  auto alphaScalar = op.getAlpha();

  auto selfTy = self.getType().cast<RankedTensorType>();
  if (!selfTy)
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types supported in TOSA Rsub");

  if (!selfTy.getElementType().isa<mlir::FloatType>())
    return rewriter.notifyMatchFailure(
        op, "Only floating-point datatype legalization supported");

  Value otherTensor, alphaTensor;

  if (failed(torchScalarToTosaTensor(rewriter, op, otherScalar, otherTensor,
                                     selfTy.getElementType(), {})))
    return rewriter.notifyMatchFailure(
        op, "Currently only scalar constants are supported for "
            "conversion in TOSA Rsub operation");

  if (failed(torchAlphaToTosaTensor(rewriter, op.getOperation(), alphaScalar,
                                    alphaTensor, selfTy.getElementType(),
                                    /*checkForUnity=*/true)))
    return failure();

  auto multTensor = rewriter.create<tosa::MulOp>(
      op->getLoc(), getTypeConverter()->convertType(op.getType()), self,
      alphaTensor, /*shift=*/0);

  rewriter.replaceOpWithNewOp<tosa::SubOp>(
      op, getTypeConverter()->convertType(op.getType()), otherTensor,
      multTensor);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenConvolutionOp>::matchAndRewrite(
    AtenConvolutionOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto input = adaptor.getInput();
  auto weight = adaptor.getWeight();

  auto inputTy = input.getType().cast<RankedTensorType>();
  auto weightTy = weight.getType().cast<RankedTensorType>();
  auto outputTy = getTypeConverter()
                      ->convertType(op.getType())
                      .template cast<RankedTensorType>();

  if (!inputTy || !weightTy || !outputTy)
    return rewriter.notifyMatchFailure(
        op, "Input, weight and output to Convolution must be ranked tensors");

  auto inputElemTy = inputTy.getElementType();
  auto weightElemTy = weightTy.getElementType();
  auto inputShape = makeShapeTorchCompatible(inputTy.getShape());
  auto weightShape = makeShapeTorchCompatible(weightTy.getShape());

  if (inputTy.getRank() != 4)
    return rewriter.notifyMatchFailure(
        op, "Unimplemented: only 2D convolutions supported");

  if (!weightTy.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "Unimplemented: TOSA only supports static weight");

  // Bias is optional. TOSA mandates a zero tensor here, so construct one if
  // required.
  auto bias = adaptor.getBias();
  if (adaptor.getBias().getType().template isa<Torch::NoneType>()) {
    // TBD: This is only valid for quantized 8-bit. For 16-bit, the bias (and
    // accumulator) are 48-bit and not 32-bit, and requires the use of APInt to
    // define a 48-bit int.
    if (inputElemTy.isa<quant::QuantizedType>()) {
      SmallVector<int32_t> zeroVec(weightShape[0], 0);
      bias = tosa::getConstTensor<int32_t>(
                 rewriter, op, zeroVec, {static_cast<int32_t>(weightShape[0])})
                 .value();
    } else {
      SmallVector<float> zeroVec(weightShape[0], 0);
      bias = tosa::getConstTensor<float>(rewriter, op, zeroVec,
                                         {static_cast<int32_t>(weightShape[0])})
                 .value();
    }
  } else {
    if (!bias.getType().cast<RankedTensorType>())
      return rewriter.notifyMatchFailure(
          op, "Bias provided but not a ranked tensor");
  }
  auto biasElemTy =
      inputElemTy.isa<mlir::FloatType>() ? inputElemTy : rewriter.getI32Type();

  SmallVector<int64_t, 2> stride;
  if (!matchPattern(adaptor.getStride(), m_TorchListOfConstantInts(stride)))
    return rewriter.notifyMatchFailure(op, "non-const stride list unsupported");

  SmallVector<int64_t, 2> padding_2d;
  if (!matchPattern(adaptor.getPadding(),
                    m_TorchListOfConstantInts(padding_2d)))
    return rewriter.notifyMatchFailure(op,
                                       "non-const padding list unsupported");
  // TOSA uses 4D padding {t, b, l, r} while Torch defines 2D padding {t, l}.
  // The Torch OFM computation uses 2*pad in each spatial direction, implying
  // the same t=b and l=r values for TOSA.
  SmallVector<int64_t> padding(
      {padding_2d[0], padding_2d[0], padding_2d[1], padding_2d[1]});

  SmallVector<int64_t, 2> dilation;
  if (!matchPattern(adaptor.getDilation(), m_TorchListOfConstantInts(dilation)))
    return rewriter.notifyMatchFailure(op,
                                       "non-const dilation list unsupported");

  // TOSA works in NHWC and takes OHWI weights. Perform the necessary transpose.
  std::optional<Value> nchwToNhwcTransposeConst =
      tosa::getConstTensor<int32_t>(rewriter, op,
                                    /*vec=*/{0, 2, 3, 1},
                                    /*shape=*/{static_cast<int32_t>(4)});
  SmallVector<int64_t> transposedInputShape(
      {inputShape[0], inputShape[2], inputShape[3], inputShape[1]});
  auto transposedInputType = RankedTensorType::get(
      makeShapeLLVMCompatible(transposedInputShape), inputElemTy);
  auto transposedInput =
      rewriter
          .create<tosa::TransposeOp>(
              op->getLoc(),
              getTypeConverter()->convertType(transposedInputType), input,
              nchwToNhwcTransposeConst.value())
          .getResult();

  SmallVector<int64_t> transposedWeightShape(
      {weightShape[0], weightShape[2], weightShape[3], weightShape[1]});
  auto transposedWeightType = RankedTensorType::get(
      makeShapeLLVMCompatible(transposedWeightShape), weightElemTy);
  auto transposedWeight =
      rewriter
          .create<tosa::TransposeOp>(
              op->getLoc(),
              getTypeConverter()->convertType(transposedWeightType), weight,
              nchwToNhwcTransposeConst.value())
          .getResult();

  int64_t outputHDim, outputWDim;
  if (inputTy.hasStaticShape()) {
    outputHDim = (transposedInputShape[1] + padding[0] + padding[1] -
                  dilation[0] * (transposedWeightShape[1] - 1) - 1) /
                     stride[0] +
                 1;
    outputWDim = (transposedInputShape[2] + padding[2] + padding[3] -
                  dilation[1] * (transposedWeightShape[2] - 1) - 1) /
                     stride[1] +
                 1;
  } else {
    outputHDim = kUnknownSize;
    outputWDim = kUnknownSize;
  }

  // Output shape is NHWC, to be transposed back to NCHW. Output elemTy for
  // quantized input is i32, which gets rescaled down to quantized output range.
  SmallVector<int64_t> outputShape = {transposedInputShape[0], outputHDim,
                                      outputWDim, transposedWeightShape[0]};
  auto convOpTy =
      RankedTensorType::get(makeShapeLLVMCompatible(outputShape), biasElemTy);

  Value convOpResult =
      rewriter
          .create<tosa::Conv2DOp>(op->getLoc(),
                                  getTypeConverter()->convertType(convOpTy),
                                  transposedInput, transposedWeight, bias,
                                  rewriter.getDenseI64ArrayAttr(padding),
                                  rewriter.getDenseI64ArrayAttr(stride),
                                  rewriter.getDenseI64ArrayAttr(dilation))
          .getResult();

  std::optional<Value> nhwcToNchwTransposeConst =
      tosa::getConstTensor<int32_t>(rewriter, op,
                                    /*vec=*/{0, 3, 1, 2},
                                    /*shape=*/{static_cast<int32_t>(4)});
  SmallVector<int64_t> transposedOutputShape(
      {outputShape[0], outputShape[3], outputShape[1], outputShape[2]});
  auto transposedOutputType = RankedTensorType::get(
      makeShapeLLVMCompatible(transposedOutputShape), biasElemTy);
  auto transposedOutput =
      rewriter
          .create<tosa::TransposeOp>(
              op->getLoc(),
              getTypeConverter()->convertType(transposedOutputType),
              convOpResult, nhwcToNchwTransposeConst.value())
          .getResult();

  Value rescaledResult = transposedOutput;
  if (inputElemTy.isa<quant::QuantizedType>()) {
    rescaledResult = tosa::buildRescaleOpConvOutput(
        rewriter, op, transposedOutput, inputTy, weightTy, outputTy);
  }

  rewriter.replaceOpWithNewOp<tensor::CastOp>(
      op, getTypeConverter()->convertType(op.getType()), rescaledResult);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenReshapeOp>::matchAndRewrite(
    AtenReshapeOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto self = adaptor.getSelf();

  auto selfTy = self.getType().cast<RankedTensorType>();
  if (!selfTy)
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types supported in TOSA Reshape");

  // Check that at most one dimension is -1
  SmallVector<int64_t> newShape;
  if (!matchPattern(op.getShape(), m_TorchListOfConstantInts(newShape)))
    return rewriter.notifyMatchFailure(
        op, "Only constant shape supported in TOSA Reshape");

  int auto_sz = 0;
  for (auto s : newShape)
    auto_sz += (s == -1 ? 1 : 0);
  if (auto_sz > 1)
    return rewriter.notifyMatchFailure(
        op, "At most one dimension may be specified as -1 to "
            "automatically calculate its size");

  auto newType = RankedTensorType::get(makeShapeLLVMCompatible(newShape),
                                       selfTy.getElementType());

  rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(
      op, getTypeConverter()->convertType(newType), self,
      rewriter.getDenseI64ArrayAttr(newShape));

  return success();
}

Value computeBatchNorm(Operation *op, ConversionPatternRewriter &rewriter,
                       Type outType, Value input, Value variance, Value eps,
                       Value mean, Value weight, Value bias) {
  // For PyTorch:
  //   scale  = gamma = weight
  //   offset = beta  = bias
  // Lowering:
  // fused batchnorm = (input-mean) * scale * rsqrt(var+epsilon)) + offset
  //
  // shape_0 = ones(input.rank)
  // shape_0[input.rank-1] = input.shape[input.rank-1]
  // shape_1 = ones(1)
  //
  // bmean  = reshape(mean, shape_0)
  // bscale = reshape(scale, shape_0)
  // boffset= reshape(offset, shape_0)
  // beps   = reshape(epsilon, shape_1)
  //
  // op1 = sub(input, bmean)
  // op2 = add(var, beps)
  // op3 = rsqrt(op2)
  // bvar = reshape(op3, shape_0)
  // op4 = mul(op1, bvar)
  // op5 = mul(op4, bscale)
  // op6 = add(op5, boffset)

  auto op1SubInputMean =
      rewriter.create<tosa::SubOp>(op->getLoc(), outType, input, mean);

  auto op2AddVarEpsilon = rewriter.create<tosa::AddOp>(
      op->getLoc(), variance.getType(), variance, eps);

  auto op3RsqrtOp2 = rewriter.create<tosa::RsqrtOp>(
      op->getLoc(), variance.getType(), op2AddVarEpsilon.getResult());

  auto op4MulOp1Op3 = rewriter.create<tosa::MulOp>(op->getLoc(), outType,
                                                   op1SubInputMean.getResult(),
                                                   op3RsqrtOp2.getResult(), 0);

  auto op5MulOp4Scale = rewriter.create<tosa::MulOp>(
      op->getLoc(), outType, op4MulOp1Op3.getResult(), weight, 0);

  return rewriter
      .create<tosa::AddOp>(op->getLoc(), outType, op5MulOp4Scale.getResult(),
                           bias)
      .getResult();
}

// This lowering is based on the TensorFlow to TOSA lowering.
template <>
LogicalResult ConvertAtenOp<AtenBatchNormOp>::matchAndRewrite(
    AtenBatchNormOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a ranked tensor output
  if (!adaptor.getInput().getType().dyn_cast<RankedTensorType>())
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types are supported");

  auto outType = getTypeConverter()->convertType(op.getType());

  // Note: cudnn_enabled is not handled.

  // FIXME: Handle training and momentum.
  if (op.getMomentum().getType().isa<Torch::NoneType>())
    return rewriter.notifyMatchFailure(op, "Unsupported None for momentum");

  auto meanType = adaptor.getRunningMean().getType().dyn_cast<TensorType>();
  auto varianceType = adaptor.getRunningVar().getType().dyn_cast<TensorType>();
  if (!varianceType || !meanType)
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types are supported");

  // Normalization ops perform elementwise ops of a single mean/stdev value
  // against the feature map and because input is NCHW, the rank-1 value must be
  // reshaped so it sits on the same dim as 'C'.
  auto reshapeToNormInputDim = [&](Operation *op,
                                   ConversionPatternRewriter &rewriter,
                                   TypeConverter *converter, Type outType,
                                   const Value toBcast, Value &result) {
    RankedTensorType toBcastType =
        toBcast.getType().dyn_cast<RankedTensorType>();
    if (toBcastType.getRank() > 1)
      return rewriter.notifyMatchFailure(op, "Rank cannot be more than 1");

    RankedTensorType outTensorType = outType.cast<RankedTensorType>();
    SmallVector<int64_t> newShape = {
        makeShapeTorchCompatible(toBcastType.getShape())[0]};
    for (auto i = 2; i < outTensorType.getRank(); ++i)
      newShape.push_back(1);
    auto newType = RankedTensorType::get(makeShapeLLVMCompatible(newShape),
                                         outTensorType.getElementType());

    result = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(), newType, toBcast,
        rewriter.getDenseI64ArrayAttr(newShape));

    return success();
  };

  Value meanVal, varianceVal, weightVal, biasVal;
  assert(meanType.getNumElements() != 0 && varianceType.getNumElements() != 0);
  if (failed(reshapeToNormInputDim(op.getOperation(), rewriter,
                                   getTypeConverter(), outType,
                                   adaptor.getRunningMean(), meanVal)))
    return rewriter.notifyMatchFailure(op, "Failed to reshape running mean");

  if (failed(reshapeToNormInputDim(op.getOperation(), rewriter,
                                   getTypeConverter(), outType,
                                   adaptor.getRunningVar(), varianceVal)))
    return rewriter.notifyMatchFailure(op,
                                       "Failed to reshape running variance");

  if (failed(reshapeToNormInputDim(op.getOperation(), rewriter,
                                   getTypeConverter(), outType,
                                   adaptor.getWeight(), weightVal)))
    return rewriter.notifyMatchFailure(op, "Failed to reshape weight");

  if (failed(reshapeToNormInputDim(op.getOperation(), rewriter,
                                   getTypeConverter(), outType,
                                   adaptor.getBias(), biasVal)))
    return rewriter.notifyMatchFailure(op, "Failed to reshape bias");

  double eps;
  if (!matchPattern(op.getEps(), m_TorchConstantFloat(&eps)))
    return rewriter.notifyMatchFailure(op, "eps must be a scalar constant");

  auto epsilonConst =
      mlir::tosa::getTosaConstTensorSingleF32(rewriter, op, eps);

  auto batchNorm =
      computeBatchNorm(op, rewriter, outType, adaptor.getInput(), varianceVal,
                       epsilonConst, meanVal, weightVal, biasVal);

  rewriter.replaceOp(op, {batchNorm});

  return success();
}

// This lowering is loosely based on Torch to LinAlg lowering.
template <>
LogicalResult ConvertAtenOp<AtenNativeLayerNormOp>::matchAndRewrite(
    AtenNativeLayerNormOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // The key difference from BatchNorm is that a specified set of dims
  // (normalized_shape) are chosen to compute the mean and variance from input.
  // Where as in BatchNorm the mean and variance are operands. tosa::ReduceSumOp
  // is used to sum up the these dims for mean and for variance. The results
  // eventually being reshaped for broadcasting.

  // Not a ranked tensor output
  if (!adaptor.getInput().getType().dyn_cast<RankedTensorType>())
    return rewriter.notifyMatchFailure(
        op, "Only ranked tensor types are supported");

  auto inputType = adaptor.getInput().getType().cast<RankedTensorType>();
  if (inputType.getRank() > 4)
    return rewriter.notifyMatchFailure(op,
                                       "Only up to 4D tensors are supported");

  auto outType = getTypeConverter()->convertType(op.getType(0));

  // Note: cudnn_enabled is not handled.

  // FIXME: Handle the None cases for the optional parameters.
  if (adaptor.getWeight().getType().isa<Torch::NoneType>())
    return rewriter.notifyMatchFailure(op, "Unsupported None for weight");
  if (adaptor.getBias().getType().isa<Torch::NoneType>())
    return rewriter.notifyMatchFailure(op, "Unsupported None for bias");

  auto weightType = adaptor.getWeight().getType().cast<RankedTensorType>();
  auto biasType = adaptor.getBias().getType().cast<RankedTensorType>();
  int64_t inputRank = inputType.getRank();
  Type elemTy = inputType.getElementType();
  SmallVector<int64_t> inputTypeShape(
      makeShapeTorchCompatible(inputType.getShape()));

  // Check if all the arguments meet the requirements.
  SmallVector<int64_t> normalizedShapeSizesInt;
  if (!matchPattern(op.getNormalizedShape(),
                    m_TorchListOfConstantInts(normalizedShapeSizesInt))) {
    return rewriter.notifyMatchFailure(op, "Unimplemented normalized_shape not"
                                           "constructed from ListConstruct");
  }
  int64_t normalizedShapeRank = normalizedShapeSizesInt.size();
  if (weightType.getRank() != normalizedShapeRank ||
      biasType.getRank() != normalizedShapeRank ||
      inputRank < normalizedShapeRank || normalizedShapeRank < 1)
    return rewriter.notifyMatchFailure(op, "Input or weight or bias shape or"
                                           "normalized shape not compatible");

  // Check all the dimensions match the normalized_shape, only static shapes as
  // of now
  int64_t meanAndVarShapeRank = inputRank - normalizedShapeSizesInt.size();
  for (auto en : llvm::enumerate((normalizedShapeSizesInt))) {
    int64_t index = en.index();
    int64_t value = en.value();
    if (inputTypeShape[index + meanAndVarShapeRank] != value ||
        makeShapeTorchCompatible(weightType.getShape())[index] != value ||
        makeShapeTorchCompatible(biasType.getShape())[index] != value)
      return rewriter.notifyMatchFailure(op,
                                         "mismatching contracting dimension");
  }

  // Helper for computing mean and variance.
  auto computeSumAndReshape = [&](Value toReduce, RankedTensorType toReduceType,
                                  Type outType, SmallVector<int64_t> outShape) {
    Value sumDiv = toReduce;
    SmallVector<int64_t> toReduceShape(
        makeShapeTorchCompatible(toReduceType.getShape()));
    for (int64_t i = toReduceShape.size() - 1; i >= meanAndVarShapeRank; i--) {
      toReduceShape[i] = 1;
      sumDiv = rewriter.create<tosa::ReduceSumOp>(
          op.getLoc(),
          RankedTensorType::get(makeShapeLLVMCompatible(toReduceShape),
                                inputType.getElementType()),
          sumDiv, rewriter.getI64IntegerAttr(i));
    }

    return rewriter.create<tosa::ReshapeOp>(
        op.getLoc(), outType, sumDiv, rewriter.getDenseI64ArrayAttr(outShape));
  };

  // TOSA has integer Div so, compute reciprocal of element count to be used in
  // mul.
  int64_t elemCnt = 1;
  for (auto i : normalizedShapeSizesInt)
    elemCnt *= i;

  auto elemCntConst =
      tosa::getConstTensor<float>(rewriter, op.getOperation(),
                                  {static_cast<float>(elemCnt)}, {1})
          .value();
  Value elemCntRcp = rewriter.create<tosa::ReciprocalOp>(
      op.getLoc(), elemCntConst.getType(), elemCntConst);

  // Broadcast type and shape for various intermediate values.
  SmallVector<int64_t> bcastOutShape;
  for (auto en : llvm::enumerate(inputTypeShape)) {
    bcastOutShape.push_back(
        static_cast<int64_t>(en.index()) >= meanAndVarShapeRank ? 1
                                                                : en.value());
  }
  auto bcastOutType =
      RankedTensorType::get(makeShapeLLVMCompatible(bcastOutShape), elemTy);

  // Compute mean.
  Value sum = computeSumAndReshape(adaptor.getInput(), inputType, bcastOutType,
                                   bcastOutShape);
  Value meanVal = rewriter.create<tosa::MulOp>(op.getLoc(), bcastOutType, sum,
                                               elemCntRcp, /*shift=*/0);

  // Compute variance.
  Value squareSumSub = rewriter.create<tosa::SubOp>(
      op.getLoc(), inputType, adaptor.getInput(), meanVal);
  Value squareSum = rewriter.create<tosa::MulOp>(op.getLoc(), inputType,
                                                 squareSumSub, squareSumSub, 0);

  Value squareSumReduced =
      computeSumAndReshape(squareSum, inputType, bcastOutType, bcastOutShape);
  Value varianceVal = rewriter.create<tosa::MulOp>(
      op.getLoc(), bcastOutType, squareSumReduced, elemCntRcp, /*shift=*/0);

  // Reshape weight and bias.
  SmallVector<int64_t> weightAndBiasBcastShape;
  for (auto en :
       llvm::enumerate(makeShapeTorchCompatible(inputType.getShape()))) {
    weightAndBiasBcastShape.push_back(
        static_cast<int64_t>(en.index()) < meanAndVarShapeRank ? 1
                                                               : en.value());
  }
  auto weightAndMeanBcastType = RankedTensorType::get(
      makeShapeLLVMCompatible(weightAndBiasBcastShape), elemTy);

  Value weightVal = rewriter.create<tosa::ReshapeOp>(
      op.getLoc(), weightAndMeanBcastType, adaptor.getWeight(),
      rewriter.getDenseI64ArrayAttr(weightAndBiasBcastShape));

  Value biasVal = rewriter.create<tosa::ReshapeOp>(
      op.getLoc(), weightAndMeanBcastType, adaptor.getBias(),
      rewriter.getDenseI64ArrayAttr(weightAndBiasBcastShape));

  double eps;
  if (!matchPattern(op.getEps(), m_TorchConstantFloat(&eps)))
    return rewriter.notifyMatchFailure(op, "eps must be a scalar constant");
  auto epsilonConst =
      mlir::tosa::getTosaConstTensorSingleF32(rewriter, op, eps);

  // Compute layer norm.
  auto layerNorm =
      computeBatchNorm(op, rewriter, outType, adaptor.getInput(), varianceVal,
                       epsilonConst, meanVal, weightVal, biasVal);

  rewriter.replaceOp(op, {layerNorm, meanVal, varianceVal});

  return success();
}

// Torch constants are converted to tosa.const .
template <>
LogicalResult ConvertAtenOp<ValueTensorLiteralOp>::matchAndRewrite(
    ValueTensorLiteralOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto outputTy = getTypeConverter()
                      ->convertType(op.getType())
                      .template cast<RankedTensorType>();

  // Tensors with integer types need to be converted to signless integer
  // element type. All tensors with element types other than integer can reuse
  // existing elements attribute.
  // TODO: what about unsigned integer?
  if (auto elements = op.getValueAttr().dyn_cast<DenseIntElementsAttr>()) {
    if (elements.getElementType().isSignedInteger()) {
      Type builtinTensorElemTy = outputTy.getElementType();
      unsigned bitWidth = builtinTensorElemTy.getIntOrFloatBitWidth();
      DenseElementsAttr valueAttr =
          elements.mapValues(builtinTensorElemTy, [&](const APInt &v) {
            return APInt(bitWidth, v.getSExtValue());
          });
      rewriter.replaceOpWithNewOp<tosa::ConstOp>(op, outputTy, valueAttr);
      return success();
    }
  }
  rewriter.replaceOpWithNewOp<tosa::ConstOp>(op, outputTy, adaptor.getValue());
  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenFlattenUsingIntsOp>::matchAndRewrite(
    AtenFlattenUsingIntsOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a ranked tensor type
  auto selfType = adaptor.getSelf().getType().dyn_cast<RankedTensorType>();
  if (!selfType || !selfType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op,
        "Only ranked tensor types with static shapes are currently supported");

  int64_t selfRank = selfType.getRank();

  int64_t start_dim, end_dim;

  if (!matchPattern(op.getStartDim(), m_TorchConstantInt(&start_dim)))
    return rewriter.notifyMatchFailure(op,
                                       "start_dim must be a Scalar constant");
  start_dim = toPositiveDim(start_dim, selfRank);

  if (!matchPattern(op.getEndDim(), m_TorchConstantInt(&end_dim)))
    return rewriter.notifyMatchFailure(op, "end_dim must be a Scalar constant");
  end_dim = toPositiveDim(end_dim, selfRank);

  if (selfRank > 0 && !isValidDim(start_dim, selfRank))
    return rewriter.notifyMatchFailure(op, "start_dim is statically invalid");
  if (selfRank > 0 && !isValidDim(end_dim, selfRank))
    return rewriter.notifyMatchFailure(op, "end_dim is statically invalid");
  if (end_dim < start_dim)
    return rewriter.notifyMatchFailure(op,
                                       "end_dim must be larger than start_dim");

  SmallVector<int64_t> newShape;
  for (auto s :
       llvm::enumerate(makeShapeTorchCompatible(selfType.getShape()))) {
    int64_t idx = s.index();
    if (idx < start_dim || idx > end_dim) {
      newShape.push_back(s.value());
    } else {
      if (idx == start_dim)
        newShape.push_back(s.value());
      else
        newShape.back() *= s.value();
    }
  }

  // Handle the Scalar case
  if (newShape.size() == 0)
    newShape.push_back(1);

  auto newType = RankedTensorType::get(makeShapeLLVMCompatible(newShape),
                                       selfType.getElementType());
  auto reshapeOp =
      rewriter.create<tosa::ReshapeOp>(op.getLoc(), newType, adaptor.getSelf(),
                                       rewriter.getDenseI64ArrayAttr(newShape));

  rewriter.replaceOpWithNewOp<tensor::CastOp>(
      op, getTypeConverter()->convertType(op.getType()), reshapeOp);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenPermuteOp>::matchAndRewrite(
    AtenPermuteOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a ranked tensor type
  auto selfType = adaptor.getSelf().getType().dyn_cast<RankedTensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op,
        "Only ranked tensor types with static shapes are currently supported");

  SmallVector<int64_t> dimListInt;
  if (!matchPattern(adaptor.getDims(), m_TorchListOfConstantInts(dimListInt)))
    return rewriter.notifyMatchFailure(
        op, "Only constant dimensions are currently supported");

  int64_t selfRank = selfType.getRank();
  // TODO: If this is already verified on the op then we can drop checking here.
  for (auto &d : dimListInt) {
    d = toPositiveDim(d, selfRank);
    if (!isValidDim(d, selfRank))
      return rewriter.notifyMatchFailure(op, "Not all dims are valid");
  }

  auto transposeDimsConst = mlir::tosa::getConstTensor<int64_t>(
      rewriter, op.getOperation(), dimListInt, {selfRank});

  rewriter.replaceOpWithNewOp<tosa::TransposeOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getSelf(),
      transposeDimsConst.value());

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenLog2Op>::matchAndRewrite(
    AtenLog2Op op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  // Constant value of ln2.
  SmallVector<int64_t> ln2Shape(selfType.getRank(), 1);
  auto ln2Op =
      tosa::getConstTensor<float>(rewriter, op, {0.69314718056}, ln2Shape)
          .value();
  auto rcpOp =
      rewriter.create<tosa::ReciprocalOp>(op.getLoc(), ln2Op.getType(), ln2Op);

  auto outType = getTypeConverter()->convertType(op.getType());
  auto logOp =
      rewriter.create<tosa::LogOp>(op.getLoc(), outType, adaptor.getSelf());
  rewriter.replaceOpWithNewOp<tosa::MulOp>(op, outType, logOp, rcpOp,
                                           /*shift=*/0);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenThresholdOp>::matchAndRewrite(
    AtenThresholdOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  auto selfElemTy = selfType.getElementType();
  if (!selfElemTy.isIntOrFloat())
    return rewriter.notifyMatchFailure(
        op, "Only floating-point or integer datatype legalization supported");

  // Integer types with width > 32 are not supported
  auto selfIntType = selfElemTy.dyn_cast<IntegerType>();
  if (selfIntType && selfIntType.getWidth() > 32) {
    return rewriter.notifyMatchFailure(
        op, "Integer types with width greater than 32 are not supported");
  }

  SmallVector<int64_t> constTypeShape(selfType.getRank(), 1);
  Value threshold, value;
  if (failed(torchScalarToTosaTensor(rewriter, op, op.getThreshold(), threshold,
                                     selfElemTy, constTypeShape)))
    return rewriter.notifyMatchFailure(
        op, "Only scalar constant is supported for threshold");

  if (failed(torchScalarToTosaTensor(rewriter, op, op.getValue(), value,
                                     selfElemTy, constTypeShape)))
    return rewriter.notifyMatchFailure(
        op, "Only scalar constant is supported for value");

  // Threshold only clamps the upper values. tosa::ClampOp has the same
  // value for both threshold and clamped value so cannot be used.
  auto outType = getTypeConverter()->convertType(op.getType());

  auto cmpOp = rewriter.create<tosa::GreaterOp>(
      op.getLoc(),
      RankedTensorType::get(selfType.getShape(), rewriter.getIntegerType(1)),
      adaptor.getSelf(), threshold);

  rewriter.replaceOpWithNewOp<tosa::SelectOp>(op, outType, cmpOp,
                                              adaptor.getSelf(), value);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenUnsqueezeOp>::matchAndRewrite(
    AtenUnsqueezeOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType) {
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");
  }

  auto selfRank = selfType.getRank();
  auto selfElemTy = selfType.getElementType();
  if (!selfElemTy.isIntOrFloat()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point or integer datatype legalization supported");
  }

  int64_t dim;
  if (!matchPattern(op.getDim(), m_TorchConstantInt(&dim)))
    return rewriter.notifyMatchFailure(op, "dim must be a Scalar constant");

  // toPositiveDim converts negative dims to the range [0, inputRank). So, -1
  // will be converted to inputRank-1. For `torch.unsqueeze` op, -1 has to be
  // converted to inputRank, and the valid dim range is [0, inputRank + 1).
  dim = toPositiveDim(dim, selfRank + 1);
  if (!isValidDim(dim, selfRank + 1))
    return rewriter.notifyMatchFailure(op, "dim is statically invalid");

  SmallVector<int64_t> outShape;
  for (auto en :
       llvm::enumerate(makeShapeTorchCompatible(selfType.getShape()))) {
    if (static_cast<int64_t>(en.index()) == dim)
      outShape.push_back(1);

    outShape.push_back(en.value());
  }
  if (dim == selfRank)
    outShape.push_back(1);

  rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getSelf(),
      rewriter.getDenseI64ArrayAttr(outShape));

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenContiguousOp>::matchAndRewrite(
    AtenContiguousOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  // FIXME: memory_format is not handled.

  rewriter.replaceOp(op, adaptor.getSelf());

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenDropoutOp>::matchAndRewrite(
    AtenDropoutOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getInput().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  // FIXME: train and p are not handled.

  bool train;
  if (!matchPattern(op.getTrain(), m_TorchConstantBool(&train)))
    return rewriter.notifyMatchFailure(op, "train must be a Scalar constant");

  if (train)
    return rewriter.notifyMatchFailure(op, "train must be false");

  rewriter.replaceOpWithNewOp<tosa::CastOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getInput());

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenViewOp>::matchAndRewrite(
    AtenViewOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  auto selfElemTy = selfType.getElementType();
  if (!selfElemTy.isIntOrFloat()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point or integer datatype legalization supported");
  }

  SmallVector<int64_t> outShape;
  if (!matchPattern(op.getSize(), m_TorchListOfConstantInts(outShape)))
    return rewriter.notifyMatchFailure(op,
                                       "size must consist of Scalar constants");

  rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getSelf(),
      rewriter.getDenseI64ArrayAttr(outShape));

  return success();
}

static Value approximateErfOp(ConversionPatternRewriter &rewriter,
                              Operation *op, Value x) {
  // Using:
  // https://en.wikipedia.org/wiki/Error_function#Numerical_approximations with
  // maximum error as 5 x 10^-4 where a1 = 0.278393, a2 = 0.230389, a3 =
  // 0.000972, a4 = 0.078108.
  //
  // Erf = 1 - 1 / (1 + a1X + a2X + a3X + a4X)^4

  auto outType = x.getType().cast<TensorType>();
  auto loc = op->getLoc();
  auto absX = rewriter.create<tosa::AbsOp>(loc, outType, x);
  auto zero = tosa::getConstTensor<float>(rewriter, op, 0, {}).value();
  auto one = tosa::getConstTensor<float>(rewriter, op, 1, {}).value();

  auto a1 = tosa::getConstTensor<float>(rewriter, op, 0.278393, {}).value();
  auto a1X = rewriter.create<tosa::MulOp>(loc, outType, a1, absX, /*shift=*/0);
  auto sum = rewriter.create<tosa::AddOp>(loc, outType, a1X, one);

  auto a2 = tosa::getConstTensor<float>(rewriter, op, 0.230389, {}).value();
  auto x2 = rewriter.create<tosa::MulOp>(loc, outType, absX, absX, /*shift=*/0);
  auto a2X = rewriter.create<tosa::MulOp>(loc, outType, a2, x2, /*shift=*/0);
  sum = rewriter.create<tosa::AddOp>(loc, outType, sum, a2X);

  auto a3 = tosa::getConstTensor<float>(rewriter, op, 0.000972, {}).value();
  auto x3 = rewriter.create<tosa::MulOp>(loc, outType, x2, absX, /*shift=*/0);
  auto a3X = rewriter.create<tosa::MulOp>(loc, outType, a3, x3, /*shift=*/0);
  sum = rewriter.create<tosa::AddOp>(loc, outType, sum, a3X);

  auto a4 = tosa::getConstTensor<float>(rewriter, op, 0.078108, {}).value();
  auto x4 = rewriter.create<tosa::MulOp>(loc, outType, x3, absX, /*shift=*/0);
  auto a4X = rewriter.create<tosa::MulOp>(loc, outType, a4, x4, /*shift=*/0);
  sum = rewriter.create<tosa::AddOp>(loc, outType, sum, a4X);

  auto rcprl = rewriter.create<tosa::ReciprocalOp>(loc, outType, sum);
  auto rcprl2 =
      rewriter.create<tosa::MulOp>(loc, outType, rcprl, rcprl, /*shift=*/0);
  auto rcprl4 =
      rewriter.create<tosa::MulOp>(loc, outType, rcprl2, rcprl2, /*shift=*/0);
  auto erf = rewriter.create<tosa::SubOp>(loc, outType, one, rcprl4);

  // Deal with negative x.
  auto cond = rewriter.create<tosa::GreaterEqualOp>(
      loc,
      RankedTensorType::get(outType.getShape(), rewriter.getIntegerType(1)), x,
      zero);
  auto negateErf = rewriter.create<tosa::NegateOp>(loc, outType, erf);

  return rewriter.create<tosa::SelectOp>(loc, outType, cond, erf, negateErf);
}

static Value buildUnitNormalCdf(ConversionPatternRewriter &rewriter,
                                Operation *op, Value x) {
  auto zero = tosa::getConstTensor<float>(rewriter, op, 0, {}).value();
  auto one = tosa::getConstTensor<float>(rewriter, op, 1, {}).value();
  auto loc = op->getLoc();

  // buildNormalCdf, mean = zero, sigma = one
  auto outType = x.getType();
  auto mean = zero;
  Value xMinusMean = rewriter.create<tosa::SubOp>(loc, outType, x, mean);
  // rsqrt of 2
  Value rsqrt2 =
      tosa::getConstTensor<float>(rewriter, op, 0.70710678, {}).value();
  Value erfArg = rewriter.create<tosa::MulOp>(loc, outType, xMinusMean, rsqrt2,
                                              /*shift=*/0);
  Value erf = approximateErfOp(rewriter, op, erfArg);
  Value erfPlus1 = rewriter.create<tosa::AddOp>(loc, outType, one, erf);
  Value oneHalf = tosa::getConstTensor<float>(rewriter, op, 0.5, {}).value();
  Value normalCdf = rewriter.create<tosa::MulOp>(loc, outType, oneHalf,
                                                 erfPlus1, /*shift=*/0);
  return normalCdf;
}

// This lowering is based on Torch to LinAlg lowering.
template <>
LogicalResult ConvertAtenOp<AtenGeluOp>::matchAndRewrite(
    AtenGeluOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  auto selfElemTy = selfType.getElementType();
  if (!selfElemTy.isa<mlir::FloatType>()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point datatype legalization supported");
  }

  // TODO: Handle approximate.
  std::string approximate;
  if (!matchPattern(op.getApproximate(), m_TorchConstantStr(approximate)) ||
      approximate != "none") {
    return rewriter.notifyMatchFailure(op, "Unsupported value of approximate");
  }

  Value cdf = buildUnitNormalCdf(rewriter, op, adaptor.getSelf());
  rewriter.replaceOpWithNewOp<tosa::MulOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getSelf(), cdf,
      /*shift=*/0);

  return success();
}

// This lowering is based on Torch to LinAlg lowering.
template <>
LogicalResult ConvertAtenOp<AtenGeluBackwardOp>::matchAndRewrite(
    AtenGeluBackwardOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types are currently supported");

  auto selfElemTy = selfType.getElementType();
  if (!selfElemTy.isa<mlir::FloatType>()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point datatype legalization supported");
  }

  // TODO: Handle approximate.
  std::string approximate;
  if (!matchPattern(op.getApproximate(), m_TorchConstantStr(approximate)) ||
      approximate != "none") {
    return rewriter.notifyMatchFailure(op, "Unsupported value of approximate");
  }

  auto loc = op->getLoc();

  const double cstAlpha0 = 1.12837916709551257390;
  const double cstAlpha1 = 0.70710678118654752440;
  const double oneHalf = 0.5;
  const double kAlpha = cstAlpha0 * cstAlpha1;

  Value kAlphaHalf =
      tosa::getConstTensor<float>(rewriter, op, kAlpha * oneHalf, {}).value();
  Value negOneHalf =
      tosa::getConstTensor<float>(rewriter, op, -0.5, {}).value();
  Value inputSquared = rewriter.create<tosa::MulOp>(
      loc, selfType, adaptor.getSelf(), adaptor.getSelf(), /*shift=*/0);
  Value negHalfInputSquared = rewriter.create<tosa::MulOp>(
      loc, selfType, inputSquared, negOneHalf, /*shift=*/0);
  Value dinput =
      rewriter.create<tosa::ExpOp>(loc, selfType, negHalfInputSquared);
  Value cdf = buildUnitNormalCdf(rewriter, op, adaptor.getSelf());
  Value dinputInput = rewriter.create<tosa::MulOp>(
      loc, selfType, dinput, adaptor.getSelf(), /*shift=*/0);
  Value dinputInputAlpha = rewriter.create<tosa::MulOp>(
      loc, selfType, dinputInput, kAlphaHalf, /*shift=*/0);
  Value cdfExt =
      rewriter.create<tosa::AddOp>(loc, selfType, dinputInputAlpha, cdf);
  rewriter.replaceOpWithNewOp<tosa::MulOp>(
      op, getTypeConverter()->convertType(op.getType()),
      adaptor.getGradOutput(), cdfExt,
      /*shift=*/0);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenEmbeddingOp>::matchAndRewrite(
    AtenEmbeddingOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  Value weight = adaptor.getWeight();
  Value indices = adaptor.getIndices();
  RankedTensorType outType =
      typeConverter->convertType(op.getType()).cast<RankedTensorType>();

  auto indicesType = indices.getType().dyn_cast<RankedTensorType>();
  if (!indicesType || !indicesType.getElementType().isa<IntegerType>())
    return rewriter.notifyMatchFailure(
        op, "Indices must be of integer tensor type");

  if (indicesType.getRank() != 2)
    return rewriter.notifyMatchFailure(op, "indices must be of rank 2");

  auto weightType = weight.getType().cast<RankedTensorType>();
  if (weightType.getRank() != 2)
    return op.emitError("weight must be of rank 2");

  // FIXME: padding_idx, scale_grad_by_freq and sparse are not handled yet.
  int64_t paddingIdx;
  if (!matchPattern(op.getPaddingIdx(), m_TorchConstantInt(&paddingIdx)))
    return rewriter.notifyMatchFailure(
        op, "only supports constant int padding_idx for embedding op");

  bool scaleGradByFreq;
  if (!matchPattern(op.getScaleGradByFreq(),
                    m_TorchConstantBool(&scaleGradByFreq)))
    return rewriter.notifyMatchFailure(
        op, "only supports constant bool scale_grad_by_freq for embedding op");
  if (scaleGradByFreq)
    return rewriter.notifyMatchFailure(
        op,
        "only supports scale_grad_by_freq equals to False for embedding op");

  bool isSparse;
  if (!matchPattern(op.getSparse(), m_TorchConstantBool(&isSparse)))
    return rewriter.notifyMatchFailure(
        op, "only supports constant bool sparse for embedding op");
  if (isSparse)
    return rewriter.notifyMatchFailure(
        op, "only support sparse equals to False for embedding op");

  // For inference:
  //    Weights [num_embeddings, embedding_dim], Indices [X, Y]
  //    Output [X, Y, embedding_dim] = Weights[Indices[x, y]] forall x in X, y
  //    in Y
  //
  //    Condition: num_embeddings > Indices [x, y] forall x in X, y in Y

  // Reshape the weight, since tosa.gather expects a 3D tensor
  auto indicesShape = makeShapeTorchCompatible(indicesType.getShape());
  auto weightShape = makeShapeTorchCompatible(weightType.getShape());

  SmallVector<int64_t> newWeightShape = {1};
  for (auto s : weightShape)
    newWeightShape.push_back(s);

  auto reshapedWeight = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(makeShapeLLVMCompatible(newWeightShape),
                            weightType.getElementType()),
      weight, rewriter.getDenseI64ArrayAttr(newWeightShape));

  int64_t numIndices = 1;
  if (indicesType.hasStaticShape()) {
    for (auto s : indicesShape)
      numIndices *= s;
  } else {
    numIndices = kUnknownSize;
  }

  SmallVector<int64_t> newIndicesShape = {1, numIndices};
  auto reshapedIndices = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(makeShapeLLVMCompatible(newIndicesShape),
                            indicesType.getElementType()),
      indices, rewriter.getDenseI64ArrayAttr(newIndicesShape));

  auto castIndices = rewriter.create<tosa::CastOp>(
      op->getLoc(),
      RankedTensorType::get(makeShapeLLVMCompatible(newIndicesShape),
                            rewriter.getIntegerType(32)),
      reshapedIndices);

  SmallVector<int64_t> intermediateOutShape = {1, numIndices, weightShape[1]};
  auto gatherOp = rewriter.create<tosa::GatherOp>(
      op->getLoc(),
      RankedTensorType::get(makeShapeLLVMCompatible(intermediateOutShape),
                            weightType.getElementType()),
      reshapedWeight, castIndices);

  rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(
      op, outType, gatherOp,
      rewriter.getDenseI64ArrayAttr(
          makeShapeTorchCompatible(outType.getShape())));

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenTransposeIntOp>::matchAndRewrite(
    AtenTransposeIntOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(op, "Only tensor types are supported");

  // Only statically resolvable values are currently supported
  int64_t dim0, dim1;
  if (!matchPattern(op.getDim0(), m_TorchConstantInt(&dim0)))
    return rewriter.notifyMatchFailure(op, "dim0 must be a Scalar constant");

  if (!matchPattern(op.getDim1(), m_TorchConstantInt(&dim1)))
    return rewriter.notifyMatchFailure(op, "dim1 must be a Scalar constant");

  dim0 = toPositiveDim(dim0, selfType.getRank());
  dim1 = toPositiveDim(dim1, selfType.getRank());

  auto selfRank = selfType.getRank();
  if (!isValidDim(dim0, selfRank) || !isValidDim(dim1, selfRank))
    return rewriter.notifyMatchFailure(
        op, "dim0 and dim1 must be less than tensor rank");

  SmallVector<int32_t> transposeDims;
  for (auto i = 0; i < selfType.getRank(); ++i)
    transposeDims.push_back(i);

  transposeDims[dim0] = dim1;
  transposeDims[dim1] = dim0;

  auto transposeDimsConst = mlir::tosa::getConstTensor<int32_t>(
      rewriter, op.getOperation(), transposeDims, {selfType.getRank()});

  rewriter.replaceOpWithNewOp<tosa::TransposeOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getSelf(),
      transposeDimsConst.value());

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenMaxDimOp>::matchAndRewrite(
    AtenMaxDimOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(op, "Only tensor types are supported");

  auto indicesType =
      getTypeConverter()->convertType(op.getType(1)).dyn_cast<TensorType>();
  if (!indicesType)
    return rewriter.notifyMatchFailure(op, "Only tensor types are supported");

  auto selfElemType = selfType.getElementType();
  auto indicesElemType = indicesType.getElementType();

  // Only statically deducible values are currently supported
  int64_t dim;
  if (!matchPattern(op.getDim(), m_TorchConstantInt(&dim)))
    return rewriter.notifyMatchFailure(op, "dim must be a Scalar constant");

  dim = toPositiveDim(dim, selfType.getRank());

  if (!isValidDim(dim, selfType.getRank()))
    return rewriter.notifyMatchFailure(op, "dim must be less than tensor rank");

  bool keepDim;
  if (!matchPattern(op.getKeepdim(), m_TorchConstantBool(&keepDim)))
    return rewriter.notifyMatchFailure(op, "keepdim must be a Scalar constant");

  SmallVector<int64_t> reducedShape, prunedShape;
  for (auto en :
       llvm::enumerate(makeShapeTorchCompatible(selfType.getShape()))) {
    if (static_cast<int64_t>(en.index()) == dim) {
      reducedShape.push_back(1);
      continue;
    }
    reducedShape.push_back(en.value());
    prunedShape.push_back(en.value());
  }

  auto dimAttr = rewriter.getIntegerAttr(rewriter.getI64Type(), dim);
  auto prunedShapeAttr = rewriter.getDenseI64ArrayAttr(prunedShape);

  Value reduceMax = rewriter.create<tosa::ReduceMaxOp>(
      op->getLoc(),
      RankedTensorType::get(makeShapeLLVMCompatible(reducedShape),
                            selfElemType),
      adaptor.getSelf(), dimAttr);

  Value argMax = rewriter.create<tosa::ArgMaxOp>(
      op->getLoc(),
      RankedTensorType::get(makeShapeLLVMCompatible(prunedShape),
                            indicesElemType),
      adaptor.getSelf(), dimAttr);

  if (argMax.getType() != indicesType) {
    argMax = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(), indicesType, argMax,
        rewriter.getDenseI64ArrayAttr(reducedShape));
  }

  if (!keepDim) {
    reduceMax = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(),
        RankedTensorType::get(makeShapeLLVMCompatible(prunedShape),
                              selfElemType),
        reduceMax, prunedShapeAttr);
  }

  rewriter.replaceOp(op, {reduceMax, argMax});

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenSliceTensorOp>::matchAndRewrite(
    AtenSliceTensorOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType || !selfType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "Only tensor types with static shape are supported");

  // Only statically deducible values are currently supported
  int64_t dim;
  if (!matchPattern(op.getDim(), m_TorchConstantInt(&dim)))
    return rewriter.notifyMatchFailure(op, "dim must be a Scalar constant");

  dim = toPositiveDim(dim, selfType.getRank());

  if (!isValidDim(dim, selfType.getRank()))
    return rewriter.notifyMatchFailure(op, "dim must less than tensor rank");

  int64_t start;
  if (!matchPattern(op.getStart(), m_TorchConstantInt(&start)))
    return rewriter.notifyMatchFailure(op, "start must be a Scalar constant");

  if (start < 0)
    return rewriter.notifyMatchFailure(op, "Currently unsupported: start < 0");

  int64_t end;
  if (!matchPattern(op.getEnd(), m_TorchConstantInt(&end)))
    return rewriter.notifyMatchFailure(op, "end must be a Scalar constant");

  // FIXME: add support for start/end < 0 and end < start
  if (end < start)
    return rewriter.notifyMatchFailure(op,
                                       "Currently unsupported: end < start");

  int64_t step;
  if (!matchPattern(op.getStep(), m_TorchConstantInt(&step)))
    return rewriter.notifyMatchFailure(op, "step must be a Scalar constant");

  if (step != 1)
    return rewriter.notifyMatchFailure(
        op, "step value other than 1 is currently unsupported");

  SmallVector<int64_t> startSlice(selfType.getRank(), 0);
  SmallVector<int64_t> sizeSlice =
      llvm::to_vector(makeShapeTorchCompatible(selfType.getShape()));

  startSlice[dim] = start;
  sizeSlice[dim] = end - start;

  rewriter.replaceOpWithNewOp<tosa::SliceOp>(
      op, getTypeConverter()->convertType(op.getType()), adaptor.getSelf(),
      rewriter.getDenseI64ArrayAttr(startSlice),
      rewriter.getDenseI64ArrayAttr(sizeSlice));

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenBroadcastToOp>::matchAndRewrite(
    AtenBroadcastToOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType || !selfType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "Only tensor types with static shape are supported");

  auto selfElemTy = selfType.getElementType();
  if (!selfElemTy.isIntOrFloat()) {
    return rewriter.notifyMatchFailure(
        op, "Only floating-point or integer datatype legalization supported");
  }

  SmallVector<int64_t> outShape;
  if (!matchPattern(op.getSize(), m_TorchListOfConstantInts(outShape)))
    return rewriter.notifyMatchFailure(op,
                                       "size must consist of Scalar constants");

  SmallVector<int64_t> inputShape(
      makeShapeTorchCompatible(selfType.getShape()));
  if (inputShape.size() == outShape.size() || inputShape.size() == 0) {
    // Check for identity case i.e, for ex: [a, b, c] -> [a, b, c]. If this is
    // true then we can replace the op result with the input operand
    // irrespective of the users of the op result.
    if (!llvm::equal(inputShape, outShape)) {
      for (auto user : op->getResult(0).getUsers()) {
        // This case is only supported if the result of the `broadcast_to` op is
        // not used by an op which is a view like.
        if (isViewLikeOp(user)) {
          return rewriter.notifyMatchFailure(
              op, "unimplemented: broadcast not supported for this case");
        }
      }
    }
    // If we reach here, then it means the given case is handled by implicit
    // broadcasting done by tosa.
    op.replaceAllUsesWith(op.getSelf());
    rewriter.eraseOp(op);
    return success();
  }
  return rewriter.notifyMatchFailure(
      op,
      "unimplemented: broadcasts other than same rank or zero ranked tensor.");
}

template <>
LogicalResult ConvertAtenOp<AtenGatherOp>::matchAndRewrite(
    AtenGatherOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  // For easy understanding of this algorithm, I will comment the code with an
  // exact example: torch.aten.gather (!torch.vtensor<[1,4,3],f32>,
  // !torch.int-1, !torch.vtensor<[1,4,2],si64>)
  // -> !torch.vtensor<[1,4,2],f32>
  // https://gist.github.com/AmosLewis/2f18434397025211da4491735bcc6db6

  // Not a tensor type.
  auto input = adaptor.getSelf();
  auto inputType = adaptor.getSelf().getType().dyn_cast<RankedTensorType>();
  if (!inputType)
    return rewriter.notifyMatchFailure(
        op, "Only RankedTensorType input are currently supported");

  auto index = adaptor.getIndex();
  auto indexType = adaptor.getIndex().getType().dyn_cast<RankedTensorType>();
  auto inputShape = inputType.getShape();
  int paramsRank = inputShape.size();

  if (!indexType)
    return rewriter.notifyMatchFailure(
        op, "Only RankedTensorType index are currently supported");

  // Check `index` and `input` param should have the same rank
  if (indexType.getRank() != inputType.getRank())
    return rewriter.notifyMatchFailure(
        op, "`index` and `input` param should have the same rank");

  // Dynamic shape check
  if (!inputType.hasStaticShape() || !indexType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "AtenGatherOp: support for dynamic input "
            "shape not implemented");

  // index i64 to i32 for tosa compatitable
  if (indexType.getElementType() != rewriter.getIntegerType(32)) {
    index = rewriter.create<tosa::CastOp>(
        op->getLoc(),
        RankedTensorType::get(indexType.getShape(),
                              rewriter.getIntegerType(32)),
        index);
  }

  // Get positive dim
  int64_t dim{0};
  if (!matchPattern(op.getDim(), m_TorchConstantInt(&dim)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: value `dim` should be a torch constant int");
  dim = toPositiveDim(dim, paramsRank);
  if (!isValidDim(dim, paramsRank))
    return rewriter.notifyMatchFailure(op, "Not dim are invalid");

  // check sparseGrad is bool type
  bool sparseGrad = false;
  if (!matchPattern(op.getSparseGrad(), m_TorchConstantBool(&sparseGrad)))
    return rewriter.notifyMatchFailure(
        op, "only constant boolean `sparse_grad` param supported");
  if (sparseGrad)
    return rewriter.notifyMatchFailure(
        op, "only constant boolean `sparse_grad` == false supported");

  // Get the output type
  auto outType = getTypeConverter()->convertType(op.getType());

  // convert torch style index and dim into tf style indices
  // tensor<[1,4,2],si64> -> tensor<[1,4,2,3],si64>
  auto indicesTf =
      tosa::convertTorchIndexToTfIndices(rewriter, op, input, index, dim);
  if (!indicesTf) {
    return rewriter.notifyMatchFailure(op,
                                       "Convert TorchIndex To TfIndices fail.");
  }

  // do the tf gathernp algorithm with tf style indices as input.
  auto result =
      tosa::convertGatherNdOp(rewriter, op, outType, input, indicesTf.value());

  if (!result) {
    return rewriter.notifyMatchFailure(op, "Convert GatherNdOp fail.");
  }
  rewriter.replaceOp(op, {result.value()});
  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenWhereSelfOp>::matchAndRewrite(
    AtenWhereSelfOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types input are currently supported");
  auto condType = adaptor.getCondition().getType().dyn_cast<TensorType>();
  if (!condType)
    return rewriter.notifyMatchFailure(
        op, "Only tensor types condition are currently supported");

  auto outType = getTypeConverter()->convertType(op.getType());
  rewriter.replaceOpWithNewOp<tosa::SelectOp>(
      op, outType, adaptor.getCondition(), adaptor.getSelf(),
      adaptor.getOther());

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenClampOp>::matchAndRewrite(
    AtenClampOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType)
    return rewriter.notifyMatchFailure(
        op, "only tensor types input are currently supported");

  int64_t int_min, int_max;
  if (!matchPattern(op.getMin(), m_TorchConstantInt(&int_min)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: value `int_min` should be a torch constant int");

  if (!matchPattern(op.getMax(), m_TorchConstantInt(&int_max)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: value `int_max` should be a torch constant int");

  IntegerAttr min_int = rewriter.getI64IntegerAttr(int_min);
  IntegerAttr max_int = rewriter.getI64IntegerAttr(int_max);
  FloatAttr min_fp = rewriter.getF32FloatAttr(float(int_min));
  FloatAttr max_fp = rewriter.getF32FloatAttr(float(int_max));

  auto outType = getTypeConverter()->convertType(op.getType());
  rewriter.replaceOpWithNewOp<tosa::ClampOp>(op, outType, adaptor.getSelf(),
                                             min_int, max_int, min_fp, max_fp);

  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenArangeStartStepOp>::matchAndRewrite(
    AtenArangeStartStepOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  TypeConverter *typeConverter = this->getTypeConverter();
  RankedTensorType resultType =
      typeConverter->convertType(op->getResult(0).getType())
          .cast<RankedTensorType>();

  // At this point all tensors should have value semantics, and hence the
  // `layout` check can be ignored.

  // TODO: Add support for pin_memory features.
  // The pin_memory should be either `False` or `none`.
  bool pinMemory;
  if (!op.getPinMemory().getType().isa<Torch::NoneType>() &&
      (!matchPattern(op.getPinMemory(), m_TorchConstantBool(&pinMemory)) ||
       pinMemory)) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: pin_memory must be either None or false");
  }

  int64_t start, step, end;
  if (!matchPattern(op.getStart(), m_TorchConstantInt(&start)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: value `start` should be a torch constant int");

  if (!matchPattern(op.getEnd(), m_TorchConstantInt(&end)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: value `end` should be a torch constant int");

  if (!matchPattern(op.getStep(), m_TorchConstantInt(&step)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: value `step` should be a torch constant int");

  // The result will always be a 1-d tensor.
  // The size of the result is calculated as follows:
  //          ceil((end - start)/step)
  int64_t resultShape = ceil((float)(end - start) / (float)step);
  SmallVector<int64_t> values(resultShape, start);
  for (unsigned i = 1; i < resultShape; i++)
    values[i] += i * step;
  Value result =
      tosa::getConstTensor<int64_t>(rewriter, op, values, resultShape).value();

  rewriter.replaceOpWithNewOp<tosa::CastOp>(op, resultType, result);
  return success();
}

template <>
LogicalResult ConvertAtenOp<PrimNumToTensorScalarOp>::matchAndRewrite(
    PrimNumToTensorScalarOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  TypeConverter *typeConverter = this->getTypeConverter();
  RankedTensorType resultType =
      typeConverter->convertType(op->getResult(0).getType())
          .cast<RankedTensorType>();

  // Only supports integer operand type, because for the floating point operand
  // type result tensor has to be of type `f64` which is not supported in the
  // tosa.
  int64_t initValue;
  if (!matchPattern(op.getA(), m_TorchConstantInt(&initValue)))
    return rewriter.notifyMatchFailure(
        op, "unimplemented: input should be a torch constant int");

  DenseElementsAttr constAttr = DenseElementsAttr::get(resultType, {initValue});
  rewriter.replaceOpWithNewOp<tosa::ConstOp>(op, resultType, constAttr);
  return success();
}

template <>
LogicalResult ConvertAtenOp<AtenCopyOp>::matchAndRewrite(
    AtenCopyOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  auto srcType = adaptor.getSrc().getType().dyn_cast<TensorType>();
  if (!selfType || !selfType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "Only tensor types with static shape are supported");

  if (!srcType || !srcType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "Only tensor types with static shape are supported");

  // The non_blocking should be a constant `False`.
  bool nonBlocking;
  if (!matchPattern(op.getNonBlocking(), m_TorchConstantBool(&nonBlocking))) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: non_blocking must be a constant");
  } else if (nonBlocking) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: non_blocking is expected to be false");
  }

  SmallVector<int64_t> selfShape(makeShapeTorchCompatible(selfType.getShape()));
  SmallVector<int64_t> srcShape(makeShapeTorchCompatible(srcType.getShape()));

  if (llvm::equal(selfShape, srcShape) || selfShape.size() == 0) {
    // If we reach here, then it means the given case is handled by implicit
    // broadcasting done by tosa.
    Value result;
    if (failed(tosa::tosaCastTensorToType(
            rewriter, op, adaptor.getSrc(),
            getTypeConverter()->convertType(op.getType()), result)))
      return rewriter.notifyMatchFailure(
          op, "unimplemented: cast to result type not supported");
    rewriter.replaceOp(op, result);
    return success();
  }
  return rewriter.notifyMatchFailure(
      op, "unimplemented: valsem.aten.copy op not supported for this case.");
}

//  Legalizes the torch.aten.to.dtype op
template <>
LogicalResult ConvertAtenOp<AtenToDtypeOp>::matchAndRewrite(
    AtenToDtypeOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {

  // Not a tensor type.
  auto selfType = adaptor.getSelf().getType().dyn_cast<TensorType>();
  if (!selfType || !selfType.hasStaticShape())
    return rewriter.notifyMatchFailure(
        op, "Only tensor types with static shape are supported");

  // The non_blocking arg should be a constant `False`.
  bool nonBlocking;
  if (!matchPattern(op.getNonBlocking(), m_TorchConstantBool(&nonBlocking))) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: non_blocking arg must be a constant");
  } else if (nonBlocking) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: non_blocking arg is expected to be false");
  }

  // The copy arg should be a constant `False`.
  bool copy;
  if (!matchPattern(op.getCopy(), m_TorchConstantBool(&copy))) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: copy arg must be a constant");
  } else if (copy) {
    return rewriter.notifyMatchFailure(
        op, "unimplemented: copy arg is expected to be false");
  }

  // Only `none`, `contiguous` and `preserve` memory_format is supported.
  if (!op.getMemoryFormat().getType().isa<Torch::NoneType>()) {
    int64_t memoryFormat;
    if (!matchPattern(op.getMemoryFormat(), m_TorchConstantInt(&memoryFormat)))
      return rewriter.notifyMatchFailure(
          op, "unimplemented: the memory format should be specified in "
              "an integer constant");
    if (memoryFormat != torch_upstream::MemoryFormat::Contiguous &&
        memoryFormat != torch_upstream::MemoryFormat::Preserve)
      return rewriter.notifyMatchFailure(
          op, "unimplemented: only none, contiguous and preserve "
              "memory_format is supported");
  }

  auto resultTy = getTypeConverter()
                      ->convertType(op.getResult().getType())
                      .cast<RankedTensorType>();

  Value result;
  if (failed(tosa::tosaCastTensorToType(rewriter, op, adaptor.getSelf(),
                                        resultTy, result)))
    return rewriter.notifyMatchFailure(op, "conversion to result type failed");

  rewriter.replaceOp(op, result);
  return success();
}

template <typename AtenOpT, typename TosaOpT>
class ConvertAtenPoolingBaseOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;

  // Different pooling variants need to process inputs differently, e.g.
  // adaptive pooling generates the kernel size rather than receive it. This
  // function also transposes inputs.
  virtual LogicalResult processInputs(AtenOpT op, OpAdaptor adaptor,
                                      ConversionPatternRewriter &rewriter,
                                      Value &input, DenseI64ArrayAttr &kernel,
                                      DenseI64ArrayAttr &stride,
                                      DenseI64ArrayAttr &pad,
                                      Type &outputTy) const {
    return rewriter.notifyMatchFailure(
        op, "Unimplemented pooling input parsing function");
  }

  static int64_t getOutputDim(int64_t inputDim, int64_t kernelDim,
                              int64_t stride, int64_t padBefore,
                              int64_t padAfter, int64_t dilation) {
    if (inputDim == kUnknownSize) {
      return kUnknownSize;
    } else {
      return (
          (inputDim + padBefore + padAfter - dilation * (kernelDim - 1) - 1) /
              stride +
          1);
    }
  }

  // Apply the transposeDims vector on input to generate a transposed form.
  Value transposeTensor(AtenOpT op, ConversionPatternRewriter &rewriter,
                        Value input, ArrayRef<int32_t> transposeDims) const {
    auto inputTy = input.getType().template cast<RankedTensorType>();
    auto inputElemTy = inputTy.getElementType();
    auto inputShape = makeShapeTorchCompatible(inputTy.getShape());
    auto inputRank = inputTy.getRank();

    std::optional<Value> transposeDimsConst = tosa::getConstTensor<int32_t>(
        rewriter, op,
        /*vec=*/transposeDims,
        /*shape=*/{static_cast<int32_t>(inputRank)});

    SmallVector<int64_t> transposedInputShape;
    for (auto &dim : transposeDims)
      transposedInputShape.push_back(inputShape[dim]);
    auto transposedInputType = RankedTensorType::get(
        makeShapeLLVMCompatible(transposedInputShape), inputElemTy);
    return rewriter
        .create<tosa::TransposeOp>(op->getLoc(), transposedInputType, input,
                                   transposeDimsConst.value())
        .getResult();
  }

  Value transposePoolingInputToHwc(AtenOpT op,
                                   ConversionPatternRewriter &rewriter,
                                   Value input) const {
    auto inputRank =
        input.getType().template cast<RankedTensorType>().getRank();

    SmallVector<int32_t> nchwToNhwc4DTransposeDims({0, 2, 3, 1});
    SmallVector<int32_t> chwToHwc3DTransposeDims({1, 2, 0});

    return transposeTensor(op, rewriter, input,
                           inputRank == 3 ? chwToHwc3DTransposeDims
                                          : nchwToNhwc4DTransposeDims);
  }

  Value transposePoolingOutputToChw(AtenOpT op,
                                    ConversionPatternRewriter &rewriter,
                                    Value input) const {
    auto inputTy = input.getType().template cast<RankedTensorType>();
    auto inputRank = inputTy.getRank();

    SmallVector<int32_t> nhwcToNchw4DTransposeDims({0, 3, 1, 2});
    SmallVector<int32_t> hwcToChw3DTransposeDims({2, 0, 1});

    return transposeTensor(op, rewriter, input,
                           inputRank == 3 ? hwcToChw3DTransposeDims
                                          : nhwcToNchw4DTransposeDims);
  }

  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input;
    DenseI64ArrayAttr kernel, stride, pad;
    Type outputTy;

    // Attempts to read input and kernel parameters, or synthesize them in the
    // case of adaptive pooling. Also performs input CHW->HWC transpose.
    if (failed(processInputs(op, adaptor, rewriter, input, kernel, stride, pad,
                             outputTy)))
      return rewriter.notifyMatchFailure(
          op, "Failed to process inputs for pooling");

    auto pooledOutput =
        rewriter
            .create<TosaOpT>(op->getLoc(), outputTy, input, kernel, stride, pad)
            .getResult();

    auto transposedOutput =
        ConvertAtenPoolingBaseOp<AtenOpT, TosaOpT>::transposePoolingOutputToChw(
            op, rewriter, pooledOutput);

    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        op,
        OpConversionPattern<AtenOpT>::getTypeConverter()->convertType(
            op.getType()),
        transposedOutput);

    return success();
  }
};

template <typename AtenOpT, typename TosaOpT>
class ConvertAtenAdaptivePoolingOp
    : public ConvertAtenPoolingBaseOp<AtenOpT, TosaOpT> {
public:
  using ConvertAtenPoolingBaseOp<AtenOpT, TosaOpT>::ConvertAtenPoolingBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult processInputs(AtenOpT op, OpAdaptor adaptor,
                              ConversionPatternRewriter &rewriter, Value &input,
                              DenseI64ArrayAttr &kernel,
                              DenseI64ArrayAttr &stride, DenseI64ArrayAttr &pad,
                              Type &outputTy) const override {
    auto inputXchw = adaptor.getSelf();
    auto inputTy = inputXchw.getType().template cast<RankedTensorType>();
    if (!inputTy)
      return rewriter.notifyMatchFailure(
          op, "Adaptive avgpool requires ranked tensor input");

    auto inputShape = makeShapeTorchCompatible(inputTy.getShape());
    auto inputRank = inputTy.getRank();
    auto inputElemTy = inputTy.getElementType();

    // Rank sanity check.
    if (inputTy.getRank() != 4 && inputRank != 3)
      return rewriter.notifyMatchFailure(
          op, "NCHW->NHWC transpose requires 3D or 4D tensor");

    int64_t inputHDim = inputShape[inputRank - 2];
    int64_t inputWDim = inputShape[inputRank - 1];

    SmallVector<int64_t> outputSize;
    if (!matchPattern(op.getOutputSize(),
                      m_TorchListOfConstantInts(outputSize)))
      return rewriter.notifyMatchFailure(
          op, "Non-const output_size for adaptive pooling unsupported.");

    SmallVector<int64_t> kernelDims;
    int64_t outputHDim, outputWDim;
    if (outputSize.size() == 1) {
      outputHDim = outputWDim = outputSize[0];
    } else {
      if (outputSize.size() != 2)
        return rewriter.notifyMatchFailure(
            op, "Adaptive avgpool output_size not 1 or 2 elements.");

      // Assumes 'None' (e.g. output_size=(None, 5) ) is expressed as <=0.
      outputHDim =
          (outputSize[0] <= 0) ? inputShape[inputRank - 2] : outputSize[0];
      outputWDim =
          (outputSize[1] <= 0) ? inputShape[inputRank - 1] : outputSize[1];
    }

    // In adaptive pooling,
    // stride = inputDim // outputDim
    // kernel = inputDim - (outputDim-1)* stride
    // pad = 0, dilation = 1

    int64_t strideH = inputShape[inputRank - 2] / outputHDim;
    int64_t strideW = inputShape[inputRank - 1] / outputWDim;

    kernelDims.push_back(inputHDim - (outputHDim - 1) * strideH);
    kernelDims.push_back(inputWDim - (outputWDim - 1) * strideW);

    SmallVector<int64_t> outputShape;
    if (inputRank > 3)
      outputShape.push_back(inputShape[0]);
    outputShape.push_back(outputHDim);
    outputShape.push_back(outputWDim);
    outputShape.push_back(inputShape[inputRank - 3]);

    // Transpose to xHWC
    input =
        ConvertAtenPoolingBaseOp<AtenOpT, TosaOpT>::transposePoolingInputToHwc(
            op, rewriter, inputXchw);
    kernel = rewriter.getDenseI64ArrayAttr(kernelDims);
    stride = rewriter.getDenseI64ArrayAttr({strideH, strideW});
    // Adaptive pooling does unit dilation and zero pad.
    pad = rewriter.getDenseI64ArrayAttr({0, 0, 0, 0});
    outputTy = RankedTensorType::get(makeShapeLLVMCompatible(outputShape),
                                     inputElemTy);

    return success();
  }
};

template <typename AtenOpT, typename tosaOp>
static Type getOutputTypeForNonAdaptivePoolingOp(
    RankedTensorType inputTy, SmallVectorImpl<int64_t> &kernelSize,
    SmallVectorImpl<int64_t> &strideArray, SmallVectorImpl<int64_t> &padArray,
    SmallVectorImpl<int64_t> &dilationArray) {
  auto inputShape = makeShapeTorchCompatible(inputTy.getShape());
  auto inputRank = inputTy.getRank();
  auto inputElemTy = inputTy.getElementType();

  int64_t outputHDim = ConvertAtenPoolingBaseOp<AtenOpT, tosaOp>::getOutputDim(
      inputShape[inputRank - 2], kernelSize[0], strideArray[0], padArray[0],
      padArray[0], dilationArray[0]);
  int64_t outputWDim = ConvertAtenPoolingBaseOp<AtenOpT, tosaOp>::getOutputDim(
      inputShape[inputRank - 1], kernelSize[1], strideArray[1], padArray[1],
      padArray[1], dilationArray[1]);
  SmallVector<int64_t> outputShape;
  if (inputRank > 3)
    outputShape.push_back(inputShape[0]);
  outputShape.push_back(outputHDim);
  outputShape.push_back(outputWDim);
  outputShape.push_back(inputShape[inputRank - 3]);
  return RankedTensorType::get(makeShapeLLVMCompatible(outputShape),
                               inputElemTy);
}

// Checks the validity of pooling parameters and stores them in the respective
// vector. Also, gets the output type for the pooling op.
template <typename AtenOpT, typename tosaOp>
static LogicalResult getOutputTypeAndPoolingParameters(
    AtenOpT op, ConversionPatternRewriter &rewriter, Value inputXchw,
    SmallVectorImpl<int64_t> &dilationArray, Type &outputTy,
    DenseI64ArrayAttr &kernel, DenseI64ArrayAttr &stride,
    DenseI64ArrayAttr &pad) {

  RankedTensorType inputTy = inputXchw.getType().cast<RankedTensorType>();
  if (!inputTy)
    return rewriter.notifyMatchFailure(
        op, "Pooling op requires ranked tensor input");

  auto inputRank = inputTy.getRank();
  // Rank sanity check.
  if (inputTy.getRank() != 4 && inputRank != 3)
    return rewriter.notifyMatchFailure(
        op, "NCHW->NHWC transpose requires 3D or 4D tensor");

  SmallVector<int64_t, 2> kernelSizeInts, strideInts, paddingInts;
  if (!matchPattern(op.getKernelSize(),
                    m_TorchListOfConstantInts(kernelSizeInts)))
    return rewriter.notifyMatchFailure(
        op, "Non-const kernel_size for pooling op unsupported");
  if (!matchPattern(op.getStride(), m_TorchListOfConstantInts(strideInts)))
    return rewriter.notifyMatchFailure(
        op, "Non-const stride for pooling op unsupported");
  if (!matchPattern(op.getPadding(), m_TorchListOfConstantInts(paddingInts)))
    return rewriter.notifyMatchFailure(
        op, "Non-const padding factor for pooling op unsupported");

  kernel = rewriter.getDenseI64ArrayAttr(kernelSizeInts);
  stride = rewriter.getDenseI64ArrayAttr(strideInts);
  pad = rewriter.getDenseI64ArrayAttr(
      {paddingInts[0], paddingInts[0], paddingInts[1], paddingInts[1]});

  // FIXME: add ceil_mode support.
  bool ceilMode;
  if (!matchPattern(op.getCeilMode(), m_TorchConstantBool(&ceilMode)))
    return rewriter.notifyMatchFailure(
        op, "only support constant bool ceil_mode for pooling op");
  if (ceilMode)
    return rewriter.notifyMatchFailure(
        op, "only support ceil_mode equals to False for pooling op");

  outputTy = getOutputTypeForNonAdaptivePoolingOp<AtenOpT, tosaOp>(
      inputTy, kernelSizeInts, strideInts, paddingInts, dilationArray);

  return success();
}

class ConvertAtenMaxPool2dOp
    : public ConvertAtenPoolingBaseOp<AtenMaxPool2dOp, tosa::MaxPool2dOp> {
public:
  using ConvertAtenPoolingBaseOp<AtenMaxPool2dOp,
                                 tosa::MaxPool2dOp>::ConvertAtenPoolingBaseOp;
  LogicalResult processInputs(AtenMaxPool2dOp op, OpAdaptor adaptor,
                              ConversionPatternRewriter &rewriter, Value &input,
                              DenseI64ArrayAttr &kernel,
                              DenseI64ArrayAttr &stride, DenseI64ArrayAttr &pad,
                              Type &outputTy) const override {
    SmallVector<int64_t, 2> dilationArray;
    if (!matchPattern(op.getDilation(),
                      m_TorchListOfConstantInts(dilationArray)))
      return rewriter.notifyMatchFailure(
          op, "Non-const dilation for pooling op unsupported.");
    // TOSA pooling only supports unit dilation.
    if (dilationArray[0] > 1 || dilationArray[1] > 1)
      return rewriter.notifyMatchFailure(
          op, "Cannot process non-unit pooling dilation.");

    if (failed(getOutputTypeAndPoolingParameters<AtenMaxPool2dOp,
                                                 tosa::MaxPool2dOp>(
            op, rewriter, adaptor.getSelf(), dilationArray, outputTy, kernel,
            stride, pad)))
      return rewriter.notifyMatchFailure(
          op, "invalid pooling parameters or input type");

    // Transpose to xHWC
    input = ConvertAtenPoolingBaseOp<AtenMaxPool2dOp, tosa::MaxPool2dOp>::
        transposePoolingInputToHwc(op, rewriter, adaptor.getSelf());

    return success();
  }
};

class ConvertAtenAvgPool2dOp
    : public ConvertAtenPoolingBaseOp<AtenAvgPool2dOp, tosa::AvgPool2dOp> {
public:
  using ConvertAtenPoolingBaseOp<AtenAvgPool2dOp,
                                 tosa::AvgPool2dOp>::ConvertAtenPoolingBaseOp;
  LogicalResult processInputs(AtenAvgPool2dOp op, OpAdaptor adaptor,
                              ConversionPatternRewriter &rewriter, Value &input,
                              DenseI64ArrayAttr &kernel,
                              DenseI64ArrayAttr &stride, DenseI64ArrayAttr &pad,
                              Type &outputTy) const override {
    SmallVector<int64_t, 2> dilationArray{1, 1};
    if (failed(getOutputTypeAndPoolingParameters<AtenAvgPool2dOp,
                                                 tosa::AvgPool2dOp>(
            op, rewriter, adaptor.getSelf(), dilationArray, outputTy, kernel,
            stride, pad)))
      return rewriter.notifyMatchFailure(
          op, "invalid pooling parameters or input type");

    // Transpose to xHWC
    input = ConvertAtenPoolingBaseOp<AtenAvgPool2dOp, tosa::AvgPool2dOp>::
        transposePoolingInputToHwc(op, rewriter, adaptor.getSelf());

    return success();
  }
};

// Ref: Error checking based on the Torch to LinAlg lowering
template <typename AtenOpT, int fillVal>
class ConvertAtenConstPatternOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    auto outType = OpConversionPattern<AtenOpT>::getTypeConverter()
                       ->convertType(op.getType())
                       .template dyn_cast<TensorType>();

    if (!outType)
      return rewriter.notifyMatchFailure(op,
                                         "Only Tensor types supported in TOSA");

    Type outElemTy = outType.getElementType();
    if (!outElemTy.isIntOrFloat())
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");

    // FIXME: Handle layout, device and pin_memory. Assume dtype has been
    // processed to set output type correctly?
    if (!op.getLayout().getType().template isa<Torch::NoneType>())
      return rewriter.notifyMatchFailure(op,
                                         "Only default layout is supported");

    bool pinMemory;
    if (!op.getPinMemory().getType().template isa<Torch::NoneType>() &&
        (!matchPattern(op.getPinMemory(), m_TorchConstantBool(&pinMemory)) ||
         pinMemory)) {
      return rewriter.notifyMatchFailure(
          op, "Unsupported pin_memory, should be either None or false");
    }

    SmallVector<int64_t> shape;
    if (!matchPattern(op.getSize(), m_TorchListOfConstantInts(shape))) {
      return rewriter.notifyMatchFailure(
          op, "Shape must be a list of Scalar constants");
    }

    int64_t size = 1;
    for (auto s : shape)
      size *= s;

    SmallVector<int32_t> values(size, fillVal);
    auto constOp =
        tosa::getConstTensor<int32_t>(rewriter, op, values, shape).value();

    rewriter.replaceOpWithNewOp<tosa::CastOp>(op, outType, constOp);

    return success();
  }
};

template <typename AtenOpT>
class ConvertAtenFillScalarOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto outType = OpConversionPattern<AtenOpT>::getTypeConverter()
                       ->convertType(op.getType())
                       .template dyn_cast<TensorType>();

    if (!outType || !outType.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "Only Tensor types with static shapes are currently supported");

    Type outElemTy = outType.getElementType();
    if (!outElemTy.isIntOrFloat()) {
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");
    }
    Value constOp;
    if (failed(torchScalarToTosaTensor(
            rewriter, op, op.getValue(), constOp, outElemTy,
            makeShapeTorchCompatible(outType.getShape()))))
      return rewriter.notifyMatchFailure(
          op, "Supplied value must be a Scalar constant");

    rewriter.replaceOpWithNewOp<tosa::CastOp>(op, outType, constOp);

    return success();
  }
};

template <typename AtenOpT>
class ConvertAtenMaskedFillOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto outType = OpConversionPattern<AtenOpT>::getTypeConverter()
                       ->convertType(op.getType())
                       .template dyn_cast<TensorType>();

    if (!outType || !outType.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "Only Tensor types with static shapes are currently supported");

    Type outElemTy = outType.getElementType();
    if (!outElemTy.isIntOrFloat()) {
      return rewriter.notifyMatchFailure(
          op, "Only floating-point or integer datatype legalization supported");
    }

    // Not a tensor type.
    auto selfType = adaptor.getSelf().getType().template dyn_cast<TensorType>();
    if (!selfType || !outType.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op,
          "Only tensor types with static shapes input are currently supported");

    auto maskType = adaptor.getMask().getType().template dyn_cast<TensorType>();
    if (!maskType)
      return rewriter.notifyMatchFailure(
          op, "Only tensor types mask are currently supported");

    Value rhs = adaptor.getValue();
    auto rhsType = rhs.getType().template dyn_cast<TensorType>();
    Value rhsAsTensor;
    if (!rhsType) { // scalar
      if (failed(torchScalarToTosaTensor(rewriter, op, op.getValue(),
                                         rhsAsTensor, rhs.getType(), {})))
        return rewriter.notifyMatchFailure(
            op, "Currently only scalar constants are supported for "
                "conversion in TOSA operation");
    } else { // tensor
      rhsType = rhs.getType().dyn_cast<TensorType>();
    }

    auto rhsTensor = rhsType ? rhs : rhsAsTensor;
    auto rhsTensorType = rhsTensor.getType().template dyn_cast<TensorType>();
    if (rhsTensorType.getElementType() != outElemTy)
      rhsTensor = rewriter.create<tosa::CastOp>(
          op.getLoc(),
          RankedTensorType::get(rhsTensorType.getShape(), outElemTy),
          rhsTensor);

    rewriter.replaceOpWithNewOp<tosa::SelectOp>(op, outType, adaptor.getMask(),
                                                rhsTensor, adaptor.getSelf());
    return success();
  }
};

// Legalizes the torch.clone op.
template <typename AtenOpT>
class ConvertAtenCloneOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    int64_t memoryFormat;
    if (!op.getMemoryFormat().getType().template isa<Torch::NoneType>() &&
        (!matchPattern(op.getMemoryFormat(),
                       m_TorchConstantInt(&memoryFormat)) ||
         memoryFormat != torch_upstream::MemoryFormat::Contiguous)) {
      return op.emitError(
          "unimplemented: only default memory format is supported");
    }
    auto outType = OpConversionPattern<AtenOpT>::getTypeConverter()
                       ->convertType(op.getType())
                       .template dyn_cast<TensorType>();
    rewriter.replaceOpWithNewOp<tosa::CastOp>(op, outType, adaptor.getSelf());

    return success();
  }
};

} // namespace

// -----------------------------------------------------------------------------
// TorchToTosa Pass
// -----------------------------------------------------------------------------

namespace {
class ConvertTorchToTosa : public ConvertTorchToTosaBase<ConvertTorchToTosa> {
public:
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<tosa::TosaDialect>();
    registry.insert<tensor::TensorDialect>();
    registry.insert<arith::ArithDialect>();
    TorchConversion::getBackendTypeConversionDependentDialects(registry);
  }

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ConversionTarget target(*context);
    target.addLegalDialect<tosa::TosaDialect, tensor::TensorDialect,
                           arith::ArithDialect>();

    TypeConverter typeConverter;
    typeConverter.addConversion([](Type type) { return type; });
    TorchConversion::setupBackendTypeConversion(target, typeConverter);

    RewritePatternSet patterns(context);

#define INSERT_UNARY_FPONLY_PATTERN(AtenOp, TosaOp)                            \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenUnaryFPOnlyOp<AtenOp, TosaOp>>(typeConverter,        \
                                                         context);
    INSERT_UNARY_FPONLY_PATTERN(AtenLogOp, tosa::LogOp)
    INSERT_UNARY_FPONLY_PATTERN(AtenExpOp, tosa::ExpOp)
#undef INSERT_UNARY_FPONLY_PATTERN

#define INSERT_UNARY_PATTERN(AtenOp, TosaOp)                                   \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenUnaryOp<AtenOp, TosaOp>>(typeConverter, context);
    INSERT_UNARY_PATTERN(AtenNegOp, tosa::NegateOp)
    INSERT_UNARY_PATTERN(AtenFloorOp, tosa::FloorOp)
    INSERT_UNARY_PATTERN(AtenRsqrtOp, tosa::RsqrtOp)
    INSERT_UNARY_PATTERN(AtenBitwiseNotOp, tosa::BitwiseNotOp)
    INSERT_UNARY_PATTERN(AtenCeilOp, tosa::CeilOp)
    INSERT_UNARY_PATTERN(AtenReciprocalOp, tosa::ReciprocalOp)
#undef INSERT_UNARY_PATTERN

#define INSERT_BINARY_PATTERN(AtenOp, TosaOp)                                  \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenBinaryOp<AtenOp, TosaOp>>(typeConverter, context);
    INSERT_BINARY_PATTERN(AtenMaximumOp, tosa::MaximumOp)
    INSERT_BINARY_PATTERN(AtenMinimumOp, tosa::MinimumOp)
#undef INSERT_BINARY_PATTERN

#define INSERT_BINARY_ADDSUB_PATTERN(AtenOp, TosaOp)                           \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenAddSubOp<AtenOp, TosaOp>>(typeConverter, context);
    INSERT_BINARY_ADDSUB_PATTERN(AtenAddTensorOp, tosa::AddOp)
    INSERT_BINARY_ADDSUB_PATTERN(AtenAddScalarOp, tosa::AddOp)
    INSERT_BINARY_ADDSUB_PATTERN(AtenSubTensorOp, tosa::SubOp)
    INSERT_BINARY_ADDSUB_PATTERN(AtenSubScalarOp, tosa::SubOp)
#undef INSERT_BINARY_ADDSUB_PATTERN

#define INSERT_BINARY_COMPARE_PATTERN(AtenOp, TosaOp)                          \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenCompareOp<AtenOp, TosaOp>>(typeConverter, context);
    INSERT_BINARY_COMPARE_PATTERN(AtenGtTensorOp, tosa::GreaterOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenGtScalarOp, tosa::GreaterOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenLtTensorOp, tosa::GreaterOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenLtScalarOp, tosa::GreaterOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenEqTensorOp, tosa::EqualOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenEqScalarOp, tosa::EqualOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenNeTensorOp, tosa::EqualOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenNeScalarOp, tosa::EqualOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenBitwiseAndTensorOp, tosa::BitwiseAndOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenBitwiseOrTensorOp, tosa::BitwiseOrOp)
    INSERT_BINARY_COMPARE_PATTERN(AtenBitwiseXorTensorOp, tosa::BitwiseXorOp)
#undef INSERT_BINARY_COMPARE_PATTERN

#define INSERT_BINARY_MUL_PATTERN(AtenOp)                                      \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMulOp<AtenOp>>(typeConverter, context);
    INSERT_BINARY_MUL_PATTERN(AtenMulTensorOp);
    INSERT_BINARY_MUL_PATTERN(AtenMulScalarOp);
#undef INSERT_BINARY_MUL_PATTERN

#define INSERT_BINARY_DIV_PATTERN(AtenOp)                                      \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenDivOp<AtenOp>>(typeConverter, context);
    INSERT_BINARY_DIV_PATTERN(AtenDivTensorOp);
    INSERT_BINARY_DIV_PATTERN(AtenDivScalarOp);
#undef INSERT_BINARY_DIV_PATTERN

#define INSERT_NDIMS_REDUCTION_OP_PATTERN(AtenOp, ConversionFunc)              \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMultipleDimsReductionOp<AtenOp, ConversionFunc>>(    \
      typeConverter, context);
    INSERT_NDIMS_REDUCTION_OP_PATTERN(AtenMeanDimOp,
                                      mlir::tosa::convertReduceMeanOp)
    INSERT_NDIMS_REDUCTION_OP_PATTERN(AtenSumDimIntListOp,
                                      mlir::tosa::convertReduceSumOp)
#undef INSERT_NDIMS_REDUCTION_OP_PATTERN

#define INSERT_ONEDIM_REDUCTION_OP_PATTERN(AtenOp, ConversionFunc)             \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenOneDimReductionOp<AtenOp, ConversionFunc>>(          \
      typeConverter, context);
    INSERT_ONEDIM_REDUCTION_OP_PATTERN(AtenAnyDimOp,
                                       mlir::tosa::convertReduceAnyOp)
#undef INSERT_ONEDIM_REDUCTION_OP_PATTERN

#define INSERT_ALLDIMS_REDUCTION_OP_PATTERN(AtenOp, ConversionFunc)            \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenAllDimsReductionOp<AtenOp, ConversionFunc>>(         \
      typeConverter, context);
    INSERT_ALLDIMS_REDUCTION_OP_PATTERN(AtenAllOp,
                                        mlir::tosa::convertReduceAllOp)
    INSERT_ALLDIMS_REDUCTION_OP_PATTERN(AtenAnyOp,
                                        mlir::tosa::convertReduceAnyOp)
    INSERT_ALLDIMS_REDUCTION_OP_PATTERN(AtenSumOp,
                                        mlir::tosa::convertReduceSumOp)
#undef INSERT_ALLDIMS_REDUCTION_OP_PATTERN

#define INSERT_SQUEEZE_OP_PATTERN(AtenOp, TemplateForm)                        \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<TemplateForm<AtenOp>>(typeConverter, context);
    INSERT_SQUEEZE_OP_PATTERN(AtenSqueezeOp, ConvertAtenSqueezeAllDimsOp)
    INSERT_SQUEEZE_OP_PATTERN(AtenSqueezeDimOp, ConvertAtenSqueezeOneDimOp)
#undef INSERT_SQUEEZE_OP_PATTERN

#define INSERT_MATMUL_ATENOP_PATTERN(AtenOp)                                   \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMatMulOp<AtenOp>>(typeConverter, context);
    INSERT_MATMUL_ATENOP_PATTERN(AtenMatmulOp);
#undef INSERT_MATMUL_ATEMOP_PATTERN

#define INSERT_MM_ATENOP_PATTERN(AtenOp)                                       \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMmOp<AtenOp>>(typeConverter, context);
    INSERT_MM_ATENOP_PATTERN(AtenMmOp);
    INSERT_MM_ATENOP_PATTERN(AtenBmmOp);
#undef INSERT_MM_ATEMOP_PATTERN

#define INSERT_LINEAR_ATENOP_PATTERN(AtenOp)                                   \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenLinearOp<AtenOp>>(typeConverter, context);
    INSERT_LINEAR_ATENOP_PATTERN(AtenLinearOp);
#undef INSERT_LINEAR_ATEMOP_PATTERN

#define INSERT_ADAPTIVE_POOLING_ATENOP_PATTERN(AtenOp, TosaOpT)                \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenAdaptivePoolingOp<AtenOp, TosaOpT>>(typeConverter,   \
                                                              context);
    INSERT_ADAPTIVE_POOLING_ATENOP_PATTERN(AtenAdaptiveAvgPool2dOp,
                                           tosa::AvgPool2dOp);
#undef INSERT_ADAPTIVE_POOLING_ATEMOP_PATTERN

    target.addIllegalOp<AtenMaxPool2dOp>();
    patterns.add<ConvertAtenMaxPool2dOp>(typeConverter, context);

    target.addIllegalOp<AtenAvgPool2dOp>();
    patterns.add<ConvertAtenAvgPool2dOp>(typeConverter, context);

#define INSERT_CONSTANT_FILL_PATTERN(AtenOp, fillVal)                          \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenConstPatternOp<AtenOp, fillVal>>(typeConverter,      \
                                                           context);
    INSERT_CONSTANT_FILL_PATTERN(AtenOnesOp, 1);
    INSERT_CONSTANT_FILL_PATTERN(AtenZerosOp, 0);
#undef INSERT_CONSTANT_FILL_PATTERN

#define INSERT_FILL_SCALAR_PATTERN(AtenOp)                                     \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenFillScalarOp<AtenOp>>(typeConverter, context);
    INSERT_FILL_SCALAR_PATTERN(AtenFill_ScalarOp);
#undef INSERT_FILL_SCALAR_PATTERN

#define INSERT_MASKED_FILL_PATTERN(AtenOp)                                     \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMaskedFillOp<AtenOp>>(typeConverter, context);
    INSERT_MASKED_FILL_PATTERN(AtenMaskedFillScalarOp);
    INSERT_MASKED_FILL_PATTERN(AtenMaskedFillTensorOp);
#undef INSERT_MASKED_FILL_PATTERN

#define INSERT_ATENOP_PATTERN(AtenOp)                                          \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenOp<AtenOp>>(typeConverter, context);
    INSERT_ATENOP_PATTERN(AtenTanhOp);
    INSERT_ATENOP_PATTERN(AtenSigmoidOp);
    INSERT_ATENOP_PATTERN(AtenReluOp);
    INSERT_ATENOP_PATTERN(AtenLeakyReluOp);
    INSERT_ATENOP_PATTERN(AtenArgmaxOp);
    INSERT_ATENOP_PATTERN(AtenPowTensorScalarOp);
    INSERT_ATENOP_PATTERN(AtenRsubScalarOp);
    INSERT_ATENOP_PATTERN(AtenConvolutionOp);
    INSERT_ATENOP_PATTERN(ValueTensorLiteralOp);
    INSERT_ATENOP_PATTERN(AtenReshapeOp);
    INSERT_ATENOP_PATTERN(AtenBatchNormOp);
    INSERT_ATENOP_PATTERN(AtenNativeLayerNormOp);
    INSERT_ATENOP_PATTERN(AtenFlattenUsingIntsOp);
    INSERT_ATENOP_PATTERN(AtenPermuteOp);
    INSERT_ATENOP_PATTERN(AtenLog2Op);
    INSERT_ATENOP_PATTERN(AtenThresholdOp);
    INSERT_ATENOP_PATTERN(AtenUnsqueezeOp);
    INSERT_ATENOP_PATTERN(AtenContiguousOp);
    INSERT_ATENOP_PATTERN(AtenDropoutOp);
    INSERT_ATENOP_PATTERN(AtenViewOp);
    INSERT_ATENOP_PATTERN(AtenGeluOp);
    INSERT_ATENOP_PATTERN(AtenGeluBackwardOp);
    INSERT_ATENOP_PATTERN(AtenEmbeddingOp);
    INSERT_ATENOP_PATTERN(AtenTransposeIntOp);
    INSERT_ATENOP_PATTERN(AtenMaxDimOp);
    INSERT_ATENOP_PATTERN(AtenSliceTensorOp);
    INSERT_ATENOP_PATTERN(AtenBroadcastToOp);
    INSERT_ATENOP_PATTERN(AtenGatherOp);
    INSERT_ATENOP_PATTERN(AtenWhereSelfOp);
    INSERT_ATENOP_PATTERN(AtenClampOp);
    INSERT_ATENOP_PATTERN(AtenArangeStartStepOp);
    INSERT_ATENOP_PATTERN(PrimNumToTensorScalarOp);
    INSERT_ATENOP_PATTERN(AtenCopyOp);
    INSERT_ATENOP_PATTERN(AtenToDtypeOp);
#undef INSERT_ATENOP_PATTERN

#define INSERT_CLONE_ATENOP_PATTERN(AtenOp)                                    \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenCloneOp<AtenOp>>(typeConverter, context);
    INSERT_CLONE_ATENOP_PATTERN(AtenCloneOp);
#undef INSERT_CLONE_ATENOP_PATTERN

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      return signalPassFailure();
  }
};
} // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
mlir::torch::createConvertTorchToTosaPass() {
  return std::make_unique<ConvertTorchToTosa>();
}

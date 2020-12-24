//===- DynamicSize.cpp - Dynamic size related APIs --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines APIs that track and query dynamic size information.
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Core/PathSensitive/DynamicSize.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/LLVM.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymbolManager.h"


REGISTER_MAP_WITH_PROGRAMSTATE(DynamicSizeMap, const clang::ento::MemRegion *,
                               clang::ento::DefinedOrUnknownSVal)

namespace clang {
namespace ento {

/// Helper to bypass the top-level ElementRegion of \p MR.
static const MemRegion *getSuperRegion(const MemRegion *MR) {

  assert(MR);
  if (const auto *ER = MR->getAs<ElementRegion>())
    MR = ER->getSuperRegion();
  return MR;
}


DefinedOrUnknownSVal getDynamicSize(ProgramStateRef State, const MemRegion *MR,
                                    SValBuilder &SVB) {
  MR = getSuperRegion(MR);

  if (const DefinedOrUnknownSVal *Size = State->get<DynamicSizeMap>(MR))
    return *Size;

  return MR->getMemRegionManager().getStaticSize(MR, SVB);
}

DefinedOrUnknownSVal getElementSize(QualType Ty, SValBuilder &SVB) {
  return SVB.makeIntVal(SVB.getContext().getTypeSizeInChars(Ty).getQuantity(),
                        SVB.getArrayIndexType());
}

static DefinedOrUnknownSVal getSize(ProgramStateRef State, SVal ElementCount,
                                    QualType Ty, SValBuilder &SVB) {
  DefinedOrUnknownSVal ElementSize = getElementSize(Ty, SVB);

  return SVB
      .evalBinOp(State, BO_Mul, ElementCount, ElementSize,
                 SVB.getArrayIndexType())
      .castAs<DefinedOrUnknownSVal>();
}

DefinedOrUnknownSVal getDynamicElementCount(ProgramStateRef State,
                                            const MemRegion *MR,
                                            SValBuilder &SVB,
                                            QualType Ty) {

  MR = getSuperRegion(MR);

  DefinedOrUnknownSVal Size = getDynamicSize(State, MR, SVB);
  SVal ElementSize = getElementSize(Ty, SVB);




  SVal DivisionV =
      SVB.evalBinOp(State, BO_Div, Size, ElementSize, SVB.getArrayIndexType());

  return DivisionV.castAs<DefinedOrUnknownSVal>();
}

SVal getDynamicSizeWithOffset(ProgramStateRef State, const SVal &BufV) {
  SValBuilder &SvalBuilder = State->getStateManager().getSValBuilder();
  const MemRegion *MRegion = BufV.getAsRegion();
  if (!MRegion)
    return UnknownVal();
  RegionOffset Offset = MRegion->getAsOffset();
  if (Offset.hasSymbolicOffset())
    return UnknownVal();
  const MemRegion *BaseRegion = MRegion->getBaseRegion();
  if (!BaseRegion)
    return UnknownVal();

  NonLoc OffsetInBytes = SvalBuilder.makeArrayIndex(
      Offset.getOffset() /
      MRegion->getMemRegionManager().getContext().getCharWidth());
  DefinedOrUnknownSVal ExtentInBytes =
      getDynamicSize(State, BaseRegion, SvalBuilder);

  return SvalBuilder.evalBinOp(State, BinaryOperator::Opcode::BO_Sub,
                               ExtentInBytes, OffsetInBytes,
                               SvalBuilder.getArrayIndexType());
}


ProgramStateRef setDynamicSize(ProgramStateRef State, const MemRegion *MR,
                               DefinedOrUnknownSVal Size, SValBuilder &SVB) {
  if (Size.isUnknown())
    return State;

  /* FIXME: Make this work.
  if (const auto CI = Size.getAs<nonloc::ConcreteInt>())
     assert(CI->getValue().isUnsigned());*/

  MR = getSuperRegion(MR);
  return State->set<DynamicSizeMap>(MR, Size);
}

ProgramStateRef setDynamicSize(ProgramStateRef State, const MemRegion *MR,
                               const CXXNewExpr *NE,
                               const LocationContext *LCtx, SValBuilder &SVB) {
  SVal ElementCount;
  if (const Expr *SizeExpr = NE->getArraySize().getValueOr(nullptr)) {
    ElementCount = State->getSVal(SizeExpr, LCtx);
  } else {
    ElementCount = SVB.makeIntVal(1, /*IsUnsigned=*/true);
  }

  return setDynamicSize(
      State, MR, getSize(State, ElementCount, NE->getAllocatedType(), SVB),
      SVB);
}

} // namespace ento
} // namespace clang

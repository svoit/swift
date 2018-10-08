//===--- SILGenLValue.cpp - Constructs logical lvalues for SILGen ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Emission of l-value expressions and basic operations on them.
//
//===----------------------------------------------------------------------===//

#include "ASTVisitor.h"
#include "ArgumentScope.h"
#include "ArgumentSource.h"
#include "Conversion.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "SILGen.h"
#include "Scope.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//

namespace {

struct LValueWritebackCleanup : Cleanup {
  FormalEvaluationContext::stable_iterator Depth;

  LValueWritebackCleanup() : Depth() {}

  void emit(SILGenFunction &SGF, CleanupLocation loc,
            ForUnwind_t forUnwind) override {
    FullExpr scope(SGF.Cleanups, loc);

    // TODO: honor forUnwind!
    auto &evaluation = *SGF.FormalEvalContext.find(Depth);
    assert(evaluation.getKind() == FormalAccess::Exclusive);
    auto &lvalue = static_cast<ExclusiveBorrowFormalAccess &>(evaluation);
    lvalue.performWriteback(SGF, /*isFinal*/ false);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "LValueWritebackCleanup\n"
                 << "State: " << getState() << " Depth: " << Depth.getDepth()
                 << "\n";
#endif
  }
};

} // end anonymous namespace

/// Push a writeback onto the current LValueWriteback stack.
static void pushWriteback(SILGenFunction &SGF,
                          SILLocation loc,
                          std::unique_ptr<LogicalPathComponent> &&comp,
                          ManagedValue base,
                          MaterializedLValue materialized) {
  assert(SGF.InFormalEvaluationScope);

  // Push a cleanup to execute the writeback consistently.
  auto &context = SGF.FormalEvalContext;
  LValueWritebackCleanup &cleanup =
      SGF.Cleanups.pushCleanup<LValueWritebackCleanup>();
  CleanupHandle handle = SGF.Cleanups.getTopCleanup();

  context.push<ExclusiveBorrowFormalAccess>(loc, std::move(comp), base,
                                            materialized, handle);
  cleanup.Depth = context.stable_begin();
}

static bool areCertainlyEqualIndices(const Expr *e1, const Expr *e2);

void ExclusiveBorrowFormalAccess::diagnoseConflict(
                                    const ExclusiveBorrowFormalAccess &rhs,
                                    SILGenFunction &SGF) const {
  // If the two writebacks we're comparing are of different kinds (e.g.
  // ownership conversion vs a computed property) then they aren't the
  // same and thus cannot conflict.
  if (component->getKind() != rhs.component->getKind())
    return;

  // If the lvalues don't have the same base value (possibly null), then
  // they aren't the same.  Note that this is the primary source of false
  // negative for this diagnostic.
  SILValue lhsBaseValue = base.getValue(), rhsBaseValue = rhs.base.getValue();
  if (lhsBaseValue != rhsBaseValue &&
      (!lhsBaseValue || !rhsBaseValue ||
       !RValue::areObviouslySameValue(lhsBaseValue, rhsBaseValue))) {
    return;
  }

  auto lhsStorage = component->getAccessedStorage();
  if (!lhsStorage) return;

  auto rhsStorage = rhs.component->getAccessedStorage();
  if (!rhsStorage) return;

  // If the decls match, then this could conflict.
  if (lhsStorage->Storage != rhsStorage->Storage ||
      lhsStorage->IsSuper != rhsStorage->IsSuper)
    return;

  assert((lhsStorage->Indices != nullptr) == (rhsStorage->Indices != nullptr));

  auto storage = lhsStorage->Storage;

  // If the decl is monomorphically a stored property, allow aliases.
  // It could be overridden by a computed property in a subclass, but
  // that's not likely enough to be worth the strictness here.
  auto impl = storage->getImplInfo();
  // TODO: Stored properties with didSet accessors that don't look at the
  // oldValue could also be addressed.
  if ((impl.getReadImpl() == ReadImplKind::Stored ||
       impl.getReadImpl() == ReadImplKind::Address) &&
      (impl.getWriteImpl() == WriteImplKind::Immutable ||
       impl.getWriteImpl() == WriteImplKind::Stored ||
       impl.getWriteImpl() == WriteImplKind::MutableAddress)) {
    return;
  }

  // If the property is a generic requirement, allow aliases, because
  // it may be conformed to using a stored property.
  if (isa<ProtocolDecl>(storage->getDeclContext()))
    return;

  // If this is a simple property access, then we must have a conflict.
  if (!lhsStorage->Indices) {
    assert(isa<VarDecl>(storage));
    SGF.SGM.diagnose(loc, diag::writeback_overlap_property,
                     storage->getBaseName().getIdentifier())
       .highlight(loc.getSourceRange());
    SGF.SGM.diagnose(rhs.loc, diag::writebackoverlap_note)
       .highlight(rhs.loc.getSourceRange());
    return;
  }

  // Otherwise, it is a subscript, check the index values.

  // If the indices are literally identical SILValue's, then there is
  // clearly a conflict.
  if (!lhsStorage->Indices->isObviouslyEqual(*rhsStorage->Indices)) {
    // If the index value doesn't lower to literally the same SILValue's,
    // do some fuzzy matching to catch the common case.
    if (!lhsStorage->IndexExprForDiagnostics ||
        !rhsStorage->IndexExprForDiagnostics ||
        !areCertainlyEqualIndices(lhsStorage->IndexExprForDiagnostics,
                                  rhsStorage->IndexExprForDiagnostics))
      return;
  }

  // The locations for the subscripts are almost certainly SubscriptExprs.
  // If so, dig into them to produce better location info in the
  // diagnostics and be able to do more precise analysis.
  auto expr1 = loc.getAsASTNode<SubscriptExpr>();
  auto expr2 = rhs.loc.getAsASTNode<SubscriptExpr>();

  if (expr1 && expr2) {
    SGF.SGM.diagnose(loc, diag::writeback_overlap_subscript)
       .highlight(expr1->getBase()->getSourceRange());

    SGF.SGM.diagnose(rhs.loc, diag::writebackoverlap_note)
       .highlight(expr2->getBase()->getSourceRange());

  } else {
    SGF.SGM.diagnose(loc, diag::writeback_overlap_subscript)
       .highlight(loc.getSourceRange());
    SGF.SGM.diagnose(rhs.loc, diag::writebackoverlap_note)
       .highlight(rhs.loc.getSourceRange());
  }
}

//===----------------------------------------------------------------------===//

static CanType getSubstFormalRValueType(Expr *expr) {
  return expr->getType()->getRValueType()->getCanonicalType();
}

static LValueTypeData getLogicalStorageTypeData(SILGenModule &SGM,
                                                SGFAccessKind accessKind,
                                                CanType substFormalType) {
  assert(!isa<ReferenceStorageType>(substFormalType));
  AbstractionPattern origFormalType(
      substFormalType.getReferenceStorageReferent());
  return {
    accessKind,
    origFormalType,
    substFormalType,
    SGM.Types.getLoweredType(origFormalType, substFormalType).getObjectType()
  };
}

static LValueTypeData getPhysicalStorageTypeData(SILGenModule &SGM,
                                                 SGFAccessKind accessKind,
                                                 AbstractStorageDecl *storage,
                                                 CanType substFormalType) {
  assert(!isa<ReferenceStorageType>(substFormalType));
  auto origFormalType = SGM.Types.getAbstractionPattern(storage)
                                 .getReferenceStorageReferentType();
  return {
    accessKind,
    origFormalType,
    substFormalType,
    SGM.Types.getLoweredType(origFormalType, substFormalType).getObjectType()
  };
}

static bool shouldUseUnsafeEnforcement(VarDecl *var) {
  if (var->isDebuggerVar())
    return true;

  // TODO: Check for the explicit "unsafe" attribute.
  return false;
}

Optional<SILAccessEnforcement>
SILGenFunction::getStaticEnforcement(VarDecl *var) {
  if (var && shouldUseUnsafeEnforcement(var))
    return SILAccessEnforcement::Unsafe;

  return SILAccessEnforcement::Static;
}

Optional<SILAccessEnforcement>
SILGenFunction::getDynamicEnforcement(VarDecl *var) {
  if (getOptions().EnforceExclusivityDynamic) {
    if (var && shouldUseUnsafeEnforcement(var))
      return SILAccessEnforcement::Unsafe;
    return SILAccessEnforcement::Dynamic;
  }
  return None;
}

Optional<SILAccessEnforcement>
SILGenFunction::getUnknownEnforcement(VarDecl *var) {
  if (var && shouldUseUnsafeEnforcement(var))
    return SILAccessEnforcement::Unsafe;

  return SILAccessEnforcement::Unknown;
}

/// SILGenLValue - An ASTVisitor for building logical lvalues.
class LLVM_LIBRARY_VISIBILITY SILGenLValue
  : public Lowering::ExprVisitor<SILGenLValue, LValue,
                                 SGFAccessKind, LValueOptions>
{

public:
  SILGenFunction &SGF;
  SILGenLValue(SILGenFunction &SGF) : SGF(SGF) {}
  
  LValue visitRec(Expr *e, SGFAccessKind accessKind, LValueOptions options,
                  AbstractionPattern orig = AbstractionPattern::getInvalid());
  
  /// Dummy handler to log unimplemented nodes.
  LValue visitExpr(Expr *e, SGFAccessKind accessKind, LValueOptions options);

  // Nodes that form the root of lvalue paths
  LValue visitDiscardAssignmentExpr(DiscardAssignmentExpr *e,
                                    SGFAccessKind accessKind,
                                    LValueOptions options);
  LValue visitDeclRefExpr(DeclRefExpr *e, SGFAccessKind accessKind,
                          LValueOptions options);
  LValue visitOpaqueValueExpr(OpaqueValueExpr *e, SGFAccessKind accessKind,
                              LValueOptions options);

  // Nodes that make up components of lvalue paths
  
  LValue visitMemberRefExpr(MemberRefExpr *e, SGFAccessKind accessKind,
                            LValueOptions options);
  LValue visitSubscriptExpr(SubscriptExpr *e, SGFAccessKind accessKind,
                            LValueOptions options);
  LValue visitTupleElementExpr(TupleElementExpr *e, SGFAccessKind accessKind,
                               LValueOptions options);
  LValue visitForceValueExpr(ForceValueExpr *e, SGFAccessKind accessKind,
                             LValueOptions options);
  LValue visitBindOptionalExpr(BindOptionalExpr *e, SGFAccessKind accessKind,
                               LValueOptions options);
  LValue visitOpenExistentialExpr(OpenExistentialExpr *e,
                                  SGFAccessKind accessKind,
                                  LValueOptions options);
  LValue visitKeyPathApplicationExpr(KeyPathApplicationExpr *e,
                                     SGFAccessKind accessKind,
                                     LValueOptions options);

  // Expressions that wrap lvalues
  
  LValue visitInOutExpr(InOutExpr *e, SGFAccessKind accessKind,
                        LValueOptions options);
  LValue visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e,
                                       SGFAccessKind accessKind,
                                       LValueOptions options);
};

/// Read this component.
ManagedValue
LogicalPathComponent::projectForRead(SILGenFunction &SGF, SILLocation loc,
                                     ManagedValue base,
                                     SGFAccessKind accessKind) && {
  assert(isReadAccess(accessKind));

  const TypeLowering &RValueTL = SGF.getTypeLowering(getTypeOfRValue());
  TemporaryInitializationPtr tempInit;
  RValue rvalue;

  // If the access doesn't require us to make an owned address, don't
  // force a materialization.
  if (accessKind != SGFAccessKind::OwnedAddressRead &&
      accessKind != SGFAccessKind::BorrowedAddressRead) {
    auto rvalue = std::move(*this).get(SGF, loc, base, SGFContext());
    return std::move(rvalue).getAsSingleValue(SGF, loc);
  }

  // If the RValue type has an openedExistential, then the RValue must be
  // materialized before allocating a temporary for the RValue type. In that
  // case, the RValue cannot be emitted directly into the temporary.
  if (getTypeOfRValue().hasOpenedExistential()) {
    // Emit a 'get'.
    rvalue = std::move(*this).get(SGF, loc, base, SGFContext());

    // Create a temporary, whose type may depend on the 'get'.
    tempInit = SGF.emitFormalAccessTemporary(loc, RValueTL);
  } else {
    // Create a temporary for a static (non-dependent) RValue type.
    tempInit = SGF.emitFormalAccessTemporary(loc, RValueTL);

    // Emit a 'get' directly into the temporary.
    rvalue = std::move(*this).get(SGF, loc, base, SGFContext(tempInit.get()));
  }
  // `this` is now dead.

  // Force `value` into a temporary if is wasn't emitted there.
  if (!rvalue.isInContext())
    std::move(rvalue).forwardInto(SGF, loc, tempInit.get());

  return tempInit->getManagedAddress();
}

ManagedValue LogicalPathComponent::project(SILGenFunction &SGF,
                                           SILLocation loc,
                                           ManagedValue base) && {
  auto accessKind = getAccessKind();
  if (isReadAccess(accessKind))
    return std::move(*this).projectForRead(SGF, loc, base, accessKind);

  // AccessKind is Write or ReadWrite. We need to emit a get and set.
  assert(SGF.InFormalEvaluationScope &&
         "materializing l-value for modification without writeback scope");

  // Clone anything else about the component that we might need in the
  // writeback.
  auto clonedComponent = clone(SGF, loc);

  ManagedValue temp =
    std::move(*this).projectForRead(SGF, loc, base,
                                    SGFAccessKind::OwnedAddressRead);

  if (SGF.getOptions().VerifyExclusivity) {
    // Begin an access of the temporary. It is unenforced because enforcement
    // isn't required for RValues.
    SILValue accessAddress = UnenforcedFormalAccess::enter(
        SGF, loc, temp.getValue(), SILAccessKind::Modify);
    temp = std::move(temp).transform(accessAddress);
  }
  // Push a writeback for the temporary.
  pushWriteback(SGF, loc, std::move(clonedComponent), base,
                MaterializedLValue(temp));
  return temp.unmanagedBorrow();
}

void LogicalPathComponent::writeback(SILGenFunction &SGF, SILLocation loc,
                                     ManagedValue base,
                                     MaterializedLValue materialized,
                                     bool isFinal) {
  assert(!materialized.callback &&
         "unexpected materialized lvalue with callback!");

  // Load the value from the temporary unless the type is address-only
  // and this is the final use, in which case we can just consume the
  // value as-is.
  auto temporary = materialized.temporary;

  assert(temporary.getType().isAddress());
  auto &tempTL = SGF.getTypeLowering(temporary.getType());
  if (!tempTL.isAddressOnly() || !isFinal ||
      !SGF.silConv.useLoweredAddresses()) {
    if (isFinal) temporary.forward(SGF);
    temporary = SGF.emitLoad(loc, temporary.getValue(), tempTL,
                             SGFContext(), IsTake_t(isFinal));
  }
  RValue rvalue(SGF, loc, getSubstFormalType(), temporary);

  // Don't consume cleanups on the base if this isn't final.
  if (base && !isFinal) { base = ManagedValue::forUnmanaged(base.getValue()); }

  // Clone the component if this isn't final.
  std::unique_ptr<LogicalPathComponent> clonedComponent =
    (isFinal ? nullptr : clone(SGF, loc));
  LogicalPathComponent *component = (isFinal ? this : &*clonedComponent);
  std::move(*component).set(SGF, loc, ArgumentSource(loc, std::move(rvalue)),
                            base);
}

InOutConversionScope::InOutConversionScope(SILGenFunction &SGF)
  : SGF(SGF)
{
  assert(SGF.InFormalEvaluationScope
         && "inout conversions should happen in writeback scopes");
  assert(!SGF.InInOutConversionScope
         && "inout conversions should not be nested");
  SGF.InInOutConversionScope = true;
}

InOutConversionScope::~InOutConversionScope() {
  assert(SGF.InInOutConversionScope && "already exited conversion scope?!");
  SGF.InInOutConversionScope = false;
}

void PathComponent::_anchor() {}
void PhysicalPathComponent::_anchor() {}

void PathComponent::dump() const {
  dump(llvm::errs());
}

/// Return the LValueTypeData for a SIL value with the given AST formal type.
static LValueTypeData getValueTypeData(SGFAccessKind accessKind,
                                       CanType formalType, SILValue value) {
  return {
    accessKind,
    AbstractionPattern(formalType),
    formalType,
    value->getType().getObjectType()
  };
}
static LValueTypeData getValueTypeData(SILGenFunction &SGF,
                                       SGFAccessKind accessKind, Expr *e) {
  CanType formalType = getSubstFormalRValueType(e);
  SILType loweredType = SGF.getLoweredType(formalType).getObjectType();

  return {
    accessKind,
    AbstractionPattern(formalType),
    formalType,
    loweredType
  };
}

/// Given the address of an optional value, unsafely project out the
/// address of the value.
static ManagedValue getAddressOfOptionalValue(SILGenFunction &SGF,
                                              SILLocation loc,
                                              ManagedValue optAddr,
                                        const LValueTypeData &valueTypeData) {
  // Project out the 'Some' payload.
  EnumElementDecl *someDecl = SGF.getASTContext().getOptionalSomeDecl();

  // If the base is +1, we want to forward the cleanup.
  bool hadCleanup = optAddr.hasCleanup();

  // UncheckedTakeEnumDataAddr is safe to apply to Optional, because it is
  // a single-payload enum. There will (currently) never be spare bits
  // embedded in the payload.
  SILValue valueAddr =
    SGF.B.createUncheckedTakeEnumDataAddr(loc, optAddr.forward(SGF), someDecl,
                                  valueTypeData.TypeOfRValue.getAddressType());

  // Return the value as +1 if the optional was +1.
  if (hadCleanup) {
    return SGF.emitManagedBufferWithCleanup(valueAddr);
  } else {
    return ManagedValue::forLValue(valueAddr);
  }
}

namespace {
  /// A helper class for creating writebacks associated with l-value
  /// components that don't really need them.
  class WritebackPseudoComponent : public LogicalPathComponent {
  protected:
    WritebackPseudoComponent(const LValueTypeData &typeData)
      : LogicalPathComponent(typeData, WritebackPseudoKind) {}

  public:
    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation l) const override {
      llvm_unreachable("called clone on pseudo-component");
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      llvm_unreachable("called get on a pseudo-component");
    }
    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      llvm_unreachable("called set on a pseudo-component");
    }
    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      llvm_unreachable("called project on a pseudo-component");
    }

    Optional<AccessedStorage> getAccessedStorage() const override {
      return None;
    }
  };

  class EndAccessPseudoComponent : public WritebackPseudoComponent {
  public:
    EndAccessPseudoComponent(const LValueTypeData &typeData)
      : WritebackPseudoComponent(typeData) {}

  private:
    void writeback(SILGenFunction &SGF, SILLocation loc,
                   ManagedValue base,
                   MaterializedLValue materialized,
                   bool isFinal) override {
      loc.markAutoGenerated();

      assert(base.isLValue());
      SGF.B.createEndAccess(loc, base.getValue(), /*abort*/ false);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "EndAccessPseudoComponent\n";
    }
  };
} // end anonymous namespace

static SILValue enterAccessScope(SILGenFunction &SGF, SILLocation loc,
                                 SILValue addr, LValueTypeData typeData,
                                 SGFAccessKind accessKind,
                                 SILAccessEnforcement enforcement) {
  auto silAccessKind = isReadAccess(accessKind) ? SILAccessKind::Read
                                                : SILAccessKind::Modify;

  assert(SGF.InFormalEvaluationScope &&
         "tried to enter access scope without a writeback scope!");

  // Enter the access.
  addr = SGF.B.createBeginAccess(loc, addr, silAccessKind, enforcement,
                                 /*hasNoNestedConflict=*/false,
                                 /*fromBuiltin=*/false);

  // Push a writeback to end it.
  auto accessedMV = ManagedValue::forLValue(addr);
  std::unique_ptr<LogicalPathComponent>
    component(new EndAccessPseudoComponent(typeData));
  pushWriteback(SGF, loc, std::move(component), accessedMV,
                MaterializedLValue());

  return addr;
}

static ManagedValue enterAccessScope(SILGenFunction &SGF, SILLocation loc,
                                     ManagedValue addr, LValueTypeData typeData,
                                     SGFAccessKind accessKind,
                                     SILAccessEnforcement enforcement) {
  return ManagedValue::forLValue(
           enterAccessScope(SGF, loc, addr.getLValueAddress(), typeData,
                            accessKind, enforcement));
}

// Find the base of the formal access at `address`. If the base requires an
// access marker, then create a begin_access on `address`. Return the
// address to be used for the access.
//
// FIXME: In order to generate more consistent and verifiable SIL patterns, or
// subobject projections, create the access on the base address and recreate the
// projection.
SILValue UnenforcedAccess::beginAccess(SILGenFunction &SGF, SILLocation loc,
                                       SILValue address, SILAccessKind kind) {
  if (!SGF.getOptions().VerifyExclusivity)
    return address;

  const AccessedStorage &storage = findAccessedStorage(address);
  // Unsafe access may have invalid storage (e.g. a RawPointer).
  if (storage && !isPossibleFormalAccessBase(storage, &SGF.F))
    return address;

  auto BAI =
    SGF.B.createBeginAccess(loc, address, kind, SILAccessEnforcement::Unsafe,
                            /*hasNoNestedConflict=*/false,
                            /*fromBuiltin=*/false);
  beginAccessPtr = BeginAccessPtr(BAI, DeleterCheck());

  return BAI;
}

void UnenforcedAccess::endAccess(SILGenFunction &SGF) {
  emitEndAccess(SGF);
  beginAccessPtr.release();
}

void UnenforcedAccess::emitEndAccess(SILGenFunction &SGF) {
  if (!beginAccessPtr)
    return;

  SGF.B.createEndAccess(beginAccessPtr->getLoc(), beginAccessPtr.get(),
                        /*abort*/ false);
}

// Emit an end_access marker when executing a cleanup (on a side branch).
void UnenforcedFormalAccess::emitEndAccess(SILGenFunction &SGF) {
  access.emitEndAccess(SGF);
}

// End the access when existing the FormalEvaluationScope.
void UnenforcedFormalAccess::finishImpl(SILGenFunction &SGF) {
  access.endAccess(SGF);
}

namespace {
struct UnenforcedAccessCleanup : Cleanup {
  FormalEvaluationContext::stable_iterator Depth;

  UnenforcedAccessCleanup() : Depth() {}

  void emit(SILGenFunction &SGF, CleanupLocation loc,
            ForUnwind_t forUnwind) override {
    auto &evaluation = *SGF.FormalEvalContext.find(Depth);
    assert(evaluation.getKind() == FormalAccess::Unenforced);
    auto &formalAccess = static_cast<UnenforcedFormalAccess &>(evaluation);
    formalAccess.emitEndAccess(SGF);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "UnenforcedAccessCleanup\n"
                 << "State: " << getState() << " Depth: " << Depth.getDepth()
                 << "\n";
#endif
  }
};
} // end anonymous namespace

SILValue UnenforcedFormalAccess::enter(SILGenFunction &SGF, SILLocation loc,
                                       SILValue address, SILAccessKind kind) {
  assert(SGF.InFormalEvaluationScope);

  UnenforcedAccess access;
  SILValue accessAddress = access.beginAccess(SGF, loc, address, kind);
  if (!access.beginAccessPtr)
    return address;

  auto &cleanup = SGF.Cleanups.pushCleanup<UnenforcedAccessCleanup>();
  CleanupHandle handle = SGF.Cleanups.getTopCleanup();
  auto &context = SGF.FormalEvalContext;
  context.push<UnenforcedFormalAccess>(loc, std::move(access), handle);
  cleanup.Depth = context.stable_begin();

  return accessAddress;
}

static void copyBorrowedYieldsIntoTemporary(SILGenFunction &SGF,
                                            SILLocation loc,
                                            ArrayRef<ManagedValue> &yields,
                                            AbstractionPattern origFormalType,
                                            CanType substFormalType,
                                            Initialization *init) {
  if (!origFormalType.isTuple()) {
    auto value = yields.front();
    yields = yields.drop_front();
    init->copyOrInitValueInto(SGF, loc, value, /*isInit*/ false);
    init->finishInitialization(SGF);
    return;
  }

  assert(init->canSplitIntoTupleElements());
  SmallVector<InitializationPtr, 4> scratch;
  auto eltInits =
    init->splitIntoTupleElements(SGF, loc, substFormalType, scratch);
  for (size_t i : indices(eltInits)) {
    auto origEltType = origFormalType.getTupleElementType(i);
    auto substEltType = cast<TupleType>(substFormalType).getElementType(i);
    copyBorrowedYieldsIntoTemporary(SGF, loc, yields, origEltType,
                                    substEltType, eltInits[i].get());
  }
  init->finishInitialization(SGF);
}

namespace {
  class RefElementComponent : public PhysicalPathComponent {
    VarDecl *Field;
    SILType SubstFieldType;
    bool IsNonAccessing;
  public:
    RefElementComponent(VarDecl *field, LValueOptions options,
                        SILType substFieldType, LValueTypeData typeData)
      : PhysicalPathComponent(typeData, RefElementKind),
        Field(field), SubstFieldType(substFieldType),
        IsNonAccessing(options.IsNonAccessing) {}

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base.getType().isObject() &&
             "base for ref element component must be an object");
      assert(base.getType().hasReferenceSemantics() &&
             "base for ref element component must be a reference type");
      // Borrow the ref element addr using formal access. If we need the ref
      // element addr, we will load it in this expression.
      base = base.formalAccessBorrow(SGF, loc);
      SILValue result =
        SGF.B.createRefElementAddr(loc, base.getUnmanagedValue(),
                                   Field, SubstFieldType);

      // Avoid emitting access markers completely for non-accesses or immutable
      // declarations. Access marker verification is aware of these cases.
      if (!IsNonAccessing && !Field->isLet()) {
        if (auto enforcement = SGF.getDynamicEnforcement(Field)) {
          result = enterAccessScope(SGF, loc, result, getTypeData(),
                                    getAccessKind(), *enforcement);
        }
      }

      return ManagedValue::forLValue(result);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "RefElementComponent(" << Field->getName() << ")\n";
    }
  };

  class TupleElementComponent : public PhysicalPathComponent {
    unsigned ElementIndex;
  public:
    TupleElementComponent(unsigned elementIndex, LValueTypeData typeData)
      : PhysicalPathComponent(typeData, TupleElementKind),
        ElementIndex(elementIndex) {}

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base && "invalid value for element base");
      if (base.getType().isObject()) {
        return SGF.B.createTupleExtract(loc, base, ElementIndex);
      }

      // TODO: if the base is +1, break apart its cleanup.
      auto Res = SGF.B.createTupleElementAddr(loc, base.getValue(),
                                              ElementIndex,
                                              getTypeOfRValue().getAddressType());
      return ManagedValue::forLValue(Res);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "TupleElementComponent(" << ElementIndex << ")\n";
    }
  };

  class StructElementComponent : public PhysicalPathComponent {
    VarDecl *Field;
    SILType SubstFieldType;
  public:
    StructElementComponent(VarDecl *field, SILType substFieldType,
                           LValueTypeData typeData)
      : PhysicalPathComponent(typeData, StructElementKind),
        Field(field), SubstFieldType(substFieldType) {}

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base && "invalid value for element base");
      if (base.getType().isObject()) {
        return SGF.B.createStructExtract(loc, base, Field);
      }

      // TODO: if the base is +1, break apart its cleanup.
      auto Res = SGF.B.createStructElementAddr(loc, base.getValue(),
                                               Field, SubstFieldType);
      return ManagedValue::forLValue(Res);
    }
    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "StructElementComponent("
                        << Field->getName() << ")\n";
    }
  };

  /// A physical path component which force-projects the address of
  /// the value of an optional l-value.
  class ForceOptionalObjectComponent : public PhysicalPathComponent {
    bool isImplicitUnwrap;
  public:
    ForceOptionalObjectComponent(LValueTypeData typeData,
                                 bool isImplicitUnwrap)
      : PhysicalPathComponent(typeData, OptionalObjectKind),
        isImplicitUnwrap(isImplicitUnwrap) {}
    
    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      // Assert that the optional value is present and return the projected out
      // payload.
      return SGF.emitPreconditionOptionalHasValue(loc, base, isImplicitUnwrap);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "ForceOptionalObjectComponent(" << isImplicitUnwrap << ")\n";
    }
  };

  /// A physical path component which projects out an opened archetype
  /// from an existential.
  class OpenOpaqueExistentialComponent : public PhysicalPathComponent {
  public:
    OpenOpaqueExistentialComponent(CanArchetypeType openedArchetype,
                                   LValueTypeData typeData)
      : PhysicalPathComponent(typeData, OpenOpaqueExistentialKind) {
      assert(getSubstFormalType() == openedArchetype);
    }

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(base.getType().isExistentialType() &&
             "base for open existential component must be an existential");
      assert(base.getType().isAddress() &&
             "base value of open-existential component was not an address?");
      SILValue addr;

      auto rep = base.getType().getPreferredExistentialRepresentation(SGF.SGM.M);
      switch (rep) {
      case ExistentialRepresentation::Opaque:
        addr = SGF.B.createOpenExistentialAddr(
          loc, base.getValue(), getTypeOfRValue().getAddressType(),
          getOpenedExistentialAccessFor(getFormalAccessKind(getAccessKind())));
        break;
      case ExistentialRepresentation::Boxed: {
        ManagedValue error;
        if (base.getType().isObject()) {
          error = base;
        } else {
          auto &TL = SGF.getTypeLowering(base.getType());
          error = SGF.emitLoad(loc, base.getValue(), TL,
                               SGFContext(), IsNotTake);
        }
        addr = SGF.B.createOpenExistentialBox(
          loc, error.getValue(), getTypeOfRValue().getAddressType());
        break;
      }
      default:
        llvm_unreachable("Bad existential representation for address-only type");
      }

      return ManagedValue::forLValue(addr);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OpenOpaqueExistentialComponent("
                        << getSubstFormalType() << ")\n";
    }
  };

  /// A local path component for the payload of a class or metatype existential.
  ///
  /// TODO: Could be physical if we had a way to project out the
  /// payload.
  class OpenNonOpaqueExistentialComponent : public LogicalPathComponent {
    CanArchetypeType OpenedArchetype;
  public:
    OpenNonOpaqueExistentialComponent(CanArchetypeType openedArchetype,
                                      LValueTypeData typeData)
      : LogicalPathComponent(typeData, OpenNonOpaqueExistentialKind),
        OpenedArchetype(openedArchetype) {}

    virtual bool isLoadingPure() const override { return true; }

    Optional<AccessedStorage> getAccessedStorage() const override {
      return None;
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      auto refType = base.getType().getObjectType();
      auto &TL = SGF.getTypeLowering(refType);

      // Load the original value.
      auto result = SGF.emitLoad(loc, base.getValue(), TL,
                                 SGFContext(), IsNotTake);

      assert(refType.isAnyExistentialType() &&
             "base for open existential component must be an existential");
      ManagedValue ref;
      if (refType.is<ExistentialMetatypeType>()) {
        assert(refType.getPreferredExistentialRepresentation(SGF.SGM.M)
                 == ExistentialRepresentation::Metatype);
        ref = ManagedValue::forUnmanaged(
                SGF.B.createOpenExistentialMetatype(loc,
                                                    result.getUnmanagedValue(),
                                                    getTypeOfRValue()));
      } else {
        assert(refType.getPreferredExistentialRepresentation(SGF.SGM.M)
                 == ExistentialRepresentation::Class);
        ref = SGF.B.createOpenExistentialRef(loc, result, getTypeOfRValue());
      }

      return RValue(SGF, loc, getSubstFormalType(), ref);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      auto payload = std::move(value).getAsSingleValue(SGF).forward(SGF);

      SmallVector<ProtocolConformanceRef, 2> conformances;
      for (auto proto : OpenedArchetype->getConformsTo())
        conformances.push_back(ProtocolConformanceRef(proto));

      SILValue ref;
      if (base.getType().is<ExistentialMetatypeType>()) {
        ref = SGF.B.createInitExistentialMetatype(
                loc,
                payload,
                base.getType().getObjectType(),
                SGF.getASTContext().AllocateCopy(conformances));
      } else {
        assert(getSubstFormalType()->isBridgeableObjectType());
        ref = SGF.B.createInitExistentialRef(
                loc,
                base.getType().getObjectType(),
                getSubstFormalType(),
                payload,
                SGF.getASTContext().AllocateCopy(conformances));
      }

      auto &TL = SGF.getTypeLowering(base.getType());
      SGF.emitSemanticStore(loc, ref,
                            base.getValue(), TL, IsNotInitialization);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone =
        new OpenNonOpaqueExistentialComponent(OpenedArchetype, getTypeData());
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OpenNonOpaqueExistentialComponent("
                        << OpenedArchetype << ", ...)\n";
    }
  };

  /// A physical path component which returns a literal address.
  class ValueComponent : public PhysicalPathComponent {
    ManagedValue Value;
    Optional<SILAccessEnforcement> Enforcement;
    bool IsRValue;
  public:
    ValueComponent(ManagedValue value,
                   Optional<SILAccessEnforcement> enforcement,
                   LValueTypeData typeData,
                   bool isRValue = false) :
      PhysicalPathComponent(typeData, ValueKind),
      Value(value),
      Enforcement(enforcement),
      IsRValue(isRValue) {
        assert(IsRValue || value.getType().isAddress());
    }

    virtual bool isLoadingPure() const override { return true; }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(!base && "value component must be root of lvalue path");

      if (!Enforcement)
        return Value;

      SILValue addr = Value.getLValueAddress();
      addr = enterAccessScope(SGF, loc, addr, getTypeData(),
                              getAccessKind(), *Enforcement);

      return ManagedValue::forLValue(addr);
    }

    bool isRValue() const override {
      return IsRValue;
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS << "ValueComponent(";
      if (IsRValue) OS << "rvalue, ";
      if (Enforcement) {
        OS << getSILAccessEnforcementName(*Enforcement);
      } else {
        OS << "unenforced";
      }
      OS << "):\n";
      Value.dump(OS, indent + 2);
    }
  };
} // end anonymous namespace

static bool isReadNoneFunction(const Expr *e) {
  // If this is a curried call to an integer literal conversion operations, then
  // we can "safely" assume it is readnone (btw, yes this is totally gross).
  // This is better to be attribute driven, a la rdar://15587352.
  if (auto *dre = dyn_cast<DeclRefExpr>(e)) {
    DeclName name = dre->getDecl()->getFullName();
    return (name.getArgumentNames().size() == 1 &&
            name.getBaseName() == DeclBaseName::createConstructor() &&
            !name.getArgumentNames()[0].empty() &&
            (name.getArgumentNames()[0].str() == "integerLiteral" ||
             name.getArgumentNames()[0].str() == "_builtinIntegerLiteral"));
  }
  
  // Look through DotSyntaxCallExpr, since the literal functions are curried.
  if (auto *CRCE = dyn_cast<ConstructorRefCallExpr>(e))
    return isReadNoneFunction(CRCE->getFn());
  
  return false;
}


/// Given two expressions used as indexes to the same SubscriptDecl (and thus
/// are guaranteed to have the same AST type) check to see if they are going to
/// produce the same value.
static bool areCertainlyEqualIndices(const Expr *e1, const Expr *e2) {
  if (e1->getKind() != e2->getKind()) return false;
  
  // Look through ParenExpr's.
  if (auto *pe1 = dyn_cast<ParenExpr>(e1)) {
    auto *pe2 = cast<ParenExpr>(e2);
    return areCertainlyEqualIndices(pe1->getSubExpr(), pe2->getSubExpr());
  }
  
  // Calls are identical if the callee and operands are identical and we know
  // that the call is something that is "readnone".
  if (auto *ae1 = dyn_cast<ApplyExpr>(e1)) {
    auto *ae2 = cast<ApplyExpr>(e2);
    return areCertainlyEqualIndices(ae1->getFn(), ae2->getFn()) &&
           areCertainlyEqualIndices(ae1->getArg(), ae2->getArg()) &&
           isReadNoneFunction(ae1->getFn());
  }
  
  // TypeExpr's that produce the same metatype type are identical.
  if (isa<TypeExpr>(e1))
    return true;
  
  if (auto *dre1 = dyn_cast<DeclRefExpr>(e1)) {
    auto *dre2 = cast<DeclRefExpr>(e2);
    return dre1->getDecl() == dre2->getDecl() &&
           dre1->getType()->isEqual(dre2->getType());
  }

  // Compare a variety of literals.
  if (auto *il1 = dyn_cast<IntegerLiteralExpr>(e1))
    return il1->getValue() == cast<IntegerLiteralExpr>(e2)->getValue();
  if (auto *il1 = dyn_cast<FloatLiteralExpr>(e1))
    return il1->getValue().bitwiseIsEqual(
                                        cast<FloatLiteralExpr>(e2)->getValue());
  if (auto *bl1 = dyn_cast<BooleanLiteralExpr>(e1))
    return bl1->getValue() == cast<BooleanLiteralExpr>(e2)->getValue();
  if (auto *sl1 = dyn_cast<StringLiteralExpr>(e1))
    return sl1->getValue() == cast<StringLiteralExpr>(e2)->getValue();
  
  // Compare tuple expressions.
  if (auto *te1 = dyn_cast<TupleExpr>(e1)) {
    auto *te2 = cast<TupleExpr>(e2);

    // Easy checks: # of elements, trailing closures, element names.
    if (te1->getNumElements() != te2->getNumElements() ||
        te1->hasTrailingClosure() != te2->hasTrailingClosure() ||
        te1->getElementNames() != te2->getElementNames()) {
      return false;
    }

    for (unsigned i = 0, n = te1->getNumElements(); i != n; ++i) {
      if (!areCertainlyEqualIndices(te1->getElement(i), te2->getElement(i)))
        return false;
    }

    return true;
  }

  // Otherwise, we have no idea if they are identical.
  return false;
}

static LValueOptions getBaseOptions(LValueOptions options,
                                    AccessStrategy strategy) {
  return (strategy.getKind() == AccessStrategy::Storage
            ? options.forProjectedBaseLValue()
            : options.forComputedBaseLValue());
}

static ArgumentSource emitBaseValueForAccessor(SILGenFunction &SGF,
                                               SILLocation loc, LValue &&dest,
                                               CanType baseFormalType,
                                               SILDeclRef accessor);

static SGFAccessKind getBaseAccessKind(SILGenModule &SGM,
                                       AbstractStorageDecl *member,
                                       SGFAccessKind accessKind,
                                       AccessStrategy strategy,
                                       CanType baseFormalType);

namespace {
  /// A helper class for implementing components that involve accessing
  /// storage.
  template <class Base>
  class AccessComponent : public Base {
  protected:
    // The VarDecl or SubscriptDecl being get/set.
    AbstractStorageDecl *Storage;

    /// The subscript index expression.  Useless
    Expr *IndexExprForDiagnostics;
    PreparedArguments Indices;

    /// AST type of the base expression, in case the accessor call
    /// requires re-abstraction.
    CanType BaseFormalType;

    struct AccessorArgs {
      ArgumentSource base;
      PreparedArguments Indices;
    };

    /// Returns a tuple of RValues holding the accessor value, base (retained if
    /// necessary), and subscript arguments, in that order.
    AccessorArgs
    prepareAccessorArgs(SILGenFunction &SGF, SILLocation loc,
                        ManagedValue base, SILDeclRef accessor) &&
    {
      AccessorArgs result;
      if (base)
        result.base = SGF.prepareAccessorBaseArg(loc, base, BaseFormalType,
                                                 accessor);

      if (!Indices.isNull())
        result.Indices = std::move(Indices);
      
      return result;
    }

    AccessComponent(PathComponent::KindTy kind,
                    AbstractStorageDecl *storage,
                    CanType baseFormalType,
                    LValueTypeData typeData,
                    Expr *indexExprForDiagnostics,
                    PreparedArguments &&indices)
      : Base(typeData, kind), Storage(storage),
        IndexExprForDiagnostics(indexExprForDiagnostics),
        Indices(std::move(indices)),
        BaseFormalType(baseFormalType)
    {
    }

    AccessComponent(const AccessComponent &copied,
                    SILGenFunction &SGF,
                    SILLocation loc)
      : Base(copied.getTypeData(), copied.getKind()),
        Storage(copied.Storage),
        IndexExprForDiagnostics(copied.IndexExprForDiagnostics),
        Indices(copied.Indices.copy(SGF, loc)) ,
        BaseFormalType(copied.BaseFormalType) {}

    bool doesAccessorMutateSelf(SILGenFunction &SGF,
                                SILDeclRef accessor) const {
      auto accessorSelf = SGF.SGM.Types.getConstantSelfParameter(accessor);
      return accessorSelf.getType() && accessorSelf.isIndirectMutating();
    }
    
    void printBase(raw_ostream &OS, unsigned indent, StringRef name) const {
      OS.indent(indent) << name << "(" << Storage->getBaseName() << ")";
      if (IndexExprForDiagnostics) {
        OS << " subscript_index:\n";
        IndexExprForDiagnostics->print(OS, 2);
      }
      OS << '\n';
    }
  };

  /// A helper class for implementing a component that involves
  /// calling accessors.
  template <class Base>
  class AccessorBasedComponent : public AccessComponent<Base> {
    using super = AccessComponent<Base>;

  protected:
    SILDeclRef Accessor;
    bool IsSuper;
    bool IsDirectAccessorUse;
    SubstitutionMap Substitutions;

  public:
    AccessorBasedComponent(PathComponent::KindTy kind,
                           AbstractStorageDecl *decl,
                           SILDeclRef accessor,
                           bool isSuper, bool isDirectAccessorUse,
                           SubstitutionMap substitutions,
                           CanType baseFormalType,
                           LValueTypeData typeData,
                           Expr *indexExprForDiagnostics,
                           PreparedArguments &&indices)
      : super(kind, decl, baseFormalType, typeData,
              indexExprForDiagnostics, std::move(indices)),
        Accessor(accessor), IsSuper(isSuper),
        IsDirectAccessorUse(isDirectAccessorUse),
        Substitutions(substitutions) {}

    AccessorBasedComponent(const AccessorBasedComponent &copied,
                           SILGenFunction &SGF,
                           SILLocation loc)
      : super(copied, SGF, loc),
        Accessor(copied.Accessor),
        IsSuper(copied.IsSuper),
        IsDirectAccessorUse(copied.IsDirectAccessorUse),
        Substitutions(copied.Substitutions) {}

    AccessorDecl *getAccessorDecl() const {
      return cast<AccessorDecl>(Accessor.getFuncDecl());
    }
  };

  class GetterSetterComponent
    : public AccessorBasedComponent<LogicalPathComponent> {
  public:

     GetterSetterComponent(AbstractStorageDecl *decl,
                           SILDeclRef accessor,
                           bool isSuper, bool isDirectAccessorUse,
                           SubstitutionMap substitutions,
                           CanType baseFormalType,
                           LValueTypeData typeData,
                           Expr *subscriptIndexExpr,
                           PreparedArguments &&indices)
      : AccessorBasedComponent(GetterSetterKind, decl, accessor, isSuper,
                               isDirectAccessorUse, substitutions,
                               baseFormalType, typeData, subscriptIndexExpr,
                               std::move(indices))
    {
      assert(getAccessorDecl()->isGetterOrSetter());
    }
    
    GetterSetterComponent(const GetterSetterComponent &copied,
                          SILGenFunction &SGF,
                          SILLocation loc)
      : AccessorBasedComponent(copied, SGF, loc)
    {
    }

    void emitAssignWithSetter(SILGenFunction &SGF, SILLocation loc,
                              LValue &&dest, ArgumentSource &&value) {
      assert(getAccessorDecl()->isSetter());
      SILDeclRef setter = Accessor;

      // Pull everything out of this that we'll need, because we're
      // about to modify the LValue and delete this component.
      auto subs = this->Substitutions;
      bool isSuper = this->IsSuper;
      bool isDirectAccessorUse = this->IsDirectAccessorUse;
      auto indices = std::move(this->Indices);
      auto baseFormalType = this->BaseFormalType;

      // Drop this component from the l-value.
      dest.dropLastComponent(*this);

      return emitAssignWithSetter(SGF, loc, std::move(dest), baseFormalType,
                                  isSuper, setter, isDirectAccessorUse,
                                  subs, std::move(indices), std::move(value));
    }

    static void emitAssignWithSetter(SILGenFunction &SGF, SILLocation loc,
                                     LValue &&baseLV, CanType baseFormalType,
                                     bool isSuper, SILDeclRef setter,
                                     bool isDirectAccessorUse,
                                     SubstitutionMap subs,
                                     PreparedArguments &&indices,
                                     ArgumentSource &&value) {
      ArgumentSource self = [&] {
        if (!baseLV.isValid()) {
          return ArgumentSource();
        } else if (computeSelfParam(cast<FuncDecl>(setter.getDecl()))
                     .getParameterFlags().isInOut()) {
          return ArgumentSource(loc, std::move(baseLV));
        } else {
          return emitBaseValueForAccessor(SGF, loc, std::move(baseLV),
                                          baseFormalType, setter);
        }
      }();

      return SGF.emitSetAccessor(loc, setter, subs, std::move(self),
                                 isSuper, isDirectAccessorUse,
                                 std::move(indices), std::move(value));
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      assert(getAccessorDecl()->isSetter());
      SILDeclRef setter = Accessor;

      FormalEvaluationScope scope(SGF);
      // Pass in just the setter.
      auto args =
        std::move(*this).prepareAccessorArgs(SGF, loc, base, setter);

      return SGF.emitSetAccessor(loc, setter, Substitutions,
                                 std::move(args.base), IsSuper,
                                 IsDirectAccessorUse,
                                 std::move(args.Indices),
                                 std::move(value));
    }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(isReadAccess(getAccessKind()) &&
             "shouldn't be using this path to call modify");
      return std::move(*this).LogicalPathComponent::project(SGF, loc, base);
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      assert(getAccessorDecl()->isGetter());
      SILDeclRef getter = Accessor;

      FormalEvaluationScope scope(SGF);

      auto args =
        std::move(*this).prepareAccessorArgs(SGF, loc, base, getter);
      
      return SGF.emitGetAccessor(loc, getter, Substitutions,
                                 std::move(args.base), IsSuper,
                                 IsDirectAccessorUse,
                                 std::move(args.Indices), c);
    }
    
    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone = new GetterSetterComponent(*this, SGF, loc);
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      printBase(OS, indent, "GetterSetterComponent");
    }

    /// Compare 'this' lvalue and the 'rhs' lvalue (which is guaranteed to have
    /// the same dynamic PathComponent type as the receiver) to see if they are
    /// identical.  If so, there is a conflicting writeback happening, so emit a
    /// diagnostic.
    Optional<AccessedStorage> getAccessedStorage() const override {
      return AccessedStorage{Storage, IsSuper,
                             Indices.isNull() ? nullptr : &Indices,
                             IndexExprForDiagnostics };
    }
  };

  class MaterializeToTemporaryComponent final
      : public AccessComponent<LogicalPathComponent> {
    SubstitutionMap Substitutions;
    AccessStrategy ReadStrategy;
    AccessStrategy WriteStrategy;
    LValueOptions Options;
    bool IsSuper;

  public:
    MaterializeToTemporaryComponent(AbstractStorageDecl *storage,
                                    bool isSuper, SubstitutionMap subs,
                                    LValueOptions options,
                                    AccessStrategy readStrategy,
                                    AccessStrategy writeStrategy,
                                    CanType baseFormalType,
                                    LValueTypeData typeData,
                                    Expr *indexExprForDiagnostics,
                                    PreparedArguments &&indices)
      : AccessComponent(MaterializeToTemporaryKind, storage, baseFormalType,
                        typeData, indexExprForDiagnostics, std::move(indices)),
        Substitutions(subs),
        ReadStrategy(readStrategy), WriteStrategy(writeStrategy),
        Options(options), IsSuper(isSuper) {}


    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      PreparedArguments clonedIndices = Indices.copy(SGF, loc);

      LogicalPathComponent *clone =
        new MaterializeToTemporaryComponent(Storage, IsSuper, Substitutions,
                                            Options,
                                            ReadStrategy, WriteStrategy,
                                            BaseFormalType, getTypeData(),
                                            IndexExprForDiagnostics,
                                            std::move(clonedIndices));
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext C) && override {
      LValue lv = std::move(*this).prepareLValue(SGF, loc, base,
                                                 SGFAccessKind::OwnedObjectRead,
                                                 ReadStrategy);
      return SGF.emitLoadOfLValue(loc, std::move(lv), C);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&value, ManagedValue base) && override {
      LValue lv = std::move(*this).prepareLValue(SGF, loc, base,
                                                 SGFAccessKind::Write,
                                                 WriteStrategy);
      return SGF.emitAssignToLValue(loc, std::move(value), std::move(lv));
    }

    Optional<AccessedStorage> getAccessedStorage() const override {
      return AccessedStorage{Storage, IsSuper,
                             Indices.isNull() ? nullptr : &Indices,
                             IndexExprForDiagnostics};
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "MaterializeToTemporaryComponent";
    }

  private:
    LValue prepareLValue(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base, SGFAccessKind accessKind,
                         AccessStrategy strategy) && {
      LValue lv = [&] {
        if (!base) return LValue();
        auto baseAccessKind =
          getBaseAccessKind(SGF.SGM, Storage, accessKind, strategy,
                            BaseFormalType);
        return LValue::forValue(baseAccessKind, base, BaseFormalType);
      }();

      if (auto subscript = dyn_cast<SubscriptDecl>(Storage)) {
        lv.addMemberSubscriptComponent(SGF, loc, subscript, Substitutions,
                                       Options, IsSuper, accessKind, strategy,
                                       getSubstFormalType(),
                                       std::move(Indices),
                                       IndexExprForDiagnostics);
      } else {
        auto var = cast<VarDecl>(Storage);
        if (base) {
          lv.addMemberVarComponent(SGF, loc, var, Substitutions, Options,
                                   IsSuper, accessKind, strategy,
                                   getSubstFormalType());
        } else {
          lv.addNonMemberVarComponent(SGF, loc, var, Substitutions, Options,
                                      accessKind, strategy,
                                      getSubstFormalType());
        }
      }

      return lv;
    }
  };

  /// A physical component which involves calling addressors.
  class AddressorComponent
      : public AccessorBasedComponent<PhysicalPathComponent> {
    SILType SubstFieldType;
  public:
     AddressorComponent(AbstractStorageDecl *decl, SILDeclRef accessor,
                        bool isSuper, bool isDirectAccessorUse,
                        SubstitutionMap substitutions,
                        CanType baseFormalType, LValueTypeData typeData,
                        SILType substFieldType,
                        Expr *indexExprForDiagnostics,
                        PreparedArguments &&indices)
      : AccessorBasedComponent(AddressorKind, decl, accessor, isSuper,
                               isDirectAccessorUse, substitutions,
                               baseFormalType, typeData,
                               indexExprForDiagnostics, std::move(indices)),
        SubstFieldType(substFieldType)
    {
      assert(getAccessorDecl()->isAnyAddressor());
    }

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(SGF.InFormalEvaluationScope &&
             "offsetting l-value for modification without writeback scope");

      std::pair<ManagedValue, ManagedValue> result;
      {
        FormalEvaluationScope scope(SGF);

        auto args =
            std::move(*this).prepareAccessorArgs(SGF, loc, base, Accessor);
        result = SGF.emitAddressorAccessor(
            loc, Accessor, Substitutions, std::move(args.base),
            IsSuper, IsDirectAccessorUse,
            std::move(args.Indices), SubstFieldType);
      }

      switch (getAccessorDecl()->getAddressorKind()) {
      case AddressorKind::NotAddressor:
        llvm_unreachable("not an addressor!");

      // For unsafe addressors, we have no owner pointer to manage.
      case AddressorKind::Unsafe:
        assert(!result.second);
        break;

      // For owning addressors, we can just let the owner get released
      // at an appropriate point.
      case AddressorKind::Owning:
      case AddressorKind::NativeOwning:
        break;
      }

      // Enter an unsafe access scope for the access.
      auto addr = result.first;
      addr = enterAccessScope(SGF, loc, addr, getTypeData(), getAccessKind(),
                              SILAccessEnforcement::Unsafe);

      return addr;
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      printBase(OS, indent, "AddressorComponent");
    }
  };

  class EndApplyPseudoComponent : public WritebackPseudoComponent {
    CleanupHandle EndApplyHandle;
    AbstractStorageDecl *Storage;
    bool IsSuper;
    PreparedArguments PeekedIndices;
    Expr *IndexExprForDiagnostics;
  public:
    EndApplyPseudoComponent(const LValueTypeData &typeData,
                            CleanupHandle endApplyHandle,
                            AbstractStorageDecl *storage,
                            bool isSuper,
                            PreparedArguments &&peekedIndices,
                            Expr *indexExprForDiagnostics)
      : WritebackPseudoComponent(typeData),
        EndApplyHandle(endApplyHandle),
        Storage(storage), IsSuper(isSuper),
        PeekedIndices(std::move(peekedIndices)),
        IndexExprForDiagnostics(indexExprForDiagnostics) {}

  private:
    void writeback(SILGenFunction &SGF, SILLocation loc,
                   ManagedValue base,
                   MaterializedLValue materialized,
                   bool isFinal) override {
      // Just let the cleanup get emitted normally if the writeback is for
      // an unwind.
      if (!isFinal) return;

      SGF.Cleanups.popAndEmitCleanup(EndApplyHandle, CleanupLocation::get(loc),
                                     NotForUnwind);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "EndApplyPseudoComponent";
    }

    Optional<AccessedStorage> getAccessedStorage() const override {
      return AccessedStorage{Storage, IsSuper,
                             PeekedIndices.isNull() ? nullptr : &PeekedIndices,
                             IndexExprForDiagnostics};
    }
  };

  /// A physical component which involves calling coroutine accessors.
  class CoroutineAccessorComponent
      : public AccessorBasedComponent<PhysicalPathComponent> {
  public:
    CoroutineAccessorComponent(AbstractStorageDecl *decl,
                               SILDeclRef accessor,
                               bool isSuper, bool isDirectAccessorUse,
                               SubstitutionMap substitutions,
                               CanType baseFormalType,
                               LValueTypeData typeData,
                               Expr *indexExprForDiagnostics,
                               PreparedArguments &&indices)
      : AccessorBasedComponent(CoroutineAccessorKind,
                               decl, accessor, isSuper, isDirectAccessorUse,
                               substitutions, baseFormalType, typeData,
                               indexExprForDiagnostics, std::move(indices)) {
    }

    using AccessorBasedComponent::AccessorBasedComponent;

    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(SGF.InFormalEvaluationScope &&
             "offsetting l-value for modification without writeback scope");

      ManagedValue result;

      auto args =
        std::move(*this).prepareAccessorArgs(SGF, loc, base, Accessor);
      auto peekedIndices = args.Indices.copyForDiagnostics();
      SmallVector<ManagedValue, 4> yields;
      auto endApplyHandle =
        SGF.emitCoroutineAccessor(
          loc, Accessor, Substitutions, std::move(args.base), IsSuper,
          IsDirectAccessorUse, std::move(args.Indices), yields);

      // Push a writeback that ends the access.
      std::unique_ptr<LogicalPathComponent>
        component(new EndApplyPseudoComponent(getTypeData(), endApplyHandle,
                                              Storage, IsSuper,
                                              std::move(peekedIndices),
                                              IndexExprForDiagnostics));
      pushWriteback(SGF, loc, std::move(component), /*for diagnostics*/ base,
                    MaterializedLValue());

      auto decl = cast<AccessorDecl>(Accessor.getFuncDecl());

      // 'modify' always returns an address of the right type.
      if (decl->getAccessorKind() == AccessorKind::Modify) {
        assert(yields.size() == 1);
        return yields[0];
      }

      // 'read' returns a borrowed r-value, which might or might not be
      // an address of the right type.

      // Use the address if we have one.
      if (!getOrigFormalType().isTuple()) {
        assert(yields.size() == 1);
        if (yields[0].isLValue()) {
          return yields[0];
        }
      }

      // Otherwise, we need to make a temporary.
      auto temporary =
        SGF.emitTemporary(loc, SGF.getTypeLowering(getTypeOfRValue()));
      auto yieldsAsArray = llvm::makeArrayRef(yields);
      copyBorrowedYieldsIntoTemporary(SGF, loc, yieldsAsArray,
                                      getOrigFormalType(), getSubstFormalType(),
                                      temporary.get());
      assert(yieldsAsArray.empty() && "didn't claim all yields");
      return temporary->getManagedAddress();
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      printBase(OS, indent, "CoroutineAccessorComponent");
    }
  };
  
  /// A physical component which involves applying a key path.
  class KeyPathApplicationComponent final : public PhysicalPathComponent {
    ArgumentSource KeyPath;
  public:
    KeyPathApplicationComponent(LValueTypeData typeData,
                                ArgumentSource &&KeyPath)
      : PhysicalPathComponent(typeData, KeyPathApplicationKind),
        KeyPath(std::move(KeyPath))
    {}
  
    // An rvalue base object is passed indirectly +1 so needs to be
    // materialized if the base we have is +0 or a loaded value.
    void makeBaseConsumableMaterializedRValue(SILGenFunction &SGF,
                                              SILLocation loc,
                                              ManagedValue &base) {
      if (base.isLValue()) {
        auto tmp = SGF.emitTemporaryAllocation(loc, base.getType());
        SGF.B.createCopyAddr(loc, base.getValue(), tmp,
                             IsNotTake, IsInitialization);
        base = SGF.emitManagedBufferWithCleanup(tmp);
        return;
      }

      bool isBorrowed = base.isPlusZeroRValueOrTrivial()
        && !base.getType().isTrivial(SGF.SGM.M);
      if (!base.getType().isAddress() || isBorrowed) {
        auto tmp = SGF.emitTemporaryAllocation(loc, base.getType());
        if (isBorrowed)
          base.copyInto(SGF, tmp, loc);
        else
          base.forwardInto(SGF, loc, tmp);
        base = SGF.emitManagedBufferWithCleanup(tmp);
      }
    }
  
    ManagedValue mutableOffset(
                   SILGenFunction &SGF, SILLocation loc, ManagedValue base) && {
      auto &C = SGF.getASTContext();
      auto keyPathTy = KeyPath.getSubstRValueType()->castTo<BoundGenericType>();

      FuncDecl *projectionFunction;
      if (keyPathTy->getDecl() == C.getWritableKeyPathDecl()) {
        // Turn the base lvalue into a pointer to pass to the projection
        // function.
        // This is OK since the materialized base is exclusive-borrowed for the
        // duration of the access.
        auto baseRawPtr = SGF.B.createAddressToPointer(loc,
                              base.getValue(),
                              SILType::getRawPointerType(SGF.getASTContext()));
        auto basePtrTy = BoundGenericType::get(C.getUnsafeMutablePointerDecl(),
                                               nullptr,
                                               keyPathTy->getGenericArgs()[0])
          ->getCanonicalType();
        auto basePtr = SGF.B.createStruct(loc,
                                    SILType::getPrimitiveObjectType(basePtrTy),
                                    SILValue(baseRawPtr));
        base = ManagedValue::forUnmanaged(basePtr);
        projectionFunction = C.getProjectKeyPathWritable(nullptr);
      } else if (keyPathTy->getDecl() == C.getReferenceWritableKeyPathDecl()) {
        projectionFunction = C.getProjectKeyPathReferenceWritable(nullptr);
        makeBaseConsumableMaterializedRValue(SGF, loc, base);
      } else {
        llvm_unreachable("not a writable key path type?!");
      }
      
      auto projectionGenericSig = projectionFunction->getGenericSignature();
      auto subMap = SubstitutionMap::get(
          projectionGenericSig,
          [&](SubstitutableType *type) -> Type {
            auto genericParam = cast<GenericTypeParamType>(type);
            auto index =
                projectionGenericSig->getGenericParamOrdinal(genericParam);
            return keyPathTy->getGenericArgs()[index];
          },
          LookUpConformanceInSignature(*projectionGenericSig));

      // The projection function behaves like an owning addressor, returning
      // a pointer to the projected value and an owner reference that keeps
      // it alive.
      auto keyPathValue = std::move(KeyPath).getAsSingleValue(SGF);
      auto resultTuple = SGF.emitApplyOfLibraryIntrinsic(loc,
                                                         projectionFunction,
                                                         subMap,
                                                         {base, keyPathValue},
                                                         SGFContext());
      SmallVector<ManagedValue, 2> members;
      std::move(resultTuple).getAll(members);
      auto projectedPtr = members[0];
      auto projectedOwner = members[1];

      // Pass along the projected pointer.
      auto rawValueField = *C.getUnsafeMutablePointerDecl()
        ->getStoredProperties().begin();
      auto projectedRawPtr = SGF.B.createStructExtract(loc,
                                               projectedPtr.getUnmanagedValue(),
                                               rawValueField,
                                               SILType::getRawPointerType(C));
      SILValue projectedAddr = SGF.B.createPointerToAddress(loc,
                                            projectedRawPtr,
                                            getTypeOfRValue().getAddressType(),
                                            /*strict*/ true);
      // Mark the projected address's dependence on the owner.
      projectedAddr = SGF.B.createMarkDependence(loc, projectedAddr,
                                                 projectedOwner.getValue());
      
      // Push a cleanup to destroy the owner object. We don't want to leave
      // it to release whenever because the key path implementation hangs
      // the writeback operations specific to the traversal onto the destruction
      // of the owner.
      struct DestroyOwnerPseudoComponent : WritebackPseudoComponent {
        ManagedValue owner;
        
        DestroyOwnerPseudoComponent(const LValueTypeData &typeData,
                                    ManagedValue owner)
          : WritebackPseudoComponent(typeData), owner(owner)
        {}
        
        void writeback(SILGenFunction &SGF, SILLocation loc,
                       ManagedValue base,
                       MaterializedLValue materialized,
                       bool isFinal) override {
          // Just let the cleanup get emitted normally if the writeback is for
          // an unwind.
          if (!isFinal) return;

          SGF.Cleanups.popAndEmitCleanup(owner.getCleanup(),
                                         CleanupLocation::get(loc),
                                         NotForUnwind);
        }
        
        void dump(raw_ostream &OS, unsigned indent) const override {
          OS << "DestroyOwnerPseudoComponent";
        }
      };
      
      std::unique_ptr<LogicalPathComponent> component(
        new DestroyOwnerPseudoComponent(getTypeData(), projectedOwner));
      
      pushWriteback(SGF, loc, std::move(component), base, MaterializedLValue());

      return ManagedValue::forLValue(projectedAddr);
    }

    ManagedValue readOnlyOffset(
                   SILGenFunction &SGF, SILLocation loc, ManagedValue base) && {
      auto &C = SGF.getASTContext();
      
      makeBaseConsumableMaterializedRValue(SGF, loc, base);
      auto keyPathValue = std::move(KeyPath).getAsSingleValue(SGF);
      // Upcast the key path operand to the base KeyPath type, as expected by
      // the read-only projection function.
      BoundGenericType *keyPathTy
        = keyPathValue.getType().castTo<BoundGenericType>();
      if (keyPathTy->getDecl() != C.getKeyPathDecl()) {
        keyPathTy = BoundGenericType::get(C.getKeyPathDecl(), Type(),
                                          keyPathTy->getGenericArgs());
        keyPathValue = SGF.B.createUpcast(loc, keyPathValue,
                SILType::getPrimitiveObjectType(keyPathTy->getCanonicalType()));
      }
      
      auto projectFn = C.getProjectKeyPathReadOnly(nullptr);

      auto projectionGenericSig = projectFn->getGenericSignature();
      auto subMap = SubstitutionMap::get(
          projectionGenericSig,
          [&](SubstitutableType *type) -> Type {
            auto genericParam = cast<GenericTypeParamType>(type);
            auto index =
                projectionGenericSig->getGenericParamOrdinal(genericParam);
            return keyPathTy->getGenericArgs()[index];
          },
          LookUpConformanceInSignature(*projectionGenericSig));

      // Allocate a temporary to own the projected value.
      auto &resultTL = SGF.getTypeLowering(keyPathTy->getGenericArgs()[1]);
      auto resultInit = SGF.emitTemporary(loc, resultTL);

      auto result = SGF.emitApplyOfLibraryIntrinsic(loc, projectFn,
        subMap, {base, keyPathValue}, SGFContext(resultInit.get()));
      if (!result.isInContext())
        std::move(result).forwardInto(SGF, loc, resultInit.get());
      
      // Result should be a temporary we own. Return its address as an lvalue.
      return ManagedValue::forLValue(resultInit->getAddress());
    }
  
    ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base) && override {
      assert(SGF.InFormalEvaluationScope &&
             "offsetting l-value for modification without writeback scope");
      if (isReadAccess(getAccessKind())) {
        // For a read-only access, project the key path as if immutable,
        // so that we don't trigger captured writebacks or observers or other
        // operations that might be enqueued by a mutable projection.
        return std::move(*this).readOnlyOffset(SGF, loc, base);
      } else {
        return std::move(*this).mutableOffset(SGF, loc, base);
      }
    }
    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "KeyPathApplicationComponent\n";
    }
  };
} // end anonymous namespace

RValue
TranslationPathComponent::get(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue base, SGFContext c) && {
  // Load the original value.
  RValue baseVal(SGF, loc, getSubstFormalType(),
           SGF.emitLoad(loc, base.getValue(),
                        SGF.getTypeLowering(base.getType()),
                        SGFContext(), IsNotTake));

  // Map the base value to its substituted representation.
  return std::move(*this).translate(SGF, loc, std::move(baseVal), c);
}

void TranslationPathComponent::set(SILGenFunction &SGF, SILLocation loc,
                                   ArgumentSource &&valueSource,
                                   ManagedValue base) && {
  RValue value = std::move(valueSource).getAsRValue(SGF);

  // Map the value to the original pattern.
  RValue newValue = std::move(*this).untranslate(SGF, loc, std::move(value));

  // Store to the base.
  std::move(newValue).assignInto(SGF, loc, base.getValue());
}

namespace {
  /// Remap an lvalue referencing a generic type to an lvalue of its
  /// substituted type in a concrete context.
  class OrigToSubstComponent : public TranslationPathComponent {
    AbstractionPattern OrigType;

  public:
    OrigToSubstComponent(const LValueTypeData &typeData,
                         AbstractionPattern origType)
      : TranslationPathComponent(typeData, OrigToSubstKind),
        OrigType(origType)
    {}

    virtual bool isLoadingPure() const override { return true; }

    RValue untranslate(SILGenFunction &SGF, SILLocation loc,
                       RValue &&rv, SGFContext c) && override {
      return SGF.emitSubstToOrigValue(loc, std::move(rv), OrigType,
                                      getSubstFormalType(), c);
    }

    RValue translate(SILGenFunction &SGF, SILLocation loc,
                     RValue &&rv, SGFContext c) && override {
      return SGF.emitOrigToSubstValue(loc, std::move(rv), OrigType,
                                      getSubstFormalType(), c);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone
        = new OrigToSubstComponent(getTypeData(), OrigType);
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OrigToSubstComponent("
                        << getOrigFormalType() << ", "
                        << getSubstFormalType() << ", "
                        << getTypeOfRValue() << ")\n";
    }
  };

  /// Remap an lvalue referencing a concrete type to an lvalue of a
  /// generically-reabstracted type.
  class SubstToOrigComponent : public TranslationPathComponent {
  public:
    SubstToOrigComponent(const LValueTypeData &typeData)
      : TranslationPathComponent(typeData, SubstToOrigKind)
    {}

    virtual bool isLoadingPure() const override { return true; }

    RValue untranslate(SILGenFunction &SGF, SILLocation loc,
                       RValue &&rv, SGFContext c) && override {
      return SGF.emitOrigToSubstValue(loc, std::move(rv), getOrigFormalType(),
                                      getSubstFormalType(), c);
    }

    RValue translate(SILGenFunction &SGF, SILLocation loc,
                     RValue &&rv, SGFContext c) && override {
      return SGF.emitSubstToOrigValue(loc, std::move(rv), getOrigFormalType(),
                                      getSubstFormalType(), c);
    }
    
    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone
        = new SubstToOrigComponent(getTypeData());
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "SubstToOrigComponent("
                        << getOrigFormalType() << ", "
                        << getSubstFormalType() << ", "
                        << getTypeOfRValue() << ")\n";
    }
  };

  /// Remap a weak value to Optional<T>*, or unowned pointer to T*.
  class OwnershipComponent : public LogicalPathComponent {
  public:
    OwnershipComponent(LValueTypeData typeData)
      : LogicalPathComponent(typeData, OwnershipKind) {
    }

    virtual bool isLoadingPure() const override { return true; }

    Optional<AccessedStorage> getAccessedStorage() const override {
      return None;
    }

    RValue get(SILGenFunction &SGF, SILLocation loc,
               ManagedValue base, SGFContext c) && override {
      assert(base && "ownership component must not be root of lvalue path");
      auto &TL = SGF.getTypeLowering(getTypeOfRValue());

      // Load the original value.
      ManagedValue result = SGF.emitLoad(loc, base.getValue(), TL,
                                         SGFContext(), IsNotTake);
      return RValue(SGF, loc, getSubstFormalType(), result);
    }

    void set(SILGenFunction &SGF, SILLocation loc,
             ArgumentSource &&valueSource, ManagedValue base) && override {
      assert(base && "ownership component must not be root of lvalue path");
      auto &TL = SGF.getTypeLowering(base.getType());

      auto value = std::move(valueSource).getAsSingleValue(SGF).forward(SGF);
      SGF.emitSemanticStore(loc, value, base.getValue(), TL,
                            IsNotInitialization);
    }

    std::unique_ptr<LogicalPathComponent>
    clone(SILGenFunction &SGF, SILLocation loc) const override {
      LogicalPathComponent *clone = new OwnershipComponent(getTypeData());
      return std::unique_ptr<LogicalPathComponent>(clone);
    }

    void dump(raw_ostream &OS, unsigned indent) const override {
      OS.indent(indent) << "OwnershipComponent(...)\n";
    }
  };
} // end anonymous namespace

LValue LValue::forValue(SGFAccessKind accessKind, ManagedValue value,
                        CanType substFormalType) {
  if (value.getType().isObject()) {
    LValueTypeData typeData = getValueTypeData(accessKind, substFormalType,
                                               value.getValue());

    LValue lv;
    lv.add<ValueComponent>(value, None, typeData, /*isRValue=*/true);
    return lv;
  } else {
    // Treat an address-only value as an lvalue we only read from.
    if (!value.isLValue())
      value = ManagedValue::forLValue(value.getValue());
    return forAddress(accessKind, value, None,
                      AbstractionPattern(substFormalType), substFormalType);
  }
}

LValue LValue::forAddress(SGFAccessKind accessKind, ManagedValue address,
                          Optional<SILAccessEnforcement> enforcement,
                          AbstractionPattern origFormalType,
                          CanType substFormalType) {
  assert(address.isLValue());
  LValueTypeData typeData = {
    accessKind, origFormalType, substFormalType,
    address.getType().getObjectType()
  };

  LValue lv;
  lv.add<ValueComponent>(address, enforcement, typeData);
  return lv;
}

void LValue::addMemberComponent(SILGenFunction &SGF, SILLocation loc,
                                AbstractStorageDecl *storage,
                                SubstitutionMap subs,
                                LValueOptions options,
                                bool isSuper,
                                SGFAccessKind accessKind,
                                AccessStrategy accessStrategy,
                                CanType formalRValueType,
                                PreparedArguments &&indices,
                                Expr *indexExprForDiagnostics) {
  if (auto var = dyn_cast<VarDecl>(storage)) {
    assert(indices.isNull());
    addMemberVarComponent(SGF, loc, var, subs, options, isSuper,
                          accessKind, accessStrategy, formalRValueType);
  } else {
    auto subscript = cast<SubscriptDecl>(storage);
    addMemberSubscriptComponent(SGF, loc, subscript, subs, options, isSuper,
                                accessKind, accessStrategy, formalRValueType,
                                std::move(indices), indexExprForDiagnostics);
  }
}

void LValue::addOrigToSubstComponent(SILType loweredSubstType) {
  loweredSubstType = loweredSubstType.getObjectType();
  assert(getTypeOfRValue() != loweredSubstType &&
         "reabstraction component is unnecessary!");

  // Peephole away complementary reabstractions.
  assert(!Path.empty() && "adding translation component to empty l-value");
  if (Path.back()->getKind() == PathComponent::SubstToOrigKind) {
    // But only if the lowered type matches exactly.
    if (Path[Path.size()-2]->getTypeOfRValue() == loweredSubstType) {
      Path.pop_back();
      return;
    }
    // TODO: combine reabstractions; this doesn't matter all that much
    // for most things, but it can be dramatically better for function
    // reabstraction.
  }

  auto substFormalType = getSubstFormalType();
  LValueTypeData typeData = {
    getAccessKind(),
    AbstractionPattern(substFormalType),
    substFormalType,
    loweredSubstType
  };
  add<OrigToSubstComponent>(typeData, getOrigFormalType());
}

void LValue::addSubstToOrigComponent(AbstractionPattern origType,
                                     SILType loweredSubstType) {
  loweredSubstType = loweredSubstType.getObjectType();
  assert(getTypeOfRValue() != loweredSubstType &&
         "reabstraction component is unnecessary!");

  // Peephole away complementary reabstractions.
  assert(!Path.empty() && "adding translation component to empty l-value");
  if (Path.back()->getKind() == PathComponent::OrigToSubstKind) {
    // But only if the lowered type matches exactly.
    if (Path[Path.size()-2]->getTypeOfRValue() == loweredSubstType) {
      Path.pop_back();
      return;
    }
    // TODO: combine reabstractions; this doesn't matter all that much
    // for most things, but it can be dramatically better for function
    // reabstraction.
  }

  LValueTypeData typeData = {
    getAccessKind(),
    origType,
    getSubstFormalType(),
    loweredSubstType
  };
  add<SubstToOrigComponent>(typeData);
}

void LValue::dump() const {
  dump(llvm::errs());
}

void LValue::dump(raw_ostream &OS, unsigned indent) const {
  for (const auto &component : *this) {
    component->dump(OS, indent);
  }
}

LValue SILGenFunction::emitLValue(Expr *e, SGFAccessKind accessKind,
                                  LValueOptions options) {
  // Some lvalue nodes (namely BindOptionalExprs) require immediate evaluation
  // of their subexpression, so we must have a writeback scope open while
  // building an lvalue.
  assert(InFormalEvaluationScope && "must be in a formal evaluation scope");

  LValue r = SILGenLValue(*this).visit(e, accessKind, options);
  // If the final component has an abstraction change, introduce a
  // reabstraction component.
  auto substFormalType = r.getSubstFormalType();
  auto loweredSubstType = getLoweredType(substFormalType);
  if (r.getTypeOfRValue() != loweredSubstType.getObjectType()) {
    // Logical components always re-abstract back to the substituted
    // type.
    assert(r.isLastComponentPhysical());
    r.addOrigToSubstComponent(loweredSubstType);
  }
  return r;
}

static LValue visitRecInOut(SILGenLValue &SGL, Expr *e,
                            SGFAccessKind accessKind,
                            LValueOptions options, AbstractionPattern orig) {
  auto lv = SGL.visit(e, accessKind, options);
  // If necessary, handle reabstraction with a SubstToOrigComponent that handles
  // writeback in the original representation.
  if (orig.isValid()) {
    auto &origTL = SGL.SGF.getTypeLowering(orig, e->getType()->getRValueType());
    if (lv.getTypeOfRValue() != origTL.getLoweredType().getObjectType())
      lv.addSubstToOrigComponent(orig, origTL.getLoweredType().getObjectType());
  }

  return lv;
}

// Otherwise we have a non-lvalue type (references, values, metatypes,
// etc). These act as the root of a logical lvalue.
static ManagedValue visitRecNonInOutBase(SILGenLValue &SGL, Expr *e,
                                         SGFAccessKind accessKind,
                                         LValueOptions options,
                                         AbstractionPattern orig) {
  auto &SGF = SGL.SGF;

  // For an rvalue base, apply the reabstraction (if any) eagerly, since
  // there's no need for writeback.
  if (orig.isValid()) {
    return SGF.emitRValueAsOrig(
        e, orig, SGF.getTypeLowering(orig, e->getType()->getRValueType()));
  }

  // Ok, at this point we know that re-abstraction is not required.
  SGFContext ctx;

  if (auto *dre = dyn_cast<DeclRefExpr>(e)) {
    // Any reference to "self" can be done at +0 so long as it is a direct
    // access, since we know it is guaranteed.
    //
    // TODO: it would be great to factor this even lower into SILGen to the
    // point where we can see that the parameter is +0 guaranteed.  Note that
    // this handles the case in initializers where there is actually a stack
    // allocation for it as well.
    if (isa<ParamDecl>(dre->getDecl()) &&
        dre->getDecl()->getFullName() == SGF.getASTContext().Id_self &&
        dre->getDecl()->isImplicit()) {
      ctx = SGFContext::AllowGuaranteedPlusZero;
      if (SGF.SelfInitDelegationState != SILGenFunction::NormalSelf) {
        // This needs to be inlined since there is a Formal Evaluation Scope
        // in emitRValueForDecl that causing any borrow for this LValue to be
        // popped too soon.
        auto *vd = cast<ParamDecl>(dre->getDecl());
        CanType formalRValueType = dre->getType()->getCanonicalType();
        ManagedValue selfLValue =
          SGF.emitAddressOfLocalVarDecl(dre, vd, formalRValueType,
                                        SGFAccessKind::OwnedObjectRead);
        selfLValue = SGF.emitFormalEvaluationRValueForSelfInDelegationInit(
                            e, formalRValueType,
                            selfLValue.getLValueAddress(), ctx)
                         .getAsSingleValue(SGF, e);

        return selfLValue;
      }
    }

    if (auto *VD = dyn_cast<VarDecl>(dre->getDecl())) {
      // All let values are guaranteed to be held alive across their lifetime,
      // and won't change once initialized.  Any loaded value is good for the
      // duration of this expression evaluation.
      if (VD->isLet()) {
        ctx = SGFContext::AllowGuaranteedPlusZero;
      }
    }
  }

  if (SGF.SGM.Types.isIndirectPlusZeroSelfParameter(e->getType())) {
    ctx = SGFContext::AllowGuaranteedPlusZero;
  }

  ManagedValue mv = SGF.emitRValueAsSingleValue(e, ctx);
  if (mv.isPlusZeroRValueOrTrivial())
    return mv;

  // Any temporaries needed to materialize the lvalue must be destroyed when
  // at the end of the lvalue's formal evaluation scope.
  // e.g. for foo(self.bar)
  //   %self = load [copy] %ptr_self
  //   %rvalue = barGetter(%self)
  //   destroy_value %self // self must be released before calling foo.
  //   foo(%rvalue)
  SILValue value = mv.forward(SGF);
  return SGF.emitFormalAccessManagedRValueWithCleanup(CleanupLocation(e),
                                                      value);
}

LValue SILGenLValue::visitRec(Expr *e, SGFAccessKind accessKind,
                              LValueOptions options, AbstractionPattern orig) {
  // First see if we have an lvalue type. If we do, then quickly handle that and
  // return.
  if (e->getType()->is<LValueType>() || e->isSemanticallyInOutExpr()) {
    return visitRecInOut(*this, e, accessKind, options, orig);
  }

  // Otherwise we have a non-lvalue type (references, values, metatypes,
  // etc). These act as the root of a logical lvalue. Compute the root value,
  // wrap it in a ValueComponent, and return it for our caller.
  ManagedValue rv = visitRecNonInOutBase(*this, e, accessKind, options, orig);
  CanType formalType = getSubstFormalRValueType(e);
  auto typeData = getValueTypeData(accessKind, formalType, rv.getValue());
  LValue lv;
  lv.add<ValueComponent>(rv, None, typeData, /*isRValue=*/true);
  return lv;
}

LValue SILGenLValue::visitExpr(Expr *e, SGFAccessKind accessKind,
                               LValueOptions options) {
  e->dump(llvm::errs());
  llvm_unreachable("unimplemented lvalue expr");
}

namespace {
  /// A CRTP class for emitting accesses.
  template <class Impl, class StorageType>
  struct AccessEmitter {
    SILGenFunction &SGF;
    StorageType *Storage;
    CanType FormalRValueType;
    SGFAccessKind AccessKind;

    Impl &asImpl() { return static_cast<Impl&>(*this); }

    AccessEmitter(SILGenFunction &SGF, StorageType *storage,
                  SGFAccessKind accessKind, CanType formalRValueType)
      : SGF(SGF), Storage(storage), FormalRValueType(formalRValueType),
        AccessKind(accessKind) {}

    void emitUsingStrategy(AccessStrategy strategy) {
      switch (strategy.getKind()) {
      case AccessStrategy::Storage: {
        auto typeData = getPhysicalStorageTypeData(SGF.SGM, AccessKind, Storage,
                                                   FormalRValueType);
        return asImpl().emitUsingStorage(typeData);
      }

      case AccessStrategy::BehaviorStorage:
        return asImpl().emitUsingBehaviorStorage();

      case AccessStrategy::DirectToAccessor:
        return asImpl().emitUsingAccessor(strategy.getAccessor(), true);

      case AccessStrategy::DispatchToAccessor:
        return asImpl().emitUsingAccessor(strategy.getAccessor(), false);

      case AccessStrategy::MaterializeToTemporary: {
        auto typeData =
          getLogicalStorageTypeData(SGF.SGM, AccessKind, FormalRValueType);
        return asImpl().emitUsingMaterialization(strategy.getReadStrategy(),
                                                 strategy.getWriteStrategy(),
                                                 typeData);
      }
      }
      llvm_unreachable("unknown kind");
    }

    void emitUsingAccessor(AccessorKind accessorKind, bool isDirect) {
      auto accessor =
        SGF.SGM.getAccessorDeclRef(Storage->getAccessor(accessorKind));

      switch (accessorKind) {
      case AccessorKind::Get:
      case AccessorKind::Set: {
        auto typeData =
          getLogicalStorageTypeData(SGF.SGM, AccessKind, FormalRValueType);
        return asImpl().emitUsingGetterSetter(accessor, isDirect, typeData);
      }

      case AccessorKind::Address:
      case AccessorKind::MutableAddress: {
        auto typeData = getPhysicalStorageTypeData(SGF.SGM, AccessKind, Storage,
                                                   FormalRValueType);
        return asImpl().emitUsingAddressor(accessor, isDirect, typeData);
      }

      case AccessorKind::Read:
      case AccessorKind::Modify: {
        auto typeData = getPhysicalStorageTypeData(SGF.SGM, AccessKind, Storage,
                                                   FormalRValueType);
        return asImpl().emitUsingCoroutineAccessor(accessor, isDirect,
                                                   typeData);
      }

      case AccessorKind::WillSet:
      case AccessorKind::DidSet:
        llvm_unreachable("cannot use accessor directly to perform an access");
      }
      llvm_unreachable("bad kind");
    }
  };
}

SubstitutionMap
SILGenModule::getNonMemberVarDeclSubstitutions(VarDecl *var) {
  auto *dc = var->getDeclContext();
  if (auto *genericEnv = dc->getGenericEnvironmentOfContext())
    return genericEnv->getForwardingSubstitutionMap();

  return SubstitutionMap();
}

static LValue emitLValueForNonMemberVarDecl(SILGenFunction &SGF,
                                            SILLocation loc, VarDecl *var,
                                            CanType formalRValueType,
                                            SGFAccessKind accessKind,
                                            LValueOptions options,
                                            AccessSemantics semantics) {
  LValue lv;

  auto access = getFormalAccessKind(accessKind);
  auto strategy = var->getAccessStrategy(semantics, access, SGF.FunctionDC);

  lv.addNonMemberVarComponent(SGF, loc, var, /*be lazy*/ None,
                              options, accessKind, strategy, formalRValueType);

  return lv;
}

void LValue::addNonMemberVarComponent(SILGenFunction &SGF, SILLocation loc,
                                      VarDecl *var,
                                      Optional<SubstitutionMap> subs,
                                      LValueOptions options,
                                      SGFAccessKind accessKind,
                                      AccessStrategy strategy,
                                      CanType formalRValueType) {
  struct NonMemberVarAccessEmitter :
      AccessEmitter<NonMemberVarAccessEmitter, VarDecl> {
    LValue &LV;
    SILLocation Loc;
    Optional<SubstitutionMap> Subs;
    LValueOptions Options;

    SubstitutionMap getSubs() {
      if (Subs) return *Subs;
      Subs = SGF.SGM.getNonMemberVarDeclSubstitutions(Storage);
      return *Subs;
    }

    NonMemberVarAccessEmitter(SILGenFunction &SGF, SILLocation loc,
                              VarDecl *var, Optional<SubstitutionMap> subs,
                              SGFAccessKind accessKind,
                              CanType formalRValueType,
                              LValueOptions options, LValue &lv)
      : AccessEmitter(SGF, var, accessKind, formalRValueType),
        LV(lv), Loc(loc), Subs(subs), Options(options) {}

    void emitUsingAddressor(SILDeclRef addressor, bool isDirect,
                            LValueTypeData typeData) {
      SILType storageType =
        SGF.SGM.Types.getLoweredType(Storage->getType()).getAddressType();
      LV.add<AddressorComponent>(Storage, addressor,
                                 /*isSuper=*/false, isDirect, getSubs(),
                                 CanType(), typeData, storageType,
                                 nullptr, PreparedArguments());
    }

    void emitUsingCoroutineAccessor(SILDeclRef accessor, bool isDirect,
                                    LValueTypeData typeData) {
      LV.add<CoroutineAccessorComponent>(Storage, accessor,
                                         /*isSuper*/ false, isDirect, getSubs(),
                                         CanType(), typeData,
                                         nullptr, PreparedArguments());
    }

    void emitUsingGetterSetter(SILDeclRef accessor, bool isDirect,
                               LValueTypeData typeData) {
      LV.add<GetterSetterComponent>(Storage, accessor,
                                    /*isSuper=*/false, isDirect, getSubs(),
                                    CanType(), typeData,
                                    nullptr, PreparedArguments());
    }

    void emitUsingMaterialization(AccessStrategy readStrategy,
                                  AccessStrategy writeStrategy,
                                  LValueTypeData typeData) {
      LV.add<MaterializeToTemporaryComponent>(Storage, /*super*/false,
                                              getSubs(), Options,
                                              readStrategy, writeStrategy,
                                              /*base type*/CanType(), typeData,
                                              nullptr, PreparedArguments());
    }

    void emitUsingStorage(LValueTypeData typeData) {
      // If it's a physical value (e.g. a local variable in memory), push its
      // address.

      // Check for a local (possibly captured) variable.
      auto address = SGF.maybeEmitValueOfLocalVarDecl(Storage);

      // The only other case that should get here is a global variable.
      if (!address) {
        address = SGF.emitGlobalVariableRef(Loc, Storage);
      }
      assert(address.isLValue() &&
             "physical lvalue decl ref must evaluate to an address");

      Optional<SILAccessEnforcement> enforcement;
      if (!Storage->isLet()) {
        if (Options.IsNonAccessing) {
          enforcement = None;
        } else if (Storage->getDeclContext()->isLocalContext()) {
          enforcement = SGF.getUnknownEnforcement(Storage);
        } else if (Storage->getDeclContext()->isModuleScopeContext()) {
          enforcement = SGF.getDynamicEnforcement(Storage);
        } else {
          assert(Storage->getDeclContext()->isTypeContext() &&
                 !Storage->isInstanceMember());
          enforcement = SGF.getDynamicEnforcement(Storage);
        }
      }

      LV.add<ValueComponent>(address, enforcement, typeData);

      if (address.getType().is<ReferenceStorageType>())
        LV.add<OwnershipComponent>(typeData);
    }
  
    void emitUsingBehaviorStorage() {
      // TODO: Behaviors aren't supported for non-instance properties yet.
      llvm_unreachable("not implemented");
    }
  } emitter(SGF, loc, var, subs, accessKind, formalRValueType, options, *this);

  emitter.emitUsingStrategy(strategy);
}

ManagedValue
SILGenFunction::maybeEmitValueOfLocalVarDecl(VarDecl *var) {
  // For local decls, use the address we allocated or the value if we have it.
  auto It = VarLocs.find(var);
  if (It != VarLocs.end()) {
    // If this has an address, return it.  By-value let's have no address.
    SILValue ptr = It->second.value;
    if (ptr->getType().isAddress())
      return ManagedValue::forLValue(ptr);

    // Otherwise, it is an RValue let.  Uses of it are borrows, but we don't
    // want to proactively emit a borrow here.
    // TODO: integrate this with how callers want these values so we can do
    // something more semantic than just forUnmanaged.
    return ManagedValue::forUnmanaged(ptr);
  }

  // Otherwise, it's non-local or not stored.
  return ManagedValue();
}

ManagedValue
SILGenFunction::emitAddressOfLocalVarDecl(SILLocation loc, VarDecl *var,
                                          CanType formalRValueType,
                                          SGFAccessKind accessKind) {
  assert(var->getDeclContext()->isLocalContext());
  assert(var->getImplInfo().isSimpleStored());
  auto address = maybeEmitValueOfLocalVarDecl(var);
  assert(address);
  assert(address.isLValue());
  return address;
}

RValue SILGenFunction::emitRValueForNonMemberVarDecl(SILLocation loc,
                                                     VarDecl *var,
                                                     CanType formalRValueType,
                                                     AccessSemantics semantics,
                                                     SGFContext C) {
  // Any writebacks for this access are tightly scoped.
  FormalEvaluationScope scope(*this);

  auto localValue = maybeEmitValueOfLocalVarDecl(var);

  // If this VarDecl is represented as an address, emit it as an lvalue, then
  // perform a load to get the rvalue.
  if (localValue && localValue.isLValue()) {
    bool guaranteedValid = false;
    IsTake_t shouldTake = IsNotTake;

    // We should only end up in this path for local and global variables,
    // i.e. ones whose lifetime is assured for the duration of the evaluation.
    // Therefore, if the variable is a constant, the value is guaranteed
    // valid as well.
    if (var->isLet())
      guaranteedValid = true;

    // Protect the lvalue read with access markers. The !is<LValueType> assert
    // above ensures that the "LValue" is actually immutable, so we use an
    // unenforced access marker.
    SILValue destAddr = localValue.getLValueAddress();
    SILValue accessAddr = UnenforcedFormalAccess::enter(*this, loc, destAddr,
                                                        SILAccessKind::Read);
    auto propagateRValuePastAccess = [&](RValue &&rvalue) {
      // Check if a new begin_access was emitted and returned as the
      // RValue. This means that the load did not actually load. If so, then
      // fix the rvalue to begin_access operand. The end_access cleanup
      // doesn't change. FIXME: this can't happen with sil-opaque-values.
      if (accessAddr != destAddr && rvalue.isComplete()
          && rvalue.isPlusZero(*this) && !isa<TupleType>(rvalue.getType())) {
        auto mv = std::move(rvalue).getScalarValue();
        if (mv.getValue() == accessAddr)
          mv = std::move(mv).transform(
              cast<BeginAccessInst>(accessAddr)->getOperand());
        return RValue(*this, loc, formalRValueType, mv);
      }
      return std::move(rvalue);
    };
    // If we have self, see if we are in an 'init' delegation sequence. If so,
    // call out to the special delegation init routine. Otherwise, use the
    // normal RValue emission logic.
    if (var->getName() == getASTContext().Id_self &&
        SelfInitDelegationState != NormalSelf) {
      auto rvalue =
        emitRValueForSelfInDelegationInit(loc, formalRValueType, accessAddr, C);
      return propagateRValuePastAccess(std::move(rvalue));
    }

    // Avoid computing an abstraction pattern for local variables.
    // This is a slight compile-time optimization, but more importantly
    // it avoids problems where locals don't always have interface types.
    if (var->getDeclContext()->isLocalContext()) {
      auto &rvalueTL = getTypeLowering(formalRValueType);
      auto rvalue = RValue(*this, loc, formalRValueType,
                           emitLoad(loc, accessAddr, rvalueTL,
                                    C, shouldTake, guaranteedValid));

      return propagateRValuePastAccess(std::move(rvalue));
    }

    // Otherwise, do the full thing where we potentially bridge and
    // reabstract the declaration.
    auto origFormalType = SGM.Types.getAbstractionPattern(var);
    auto rvalue = RValue(*this, loc, formalRValueType,
                         emitLoad(loc, accessAddr, origFormalType,
                                  formalRValueType,
                                  getTypeLowering(formalRValueType),
                                  C, shouldTake, guaranteedValid));
    return propagateRValuePastAccess(std::move(rvalue));
  }

  // For local decls, use the address we allocated or the value if we have it.
  if (localValue) {
    // Mutable lvalue and address-only 'let's are LValues.
    assert(!localValue.getType().isAddress() &&
           "LValue cases should be handled above");

    SILValue Scalar = localValue.getUnmanagedValue();

    // For weak and unowned types, convert the reference to the right
    // pointer.
    if (Scalar->getType().is<ReferenceStorageType>()) {
      Scalar = emitConversionToSemanticRValue(loc, Scalar,
                                             getTypeLowering(formalRValueType));
      // emitConversionToSemanticRValue always produces a +1 strong result.
      return RValue(*this, loc, formalRValueType,
                    emitManagedRValueWithCleanup(Scalar));
    }

    // This is a let, so we can make guarantees, so begin the borrow scope.
    ManagedValue Result = emitManagedBeginBorrow(loc, Scalar);

    // If the client can't handle a +0 result, retain it to get a +1.
    // This is a 'let', so we can make guarantees.
    return RValue(*this, loc, formalRValueType,
                  C.isGuaranteedPlusZeroOk()
                    ? Result : Result.copyUnmanaged(*this, loc));
  }

  LValue lv = emitLValueForNonMemberVarDecl(*this, loc, var, formalRValueType,
                                            SGFAccessKind::OwnedObjectRead,
                                            LValueOptions(), semantics);
  return emitLoadOfLValue(loc, std::move(lv), C);
}

LValue SILGenLValue::visitDiscardAssignmentExpr(DiscardAssignmentExpr *e,
                                                SGFAccessKind accessKind,
                                                LValueOptions options) {
  LValueTypeData typeData = getValueTypeData(SGF, accessKind, e);

  SILValue address = SGF.emitTemporaryAllocation(e, typeData.TypeOfRValue);
  address = SGF.B.createMarkUninitialized(e, address,
                                          MarkUninitializedInst::Var);
  LValue lv;
  lv.add<ValueComponent>(SGF.emitManagedBufferWithCleanup(address),
                         None, typeData);
  return lv;
}


LValue SILGenLValue::visitDeclRefExpr(DeclRefExpr *e, SGFAccessKind accessKind,
                                      LValueOptions options) {
  // The only non-member decl that can be an lvalue is VarDecl.
  return emitLValueForNonMemberVarDecl(SGF, e, cast<VarDecl>(e->getDecl()),
                                       getSubstFormalRValueType(e),
                                       accessKind, options,
                                       e->getAccessSemantics());
}

LValue SILGenLValue::visitOpaqueValueExpr(OpaqueValueExpr *e,
                                          SGFAccessKind accessKind,
                                          LValueOptions options) {
  // Handle an opaque lvalue that refers to an opened existential.
  auto known = SGF.OpaqueValueExprs.find(e);
  if (known != SGF.OpaqueValueExprs.end()) {
    // Dig the open-existential expression out of the list.
    OpenExistentialExpr *opened = known->second;
    SGF.OpaqueValueExprs.erase(known);

    // Do formal evaluation of the underlying existential lvalue.
    auto lv = visitRec(opened->getExistentialValue(), accessKind, options);
    lv = SGF.emitOpenExistentialLValue(
        opened, std::move(lv),
        CanArchetypeType(opened->getOpenedArchetype()),
        e->getType()->getWithoutSpecifierType()->getCanonicalType(),
        accessKind);
    return lv;
  }

  assert(SGF.OpaqueValues.count(e) && "Didn't bind OpaqueValueExpr");

  auto &entry = SGF.OpaqueValues.find(e)->second;
  assert(!entry.HasBeenConsumed && "opaque value already consumed");
  entry.HasBeenConsumed = true;

  RegularLocation loc(e);
  LValue lv;
  lv.add<ValueComponent>(entry.Value.borrow(SGF, loc), None,
                         getValueTypeData(SGF, accessKind, e));
  return lv;
}

LValue SILGenLValue::visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e,
                                                   SGFAccessKind accessKind,
                                                   LValueOptions options) {
  SGF.emitIgnoredExpr(e->getLHS());
  return visitRec(e->getRHS(), accessKind, options);
}

static SGFAccessKind getBaseAccessKindForAccessor(SILGenModule &SGM,
                                                  AccessorDecl *accessor,
                                                  CanType baseFormalType) {
  if (accessor->isMutating()) {
    return SGFAccessKind::ReadWrite;
  } else if (SGM.shouldEmitSelfAsRValue(accessor, baseFormalType)) {
    return SGM.isNonMutatingSelfIndirect(SGM.getAccessorDeclRef(accessor))
             ? SGFAccessKind::OwnedAddressRead
             : SGFAccessKind::OwnedObjectRead;
  } else {
    return SGM.isNonMutatingSelfIndirect(SGM.getAccessorDeclRef(accessor))
             ? SGFAccessKind::BorrowedAddressRead
             : SGFAccessKind::BorrowedObjectRead;
  }
}

static SGFAccessKind getBaseAccessKindForStorage(SGFAccessKind accessKind) {
  // Assume that the member only partially projects the enclosing value,
  // so a write to the member is a read/write of the base.
  return (accessKind == SGFAccessKind::Write
            ? SGFAccessKind::ReadWrite : accessKind);
}

/// Return the appropriate access kind for the base l-value of a
/// particular member, which is being accessed in a particular way.
static SGFAccessKind getBaseAccessKind(SILGenModule &SGM,
                                       AbstractStorageDecl *member,
                                       SGFAccessKind accessKind,
                                       AccessStrategy strategy,
                                       CanType baseFormalType) {
  switch (strategy.getKind()) {
  case AccessStrategy::Storage:
    return getBaseAccessKindForStorage(accessKind);

  case AccessStrategy::MaterializeToTemporary: {
    assert(accessKind == SGFAccessKind::ReadWrite);
    auto writeBaseKind = getBaseAccessKind(SGM, member, SGFAccessKind::Write,
                                           strategy.getWriteStrategy(),
                                           baseFormalType);

    // Fast path for the common case that the write will need to mutate
    // the base.
    if (writeBaseKind == SGFAccessKind::ReadWrite)
      return writeBaseKind;

    auto readBaseKind = getBaseAccessKind(SGM, member,
                                          SGFAccessKind::OwnedAddressRead,
                                          strategy.getReadStrategy(),
                                          baseFormalType);

    // If they're the same kind, just use that.
    if (readBaseKind == writeBaseKind)
      return readBaseKind;

    // If either access is mutating, the total access is a read-write.
    if (!isReadAccess(readBaseKind) || !isReadAccess(writeBaseKind))
      return SGFAccessKind::ReadWrite;

    // Okay, we have two different kinds of read somehow for different
    // accessors of the same storage?
    return SGFAccessKind::OwnedObjectRead;
  }

  case AccessStrategy::DirectToAccessor:
  case AccessStrategy::DispatchToAccessor: {
    auto accessor = member->getAccessor(strategy.getAccessor());
    return getBaseAccessKindForAccessor(SGM, accessor, baseFormalType);
  }
    
  case AccessStrategy::BehaviorStorage:
    // We should only access the behavior storage for initialization purposes.
    assert(accessKind == SGFAccessKind::Write);
    return SGFAccessKind::ReadWrite;
  }
  llvm_unreachable("bad access strategy");
}

static CanType getBaseFormalType(Expr *baseExpr) {
  return baseExpr->getType()->getWithoutSpecifierType()->getCanonicalType();
}

LValue SILGenLValue::visitMemberRefExpr(MemberRefExpr *e,
                                        SGFAccessKind accessKind,
                                        LValueOptions options) {
  // MemberRefExpr can refer to type and function members, but the only case
  // that can be an lvalue is a VarDecl.
  VarDecl *var = cast<VarDecl>(e->getMember().getDecl());
  AccessStrategy strategy =
    var->getAccessStrategy(e->getAccessSemantics(),
                           getFormalAccessKind(accessKind), SGF.FunctionDC);

  LValue lv = visitRec(e->getBase(),
                       getBaseAccessKind(SGF.SGM, var, accessKind, strategy,
                                         getBaseFormalType(e->getBase())),
                       getBaseOptions(options, strategy));
  assert(lv.isValid());

  CanType substFormalRValueType = getSubstFormalRValueType(e);
  lv.addMemberVarComponent(SGF, e, var,
                           e->getMember().getSubstitutions(),
                           options, e->isSuper(), accessKind,
                           strategy, substFormalRValueType);
  return lv;
}

namespace {

/// A CRTP class for emitting member accesses.
template <class Impl, class StorageType>
struct MemberStorageAccessEmitter : AccessEmitter<Impl, StorageType> {
  using super = AccessEmitter<Impl, StorageType>;
  using super::SGF;
  using super::Storage;
  using super::FormalRValueType;
  LValue &LV;
  LValueOptions Options;
  SILLocation Loc;
  bool IsSuper;
  CanType BaseFormalType;
  SubstitutionMap Subs;
  Expr *IndexExprForDiagnostics;
  PreparedArguments Indices;

  MemberStorageAccessEmitter(SILGenFunction &SGF, SILLocation loc,
                             StorageType *storage,
                             SubstitutionMap subs,
                             bool isSuper,
                             SGFAccessKind accessKind,
                             CanType formalRValueType,
                             LValueOptions options,
                             LValue &lv,
                             Expr *indexExprForDiagnostics,
                             PreparedArguments &&indices)
    : super(SGF, storage, accessKind, formalRValueType),
      LV(lv), Options(options), Loc(loc), IsSuper(isSuper),
      BaseFormalType(lv.getSubstFormalType()), Subs(subs),
      IndexExprForDiagnostics(indexExprForDiagnostics),
      Indices(std::move(indices)) {}

  void emitUsingAddressor(SILDeclRef addressor, bool isDirect,
                          LValueTypeData typeData) {
    SILType varStorageType =
      SGF.SGM.Types.getSubstitutedStorageType(Storage, FormalRValueType);

    LV.add<AddressorComponent>(Storage, addressor, IsSuper, isDirect, Subs,
                               BaseFormalType, typeData, varStorageType,
                               IndexExprForDiagnostics, std::move(Indices));
  }

  void emitUsingCoroutineAccessor(SILDeclRef accessor, bool isDirect,
                                  LValueTypeData typeData) {
    LV.add<CoroutineAccessorComponent>(Storage, accessor, IsSuper, isDirect,
                                       Subs, BaseFormalType, typeData,
                                       IndexExprForDiagnostics,
                                       std::move(Indices));
  }

  void emitUsingGetterSetter(SILDeclRef accessor, bool isDirect,
                             LValueTypeData typeData) {
    LV.add<GetterSetterComponent>(Storage, accessor, IsSuper, isDirect,
                                  Subs, BaseFormalType, typeData,
                                  IndexExprForDiagnostics, std::move(Indices));
  }

  void emitUsingMaterialization(AccessStrategy readStrategy,
                                AccessStrategy writeStrategy,
                                LValueTypeData typeData) {
    LV.add<MaterializeToTemporaryComponent>(Storage, IsSuper, Subs, Options,
                                            readStrategy, writeStrategy,
                                            BaseFormalType, typeData,
                                            IndexExprForDiagnostics,
                                            std::move(Indices));
  }
};
} // end anonymous namespace

void LValue::addMemberVarComponent(SILGenFunction &SGF, SILLocation loc,
                                   VarDecl *var,
                                   SubstitutionMap subs,
                                   LValueOptions options,
                                   bool isSuper,
                                   SGFAccessKind accessKind,
                                   AccessStrategy strategy,
                                   CanType formalRValueType) {
  struct MemberVarAccessEmitter
      : MemberStorageAccessEmitter<MemberVarAccessEmitter, VarDecl> {
    using MemberStorageAccessEmitter::MemberStorageAccessEmitter;

    void emitUsingStorage(LValueTypeData typeData) {
      // For static variables, emit a reference to the global variable backing
      // them.
      // FIXME: This has to be dynamically looked up for classes, and
      // dynamically instantiated for generics.
      if (Storage->isStatic()) {
        // FIXME: this implicitly drops the earlier components, but maybe
        // we ought to evaluate them for side-effects even during the
        // formal access?
        LV.Path.clear();
        LV.addNonMemberVarComponent(SGF, Loc, Storage, Subs, Options,
                                    typeData.getAccessKind(),
                                    AccessStrategy::getStorage(),
                                    FormalRValueType);
        return;
      }

      // Otherwise, it's a physical member.
      SILType varStorageType =
        SGF.SGM.Types.getSubstitutedStorageType(Storage, FormalRValueType);

      if (BaseFormalType->mayHaveSuperclass()) {
        LV.add<RefElementComponent>(Storage, Options, varStorageType, typeData);
      } else {
        assert(BaseFormalType->getStructOrBoundGenericStruct());
        LV.add<StructElementComponent>(Storage, varStorageType, typeData);
      }

      // If the member has weak or unowned storage, convert it away.
      if (varStorageType.is<ReferenceStorageType>()) {
        LV.add<OwnershipComponent>(typeData);
      }
    }

    // For behavior initializations, we should have set up a marking proxy that
    // replaces the access path.
    void emitUsingBehaviorStorage() {
      auto addr = SGF.maybeEmitValueOfLocalVarDecl(Storage);
      assert(addr && addr.isLValue());
      LV = LValue();
      auto typeData = getPhysicalStorageTypeData(SGF.SGM, AccessKind, Storage,
                                                 FormalRValueType);
      LV.add<ValueComponent>(addr, None, typeData);
    }
  } emitter(SGF, loc, var, subs, isSuper, accessKind,
            formalRValueType, options, *this,
            /*indices for diags*/ nullptr, /*indices*/ PreparedArguments());

  emitter.emitUsingStrategy(strategy);
}

LValue SILGenLValue::visitSubscriptExpr(SubscriptExpr *e,
                                        SGFAccessKind accessKind,
                                        LValueOptions options) {
  auto decl = cast<SubscriptDecl>(e->getDecl().getDecl());
  auto subs = e->getDecl().getSubstitutions();

  auto accessSemantics = e->getAccessSemantics();
  auto strategy =
    decl->getAccessStrategy(accessSemantics,
                            getFormalAccessKind(accessKind), SGF.FunctionDC);
  
  LValue lv = visitRec(e->getBase(),
                       getBaseAccessKind(SGF.SGM, decl, accessKind, strategy,
                                         getBaseFormalType(e->getBase())),
                       getBaseOptions(options, strategy));
  assert(lv.isValid());

  Expr *indexExpr = e->getIndex();
  auto indices = SGF.prepareSubscriptIndices(decl, subs, strategy, indexExpr);

  CanType formalRValueType = getSubstFormalRValueType(e);
  lv.addMemberSubscriptComponent(SGF, e, decl, subs,
                                 options, e->isSuper(), accessKind, strategy,
                                 formalRValueType, std::move(indices),
                                 indexExpr);
  return lv;
}

LValue SILGenLValue::visitKeyPathApplicationExpr(KeyPathApplicationExpr *e,
                                                 SGFAccessKind accessKind,
                                                 LValueOptions options) {
  // Determine the base access strategy based on the strategy of this access.
  auto keyPathTy = e->getKeyPath()->getType()->castTo<BoundGenericType>();
  SGFAccessKind subAccess;
  if (keyPathTy->getDecl() == SGF.getASTContext().getWritableKeyPathDecl()) {
    // Assume the keypath only partially projects the root value.
    subAccess = (isReadAccess(accessKind)
                    ? SGFAccessKind::BorrowedAddressRead
                    : SGFAccessKind::ReadWrite);
  } else {
    // The base is only ever read from a read-only or reference-writable
    // keypath.
    subAccess = SGFAccessKind::BorrowedAddressRead;
  }

  // For now, just ignore any options we were given.
  LValueOptions subOptions = LValueOptions();
  
  // The base should be reabstracted to the maximal abstraction pattern.
  LValue lv = visitRec(e->getBase(), subAccess, subOptions,
                       AbstractionPattern::getOpaque());
  
  // The result will end up projected at the maximal abstraction level too.
  auto resultTy = e->getType()->getRValueType()->getCanonicalType();
  auto resultSILTy = SGF.getLoweredType(AbstractionPattern::getOpaque(),
                                        resultTy);
  
  
  lv.add<KeyPathApplicationComponent>(
    LValueTypeData(accessKind, AbstractionPattern::getOpaque(), resultTy,
                   resultSILTy.getObjectType()),
    ArgumentSource(e->getKeyPath()));
  
  // Reabstract to the substituted abstraction level if necessary.
  auto substResultSILTy = SGF.getLoweredType(resultTy);
  if (resultSILTy.getObjectType() != substResultSILTy.getObjectType()) {
    lv.addOrigToSubstComponent(substResultSILTy);
  }
  
  return lv;
}

void LValue::addMemberSubscriptComponent(SILGenFunction &SGF, SILLocation loc,
                                         SubscriptDecl *decl,
                                         SubstitutionMap subs,
                                         LValueOptions options,
                                         bool isSuper,
                                         SGFAccessKind accessKind,
                                         AccessStrategy strategy,
                                         CanType formalRValueType,
                                         PreparedArguments &&indices,
                                         Expr *indexExprForDiagnostics) {
  struct MemberSubscriptAccessEmitter
      : MemberStorageAccessEmitter<MemberSubscriptAccessEmitter,
                                   SubscriptDecl> {
    using MemberStorageAccessEmitter::MemberStorageAccessEmitter;

    void emitUsingStorage(LValueTypeData typeData) {
      llvm_unreachable("subscripts never have storage");
    }

    void emitUsingBehaviorStorage() {
      llvm_unreachable("subscripts never have behaviors");
    }
  } emitter(SGF, loc, decl, subs, isSuper,
            accessKind, formalRValueType, options, *this,
            indexExprForDiagnostics, std::move(indices));

  emitter.emitUsingStrategy(strategy);
}

bool LValue::isObviouslyNonConflicting(const LValue &other,
                                       SGFAccessKind selfAccess,
                                       SGFAccessKind otherAccess) {
  // Reads never conflict with reads.
  if (isReadAccess(selfAccess) && isReadAccess(otherAccess))
    return true;

  // We can cover more cases here.
  return false;
}

LValue SILGenLValue::visitTupleElementExpr(TupleElementExpr *e,
                                           SGFAccessKind accessKind,
                                           LValueOptions options) {
  unsigned index = e->getFieldNumber();
  LValue lv = visitRec(e->getBase(),
                       getBaseAccessKindForStorage(accessKind),
                       options.forProjectedBaseLValue());

  auto baseTypeData = lv.getTypeData();
  LValueTypeData typeData = {
    accessKind,
    baseTypeData.OrigFormalType.getTupleElementType(index),
    cast<TupleType>(baseTypeData.SubstFormalType).getElementType(index),
    baseTypeData.TypeOfRValue.getTupleElementType(index)
  };

  lv.add<TupleElementComponent>(index, typeData);
  return lv;
}

LValue SILGenLValue::visitOpenExistentialExpr(OpenExistentialExpr *e,
                                              SGFAccessKind accessKind,
                                              LValueOptions options) {
  // If the opaque value is not an lvalue, open the existential immediately.
  if (!e->getOpaqueValue()->getType()->is<LValueType>()) {
    return SGF.emitOpenExistentialExpr<LValue>(e,
                                               [&](Expr *subExpr) -> LValue {
                                                 return visitRec(subExpr,
                                                                 accessKind,
                                                                 options);
                                               });
  }

  // Record the fact that we're opening this existential. The actual
  // opening operation will occur when we see the OpaqueValueExpr.
  bool inserted = SGF.OpaqueValueExprs.insert({e->getOpaqueValue(), e}).second;
  (void)inserted;
  assert(inserted && "already have this opened existential?");

  // Visit the subexpression.
  LValue lv = visitRec(e->getSubExpr(), accessKind, options);

  // Sanity check that we did see the OpaqueValueExpr.
  assert(SGF.OpaqueValueExprs.count(e->getOpaqueValue()) == 0 &&
         "opened existential not removed?");
  return lv;
}

static LValueTypeData
getOptionalObjectTypeData(SILGenFunction &SGF, SGFAccessKind accessKind,
                          const LValueTypeData &baseTypeData) {
  EnumElementDecl *someDecl = SGF.getASTContext().getOptionalSomeDecl();

  return {
      accessKind,
      baseTypeData.OrigFormalType.getOptionalObjectType(),
      baseTypeData.SubstFormalType.getOptionalObjectType(),
      baseTypeData.TypeOfRValue.getEnumElementType(someDecl, SGF.SGM.M),
  };
}

LValue SILGenLValue::visitForceValueExpr(ForceValueExpr *e,
                                         SGFAccessKind accessKind,
                                         LValueOptions options) {
  // Like BindOptional, this is a read even if we only write to the result.
  // (But it's unnecessary to use a force this way!)
  LValue lv = visitRec(e->getSubExpr(),
                       getBaseAccessKindForStorage(accessKind),
                       options.forComputedBaseLValue());
  LValueTypeData typeData =
    getOptionalObjectTypeData(SGF, accessKind, lv.getTypeData());
  bool isImplicitUnwrap = e->isImplicit() &&
    e->isForceOfImplicitlyUnwrappedOptional(); 
  lv.add<ForceOptionalObjectComponent>(typeData, isImplicitUnwrap);
  return lv;
}

LValue SILGenLValue::visitBindOptionalExpr(BindOptionalExpr *e,
                                           SGFAccessKind accessKind,
                                           LValueOptions options) {
  // Binding reads the base even if we then only write to the result.
  auto baseAccessKind = getBaseAccessKindForStorage(accessKind);

  // We're going to take the address of the base.
  // TODO: deal more efficiently with an object-preferring access.
  baseAccessKind = getAddressAccessKind(baseAccessKind);

  // Do formal evaluation of the base l-value.
  LValue optLV = visitRec(e->getSubExpr(), baseAccessKind,
                          options.forComputedBaseLValue());

  LValueTypeData optTypeData = optLV.getTypeData();
  LValueTypeData valueTypeData =
    getOptionalObjectTypeData(SGF, accessKind, optTypeData);

  // The chaining operator immediately begins a formal access to the
  // base l-value.  In concrete terms, this means we can immediately
  // evaluate the base down to an address.
  ManagedValue optAddr = SGF.emitAddressOfLValue(e, std::move(optLV));

  // Bind the value, branching to the destination address if there's no
  // value there.
  SGF.emitBindOptionalAddress(e, optAddr, e->getDepth());

  // Project out the payload on the success branch.  We can just use a
  // naked ValueComponent here; this is effectively a separate l-value.
  ManagedValue valueAddr =
    getAddressOfOptionalValue(SGF, e, optAddr, valueTypeData);
  LValue valueLV;
  valueLV.add<ValueComponent>(valueAddr, None, valueTypeData);
  return valueLV;
}

LValue SILGenLValue::visitInOutExpr(InOutExpr *e, SGFAccessKind accessKind,
                                    LValueOptions options) {
  return visitRec(e->getSubExpr(), accessKind, options);
}

/// Emit an lvalue that refers to the given property.  This is
/// designed to work with ManagedValue 'base's that are either +0 or +1.
LValue SILGenFunction::emitPropertyLValue(SILLocation loc, ManagedValue base,
                                          CanType baseFormalType,
                                          VarDecl *ivar,
                                          LValueOptions options,
                                          SGFAccessKind accessKind,
                                          AccessSemantics semantics) {
  SILGenLValue sgl(*this);
  LValue lv;

  auto baseType = base.getType().getASTType();
  auto subMap = baseType->getContextSubstitutionMap(
      SGM.M.getSwiftModule(), ivar->getDeclContext());

  AccessStrategy strategy =
    ivar->getAccessStrategy(semantics,
                            getFormalAccessKind(accessKind), FunctionDC);

  auto baseAccessKind =
    getBaseAccessKind(SGM, ivar, accessKind, strategy, baseFormalType);

  LValueTypeData baseTypeData =
    getValueTypeData(baseAccessKind, baseFormalType, base.getValue());

  // Refer to 'self' as the base of the lvalue.
  lv.add<ValueComponent>(base, None, baseTypeData,
                         /*isRValue=*/!base.isLValue());

  auto substFormalType = ivar->getInterfaceType().subst(subMap)
    ->getCanonicalType().getReferenceStorageReferent();

  lv.addMemberVarComponent(*this, loc, ivar, subMap, options, /*super*/ false,
                           accessKind, strategy, substFormalType);
  return lv;
}

// This is emitLoad that will handle re-abstraction and bridging for the client.
ManagedValue SILGenFunction::emitLoad(SILLocation loc, SILValue addr,
                                      AbstractionPattern origFormalType,
                                      CanType substFormalType,
                                      const TypeLowering &rvalueTL,
                                      SGFContext C, IsTake_t isTake,
                                      bool isAddressGuaranteed) {
  assert(addr->getType().isAddress());
  SILType addrRValueType = addr->getType().getReferenceStorageReferentType();

  // Fast path: the types match exactly.
  if (addrRValueType == rvalueTL.getLoweredType().getAddressType()) {
    return emitLoad(loc, addr, rvalueTL, C, isTake, isAddressGuaranteed);
  }

  // Otherwise, we need to reabstract or bridge.
  auto conversion =
    origFormalType.isClangType()
      ? Conversion::getBridging(Conversion::BridgeFromObjC,
                                origFormalType.getType(),
                                substFormalType, rvalueTL.getLoweredType())
      : Conversion::getOrigToSubst(origFormalType, substFormalType);

  return emitConvertedRValue(loc, conversion, C,
      [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
    return SGF.emitLoad(loc, addr, getTypeLowering(addrRValueType),
                        C, isTake, isAddressGuaranteed);
  });
}

/// Load an r-value out of the given address.
///
/// \param rvalueTL - the type lowering for the type-of-rvalue
///   of the address
/// \param isAddrGuaranteed - true if the value in this address
///   is guaranteed to be valid for the duration of the current
///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
ManagedValue SILGenFunction::emitLoad(SILLocation loc, SILValue addr,
                                      const TypeLowering &rvalueTL,
                                      SGFContext C, IsTake_t isTake,
                                      bool isAddrGuaranteed) {
  // Get the lowering for the address type.  We can avoid a re-lookup
  // in the very common case of this being equivalent to the r-value
  // type.
  auto &addrTL =
    (addr->getType() == rvalueTL.getLoweredType().getAddressType()
       ? rvalueTL : getTypeLowering(addr->getType()));

  // Never do a +0 load together with a take.
  bool isPlusZeroOk = (isTake == IsNotTake &&
                       (isAddrGuaranteed ? C.isGuaranteedPlusZeroOk()
                                          : C.isImmediatePlusZeroOk()));

  if (rvalueTL.isAddressOnly() && silConv.useLoweredAddresses()) {
    // If the client is cool with a +0 rvalue, the decl has an address-only
    // type, and there are no conversions, then we can return this as a +0
    // address RValue.
    if (isPlusZeroOk && rvalueTL.getLoweredType() == addrTL.getLoweredType())
      return ManagedValue::forUnmanaged(addr);

    // Copy the address-only value.
    return B.bufferForExpr(
        loc, rvalueTL.getLoweredType(), rvalueTL, C,
        [&](SILValue newAddr) {
          emitSemanticLoadInto(loc, addr, addrTL, newAddr, rvalueTL,
                               isTake, IsInitialization);
        });
  }

  // Ok, this is something loadable.  If this is a non-take access at plus zero,
  // we can perform a +0 load of the address instead of materializing a +1
  // value.
  if (isPlusZeroOk && addrTL.getLoweredType() == rvalueTL.getLoweredType()) {
    return B.createLoadBorrow(loc, ManagedValue::forUnmanaged(addr));
  }

  // Load the loadable value, and retain it if we aren't taking it.
  SILValue loadedV = emitSemanticLoad(loc, addr, addrTL, rvalueTL, isTake);
  return emitManagedRValueWithCleanup(loadedV);
}

/// Load an r-value out of the given address.
///
/// \param rvalueTL - the type lowering for the type-of-rvalue
///   of the address
/// \param isAddressGuaranteed - true if the value in this address
///   is guaranteed to be valid for the duration of the current
///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
ManagedValue SILGenFunction::emitFormalAccessLoad(SILLocation loc,
                                                  SILValue addr,
                                                  const TypeLowering &rvalueTL,
                                                  SGFContext C, IsTake_t isTake,
                                                  bool isAddressGuaranteed) {
  // Get the lowering for the address type.  We can avoid a re-lookup
  // in the very common case of this being equivalent to the r-value
  // type.
  auto &addrTL = (addr->getType() == rvalueTL.getLoweredType().getAddressType()
                      ? rvalueTL
                      : getTypeLowering(addr->getType()));

  // Never do a +0 load together with a take.
  bool isPlusZeroOk =
      (isTake == IsNotTake && (isAddressGuaranteed ? C.isGuaranteedPlusZeroOk()
                                                 : C.isImmediatePlusZeroOk()));

  if (rvalueTL.isAddressOnly() && silConv.useLoweredAddresses()) {
    // If the client is cool with a +0 rvalue, the decl has an address-only
    // type, and there are no conversions, then we can return this as a +0
    // address RValue.
    if (isPlusZeroOk && rvalueTL.getLoweredType() == addrTL.getLoweredType())
      return ManagedValue::forUnmanaged(addr);

    // Copy the address-only value.
    return B.formalAccessBufferForExpr(
        loc, rvalueTL.getLoweredType(), rvalueTL, C,
        [&](SILValue addressForCopy) {
          emitSemanticLoadInto(loc, addr, addrTL, addressForCopy, rvalueTL,
                               isTake, IsInitialization);
        });
  }

  // Ok, this is something loadable.  If this is a non-take access at plus zero,
  // we can perform a +0 load of the address instead of materializing a +1
  // value.
  if (isPlusZeroOk && addrTL.getLoweredType() == rvalueTL.getLoweredType()) {
    return B.createFormalAccessLoadBorrow(loc,
                                          ManagedValue::forUnmanaged(addr));
  }

  // Load the loadable value, and retain it if we aren't taking it.
  SILValue loadedV = emitSemanticLoad(loc, addr, addrTL, rvalueTL, isTake);
  return emitFormalAccessManagedRValueWithCleanup(loc, loadedV);
}

static void emitUnloweredStoreOfCopy(SILGenBuilder &B, SILLocation loc,
                                     SILValue value, SILValue addr,
                                     IsInitialization_t isInit) {
  if (isInit) {
    B.emitStoreValueOperation(loc, value, addr, StoreOwnershipQualifier::Init);
  } else {
    B.createAssign(loc, value, addr);
  }
}

SILValue SILGenFunction::emitConversionToSemanticRValue(SILLocation loc,
                                                        SILValue src,
                                                  const TypeLowering &valueTL) {
  auto storageType = src->getType();
  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    /* Address-only storage types are handled with their underlying type. */ \
    llvm_unreachable("address-only pointers are handled elsewhere");
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    return B.createCopy##Name##Value(loc, src);
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable reference storage types, we need to generate a strong */ \
    /* retain and strip the box. */ \
    assert(storageType.castTo<Name##StorageType>()->isLoadable( \
                                               ResilienceExpansion::Maximal)); \
    return B.createCopy##Name##Value(loc, src); \
  }
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For static reference storage types, we need to strip the box and */ \
    /* then do an (unsafe) retain. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    auto result = B.create##Name##ToRef(loc, src, \
              SILType::getPrimitiveObjectType(type.getReferentType())); \
    /* SEMANTIC ARC TODO: Does this need a cleanup? */ \
    return B.createCopyValue(loc, result); \
  }
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("impossible");
}

ManagedValue SILGenFunction::emitConversionToSemanticRValue(
    SILLocation loc, ManagedValue src, const TypeLowering &valueTL) {
  auto swiftStorageType = src.getType().castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    /* Address-only storage types are handled with their underlying type. */ \
    llvm_unreachable("address-only pointers are handled elsewhere");
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    /* Generate a strong retain and strip the box. */ \
    return B.createCopy##Name##Value(loc, src);
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    /* Strip the box and then do an (unsafe) retain. */ \
    return B.createUnsafeCopy##Name##Value(loc, src);
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("impossible");
}

/// Given that the type-of-rvalue differs from the type-of-storage,
/// and given that the type-of-rvalue is loadable, produce a +1 scalar
/// of the type-of-rvalue.
static SILValue emitLoadOfSemanticRValue(SILGenFunction &SGF,
                                         SILLocation loc,
                                         SILValue src,
                                         const TypeLowering &valueTL,
                                         IsTake_t isTake) {
  auto storageType = src->getType();
  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    return SGF.B.createLoad##Name(loc, src, isTake);
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  { \
    /* For loadable types, we need to strip the box. */ \
    /* If we are not performing a take, use a load_borrow. */ \
    if (!isTake) { \
      SILValue value = SGF.B.createLoadBorrow(loc, src); \
      SILValue strongValue = SGF.B.createCopy##Name##Value(loc, value); \
      SGF.B.createEndBorrow(loc, value, src); \
      return strongValue; \
    } \
    /* Otherwise perform a load take and destroy the stored value. */ \
    auto value = SGF.B.emitLoadValueOperation(loc, src, \
                                              LoadOwnershipQualifier::Take); \
    SILValue strongValue = SGF.B.createCopy##Name##Value(loc, value); \
    SGF.B.createDestroyValue(loc, value); \
    return strongValue; \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable types, we need to strip the box. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    if (!type->isLoadable(ResilienceExpansion::Maximal)) { \
      return SGF.B.createLoad##Name(loc, src, isTake); \
    } \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  }
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For static reference storage types, we need to strip the box. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    auto value = SGF.B.createLoad(loc, src, LoadOwnershipQualifier::Trivial); \
    auto result = SGF.B.create##Name##ToRef(loc, value, \
            SILType::getPrimitiveObjectType(type.getReferentType())); \
    /* SEMANTIC ARC TODO: Does this need a cleanup? */ \
    return SGF.B.createCopyValue(loc, result); \
  }
#include "swift/AST/ReferenceStorage.def"
#undef ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER
  }
  llvm_unreachable("unhandled ownership");
}

/// Given that the type-of-rvalue differs from the type-of-storage,
/// store a +1 value (possibly not a scalar) of the type-of-rvalue
/// into the given address.
static void emitStoreOfSemanticRValue(SILGenFunction &SGF,
                                      SILLocation loc,
                                      SILValue value,
                                      SILValue dest,
                                      const TypeLowering &valueTL,
                                      IsInitialization_t isInit) {
  auto storageType = dest->getType();
  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();

  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    SGF.B.createStore##Name(loc, value, dest, isInit); \
    /* store doesn't take ownership of the input, so cancel it out. */ \
    SGF.B.emitDestroyValueOperation(loc, value); \
    return; \
  }
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  { \
    auto typedValue = SGF.B.createRefTo##Name(loc, value, \
                                              storageType.getObjectType()); \
    auto copiedVal = SGF.B.createCopyValue(loc, typedValue); \
    emitUnloweredStoreOfCopy(SGF.B, loc, copiedVal, dest, isInit); \
    SGF.B.emitDestroyValueOperation(loc, value); \
    return; \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable types, we need to enter the box by */ \
    /* turning the strong retain into an type-specific retain. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    /* FIXME: resilience */ \
    if (!type->isLoadable(ResilienceExpansion::Maximal)) { \
      SGF.B.createStore##Name(loc, value, dest, isInit); \
      /* store doesn't take ownership of the input, so cancel it out. */ \
      SGF.B.emitDestroyValueOperation(loc, value); \
      return; \
    } \
    ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name) \
  }
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For static reference storage types, we need to enter the box and */ \
    /* release the strong retain. */ \
    auto typedValue = SGF.B.createRefTo##Name(loc, value, \
                                              storageType.getObjectType()); \
    emitUnloweredStoreOfCopy(SGF.B, loc, typedValue, dest, isInit); \
    SGF.B.emitDestroyValueOperation(loc, value); \
    return; \
  }
#include "swift/AST/ReferenceStorage.def"
#undef ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE_HELPER
  }
  llvm_unreachable("impossible");
}

/// Load a value of the type-of-rvalue out of the given address as a
/// scalar.  The type-of-rvalue must be loadable.
SILValue SILGenFunction::emitSemanticLoad(SILLocation loc,
                                          SILValue src,
                                          const TypeLowering &srcTL,
                                          const TypeLowering &rvalueTL,
                                          IsTake_t isTake) {
  assert(srcTL.getLoweredType().getAddressType() == src->getType());
  assert(rvalueTL.isLoadable() || !silConv.useLoweredAddresses());

  // Easy case: the types match.
  if (srcTL.getLoweredType() == rvalueTL.getLoweredType()) {
    return srcTL.emitLoadOfCopy(B, loc, src, isTake);
  }

  return emitLoadOfSemanticRValue(*this, loc, src, rvalueTL, isTake);
}

/// Load a value of the type-of-reference out of the given address
/// and into the destination address.
void SILGenFunction::emitSemanticLoadInto(SILLocation loc,
                                          SILValue src,
                                          const TypeLowering &srcTL,
                                          SILValue dest,
                                          const TypeLowering &destTL,
                                          IsTake_t isTake,
                                          IsInitialization_t isInit) {
  assert(srcTL.getLoweredType().getAddressType() == src->getType());
  assert(destTL.getLoweredType().getAddressType() == dest->getType());

  // Easy case: the types match.
  if (srcTL.getLoweredType() == destTL.getLoweredType()) {
    B.createCopyAddr(loc, src, dest, isTake, isInit);
    return;
  }

  auto rvalue = emitLoadOfSemanticRValue(*this, loc, src, srcTL, isTake);
  emitUnloweredStoreOfCopy(B, loc, rvalue, dest, isInit);
}

/// Store an r-value into the given address as an initialization.
void SILGenFunction::emitSemanticStore(SILLocation loc,
                                       SILValue rvalue,
                                       SILValue dest,
                                       const TypeLowering &destTL,
                                       IsInitialization_t isInit) {
  assert(destTL.getLoweredType().getAddressType() == dest->getType());

  // Easy case: the types match.
  if (rvalue->getType() == destTL.getLoweredType()) {
    assert(!silConv.useLoweredAddresses()
           || (destTL.isAddressOnly() == rvalue->getType().isAddress()));
    if (rvalue->getType().isAddress()) {
      B.createCopyAddr(loc, rvalue, dest, IsTake, isInit);
    } else {
      emitUnloweredStoreOfCopy(B, loc, rvalue, dest, isInit);
    }
    return;
  }

  auto &rvalueTL = getTypeLowering(rvalue->getType());
  emitStoreOfSemanticRValue(*this, loc, rvalue, dest, rvalueTL, isInit);
}

/// Convert a semantic rvalue to a value of storage type.
SILValue SILGenFunction::emitConversionFromSemanticValue(SILLocation loc,
                                                         SILValue semanticValue,
                                                         SILType storageType) {
  auto &destTL = getTypeLowering(storageType);
  (void)destTL;
  // Easy case: the types match.
  if (semanticValue->getType() == storageType) {
    return semanticValue;
  }

  auto swiftStorageType = storageType.castTo<ReferenceStorageType>();
  switch (swiftStorageType->getOwnership()) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong reference storage type should be impossible");
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    llvm_unreachable("address-only types are never loadable");
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    SILValue value = B.createRefTo##Name(loc, semanticValue, storageType); \
    value = B.createCopyValue(loc, value); \
    B.emitDestroyValueOperation(loc, semanticValue); \
    return value; \
  }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For loadable types, place into a box. */ \
    auto type = storageType.castTo<Name##StorageType>(); \
    assert(type->isLoadable(ResilienceExpansion::Maximal)); \
    (void) type; \
    SILValue value = B.createRefTo##Name(loc, semanticValue, storageType); \
    value = B.createCopyValue(loc, value); \
    B.emitDestroyValueOperation(loc, semanticValue); \
    return value; \
  }
#define UNCHECKED_REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: { \
    /* For static reference storage types, place into a box. */ \
    SILValue value = B.createRefTo##Name(loc, semanticValue, storageType); \
    B.emitDestroyValueOperation(loc, semanticValue); \
    return value; \
  }
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("impossible");
}

static void emitTsanInoutAccess(SILGenFunction &SGF, SILLocation loc,
                                ManagedValue address) {
  assert(address.getType().isAddress());
  SILValue accessFnArgs[] = {address.getValue()};

  SGF.B.createBuiltin(loc, SGF.getASTContext().getIdentifier("tsanInoutAccess"),
                      SGF.SGM.Types.getEmptyTupleType(), {}, accessFnArgs);
}

/// Produce a physical address that corresponds to the given l-value
/// component.
static ManagedValue drillIntoComponent(SILGenFunction &SGF,
                                       SILLocation loc,
                                       PathComponent &&component,
                                       ManagedValue base,
                                       TSanKind tsanKind) {
  bool isRValue = component.isRValue();
  ManagedValue addr = std::move(component).project(SGF, loc, base);

  if (!SGF.getASTContext().LangOpts.DisableTsanInoutInstrumentation &&
      (SGF.getModule().getOptions().Sanitizers & SanitizerKind::Thread) &&
      tsanKind == TSanKind::InoutAccess && !isRValue) {
    emitTsanInoutAccess(SGF, loc, addr);
  }

  return addr;
}

/// Find the last component of the given lvalue and derive a base
/// location for it.
static PathComponent &&
drillToLastComponent(SILGenFunction &SGF,
                     SILLocation loc,
                     LValue &&lv,
                     ManagedValue &addr,
                     TSanKind tsanKind = TSanKind::None) {
  assert(lv.begin() != lv.end() &&
         "lvalue must have at least one component");

  for (auto i = lv.begin(), e = lv.end() - 1; i != e; ++i) {
    addr = drillIntoComponent(SGF, loc, std::move(**i), addr, tsanKind);
  }

  return std::move(**(lv.end() - 1));
}

static ArgumentSource emitBaseValueForAccessor(SILGenFunction &SGF,
                                               SILLocation loc, LValue &&lvalue,
                                               CanType baseFormalType,
                                               SILDeclRef accessor) {
  ManagedValue base;
  PathComponent &&component =
    drillToLastComponent(SGF, loc, std::move(lvalue), base);
  base = drillIntoComponent(SGF, loc, std::move(component), base,
                            TSanKind::None);

  return SGF.prepareAccessorBaseArg(loc, base, baseFormalType, accessor);
}

RValue SILGenFunction::emitLoadOfLValue(SILLocation loc, LValue &&src,
                                        SGFContext C, bool isBaseGuaranteed) {
  assert(isReadAccess(src.getAccessKind()));

  // Any writebacks should be scoped to after the load.
  FormalEvaluationScope scope(*this);

  // We shouldn't need to re-abstract here, but we might have to bridge.
  // This should only happen if we have a global variable of NSString type.
  auto origFormalType = src.getOrigFormalType();
  auto substFormalType = src.getSubstFormalType();
  auto &rvalueTL = getTypeLowering(src.getTypeOfRValue());

  ManagedValue addr;
  PathComponent &&component =
    drillToLastComponent(*this, loc, std::move(src), addr);

  // If the last component is physical, drill down and load from it.
  if (component.isPhysical()) {
    auto projection = std::move(component).project(*this, loc, addr);
    if (projection.getType().isAddress()) {
      projection = emitLoad(loc, projection.getValue(),
                            origFormalType, substFormalType,
                            rvalueTL, C, IsNotTake,
                            isBaseGuaranteed);
    } else if (isReadAccessResultOwned(src.getAccessKind()) &&
               !projection.isPlusOne(*this)) {
      projection = projection.copy(*this, loc);
    }

    return RValue(*this, loc, substFormalType, projection);
  }

  // If the last component is logical, emit a get.
  return std::move(component.asLogical()).get(*this, loc, addr, C);
}

ManagedValue SILGenFunction::emitAddressOfLValue(SILLocation loc,
                                                 LValue &&src,
                                                 TSanKind tsanKind) {
  // Write is only included in this list because of property behaviors.
  // It's obviously unreasonable, though.
  assert(src.getAccessKind() == SGFAccessKind::IgnoredRead ||
         src.getAccessKind() == SGFAccessKind::BorrowedAddressRead ||
         src.getAccessKind() == SGFAccessKind::OwnedAddressRead ||
         src.getAccessKind() == SGFAccessKind::ReadWrite ||
         src.getAccessKind() == SGFAccessKind::Write);

  ManagedValue addr;
  PathComponent &&component =
    drillToLastComponent(*this, loc, std::move(src), addr, tsanKind);

  addr = drillIntoComponent(*this, loc, std::move(component), addr, tsanKind);
  assert(addr.getType().isAddress() &&
         "resolving lvalue did not give an address");
  return ManagedValue::forLValue(addr.getValue());
}

LValue
SILGenFunction::emitOpenExistentialLValue(SILLocation loc,
                                          LValue &&lv,
                                          CanArchetypeType openedArchetype,
                                          CanType formalRValueType,
                                          SGFAccessKind accessKind) {
  assert(!formalRValueType->hasLValueType());
  LValueTypeData typeData = {
    accessKind, AbstractionPattern::getOpaque(), formalRValueType,
    getLoweredType(formalRValueType).getObjectType()
  };

  // Open up the existential.
  auto rep = lv.getTypeOfRValue()
    .getPreferredExistentialRepresentation(SGM.M);
  switch (rep) {
  case ExistentialRepresentation::Opaque:
  case ExistentialRepresentation::Boxed: {
    lv.add<OpenOpaqueExistentialComponent>(openedArchetype, typeData);
    break;
  }
  case ExistentialRepresentation::Metatype:
  case ExistentialRepresentation::Class: {
    lv.add<OpenNonOpaqueExistentialComponent>(openedArchetype, typeData);
    break;
  }
  case ExistentialRepresentation::None:
    llvm_unreachable("cannot open non-existential");
  }

  return std::move(lv);
}

static bool trySetterPeephole(SILGenFunction &SGF, SILLocation loc,
                              ArgumentSource &&src, LValue &&dest) {
  // The last component must be a getter/setter.
  // TODO: allow reabstraction here, too.
  auto &component = **(dest.end() - 1);
  if (component.getKind() != PathComponent::GetterSetterKind)
    return false;

  // We cannot apply the peephole if the l-value includes an
  // open-existential component because we need to make sure that
  // the opened archetype is available everywhere during emission.
  // TODO: should we instead just immediately open the existential
  // during emitLValue and simply leave the opened address in the LValue?
  // Or is there some reasonable way to detect that this is happening
  // and avoid affecting cases where it is not necessary?
  for (auto &componentPtr : dest) {
    if (componentPtr->isOpenExistential())
      return false;
  }

  auto &setterComponent = static_cast<GetterSetterComponent&>(component);
  setterComponent.emitAssignWithSetter(SGF, loc, std::move(dest),
                                       std::move(src));
  return true;;
}

void SILGenFunction::emitAssignToLValue(SILLocation loc, RValue &&src,
                                        LValue &&dest) {
  emitAssignToLValue(loc, ArgumentSource(loc, std::move(src)), std::move(dest));
}

void SILGenFunction::emitAssignToLValue(SILLocation loc,
                                        ArgumentSource &&src,
                                        LValue &&dest) {
  assert(dest.getAccessKind() == SGFAccessKind::Write);

  // Enter a FormalEvaluationScope so that formal access to independent LValue
  // components do not overlap. Furthermore, use an ArgumentScope to force
  // cleanup of materialized LValues immediately, before evaluating the next
  // LValue. For example: (x[i], x[j]) = a, b
  ArgumentScope argScope(*this, loc);

  // If the last component is a getter/setter component, use a special
  // generation pattern that allows us to peephole the emission of the RHS.
  if (trySetterPeephole(*this, loc, std::move(src), std::move(dest))) {
    argScope.pop();
    return;
  }

  // Otherwise, force the RHS now to preserve evaluation order.
  auto srcLoc = src.getLocation();
  RValue srcValue = std::move(src).getAsRValue(*this);

  // Peephole: instead of materializing and then assigning into a
  // translation component, untransform the value first.
  while (dest.isLastComponentTranslation()) {
    srcValue = std::move(dest.getLastTranslationComponent())
                 .untranslate(*this, loc, std::move(srcValue));
    dest.dropLastTranslationComponent();
  }

  src = ArgumentSource(srcLoc, std::move(srcValue));

  // Resolve all components up to the last, keeping track of value-type logical
  // properties we need to write back to.
  ManagedValue destAddr;
  PathComponent &&component =
    drillToLastComponent(*this, loc, std::move(dest), destAddr);
  
  // Write to the tail component.
  if (component.isPhysical()) {
    auto finalDestAddr =
      std::move(component).project(*this, loc, destAddr);
    assert(finalDestAddr.getType().isAddress());

    auto value = std::move(src).getAsRValue(*this).ensurePlusOne(*this, loc);
    std::move(value).assignInto(*this, loc, finalDestAddr.getValue());
  } else {
    std::move(component.asLogical()).set(*this, loc, std::move(src), destAddr);
  }

  // The writeback scope closing will propagate the value back up through the
  // writeback chain.
  argScope.pop();
}

void SILGenFunction::emitCopyLValueInto(SILLocation loc, LValue &&src,
                                        Initialization *dest) {
  auto skipPeephole = [&]{
    auto loaded = emitLoadOfLValue(loc, std::move(src), SGFContext(dest));
    if (!loaded.isInContext())
      std::move(loaded).forwardInto(*this, loc, dest);
  };
  
  // If the source is a physical lvalue, the destination is a single address,
  // and there's no semantic conversion necessary, do a copy_addr from the
  // lvalue into the destination.
  if (!src.isPhysical())
    return skipPeephole();
  if (!dest->canPerformInPlaceInitialization())
    return skipPeephole();
  auto destAddr = dest->getAddressForInPlaceInitialization(*this, loc);
  assert(src.getTypeOfRValue().getASTType()
           == destAddr->getType().getASTType());

  auto srcAddr = emitAddressOfLValue(loc, std::move(src)).getUnmanagedValue();

  UnenforcedAccess access;
  SILValue accessAddress =
    access.beginAccess(*this, loc, destAddr, SILAccessKind::Modify);
  B.createCopyAddr(loc, srcAddr, accessAddress, IsNotTake, IsInitialization);
  access.endAccess(*this);

  dest->finishInitialization(*this);
}

void SILGenFunction::emitAssignLValueToLValue(SILLocation loc, LValue &&src,
                                              LValue &&dest) {
  // Only perform the peephole if both operands are physical, there's no
  // semantic conversion necessary, and exclusivity enforcement
  // is not enabled. The peephole interferes with exclusivity enforcement
  // because it causes the formal accesses to the source and destination to
  // overlap.
  bool peepholeConflict =
      !src.isObviouslyNonConflicting(dest, SGFAccessKind::OwnedObjectRead,
                                     SGFAccessKind::Write);

  if (peepholeConflict || !src.isPhysical() || !dest.isPhysical()) {
    RValue loaded = emitLoadOfLValue(loc, std::move(src), SGFContext());
    emitAssignToLValue(loc, std::move(loaded), std::move(dest));
    return;
  }

  auto &rvalueTL = getTypeLowering(src.getTypeOfRValue());

  auto srcAddr = emitAddressOfLValue(loc, std::move(src)).getUnmanagedValue();
  auto destAddr = emitAddressOfLValue(loc, std::move(dest)).getUnmanagedValue();

  if (srcAddr->getType() == destAddr->getType()) {
    B.createCopyAddr(loc, srcAddr, destAddr, IsNotTake, IsNotInitialization);
  } else {
    // If there's a semantic conversion necessary, do a load then assign.
    auto loaded = emitLoad(loc, srcAddr, rvalueTL, SGFContext(), IsNotTake);
    loaded.assignInto(*this, loc, destAddr);
  }
}

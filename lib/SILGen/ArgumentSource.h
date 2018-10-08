//===--- ArgumentSource.h - Abstracted source of an argument ----*- C++ -*-===//
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
// A structure for holding an abstracted source of a call argument.
// It's either:
//
//  - an expression, yielding an l-value or r-value
//  - an RValue
//  - an LValue
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_ARGUMENTSOURCE_H
#define SWIFT_LOWERING_ARGUMENTSOURCE_H

#include "swift/Basic/ExternalUnion.h"
#include "RValue.h"
#include "LValue.h"

namespace swift {
namespace Lowering {
class Conversion;

/// A means of generating an argument.
///
/// This is useful as a way to pass values around without either:
///   - requiring them to have already been evaluated or
///   - requiring them to come from an identifiable expression.
///
/// Being able to propagate values is important because there are a
/// number of cases (involving, say, property accessors) where values
/// are implicitly or previously generated.  However, being able to
/// propagate Expr*s is also important because there are several kinds
/// of expressions (such as closures) which can be emitted more
/// efficiently with a known target abstraction level.
///
/// Because an ArgumentSource might contain an unevaluated expression,
/// care must be taken when dealing with multiple ArgumentSources to
/// preserve the original evaluation order of the program.  APIs
/// working with multiple ArgumentSources should document the order in
/// which they plan to evaluate them.
class ArgumentSource {
  enum class Kind : uint8_t {
    Invalid,
    RValue,
    LValue,
    Expr,
  };

  struct RValueStorage {
    RValue Value;
    SILLocation Loc;
  };
  struct LValueStorage {
    LValue Value;
    SILLocation Loc;
  };

  using StorageMembers =
    ExternalUnionMembers<void, RValueStorage, LValueStorage, Expr*>;

  static StorageMembers::Index getStorageIndexForKind(Kind kind) {
    switch (kind) {
    case Kind::Invalid: return StorageMembers::indexOf<void>();
    case Kind::RValue:
      return StorageMembers::indexOf<RValueStorage>();
    case Kind::LValue: return StorageMembers::indexOf<LValueStorage>();
    case Kind::Expr: return StorageMembers::indexOf<Expr*>();
    }
    llvm_unreachable("bad kind");
  }

  ExternalUnion<Kind, StorageMembers, getStorageIndexForKind> Storage;
  Kind StoredKind;

public:
  ArgumentSource() : StoredKind(Kind::Invalid) {}
  ArgumentSource(SILLocation loc, RValue &&value) : StoredKind(Kind::RValue) {
    Storage.emplaceAggregate<RValueStorage>(StoredKind, std::move(value), loc);
  }
  ArgumentSource(SILLocation loc, LValue &&value) : StoredKind(Kind::LValue) {
    Storage.emplaceAggregate<LValueStorage>(StoredKind, std::move(value), loc);
  }
  ArgumentSource(Expr *e) : StoredKind(Kind::Expr) {
    assert(e && "initializing ArgumentSource with null expression");
    Storage.emplace<Expr*>(StoredKind, e);
  }
  // Cannot be copied.
  ArgumentSource(const ArgumentSource &other) = delete;
  ArgumentSource &operator=(const ArgumentSource &other) = delete;

  // Can be moved.
  ArgumentSource(ArgumentSource &&other) : StoredKind(other.StoredKind) {
    Storage.moveConstruct(StoredKind, std::move(other.Storage));
  }

  ArgumentSource &operator=(ArgumentSource &&other) {
    Storage.moveAssign(StoredKind, other.StoredKind, std::move(other.Storage));
    StoredKind = other.StoredKind;
    other.Storage.destruct(other.StoredKind);
    other.StoredKind = Kind::Invalid;
    return *this;
  }

  ~ArgumentSource() {
    Storage.destruct(StoredKind);
  }

  explicit operator bool() const & {
    switch (StoredKind) {
    case Kind::Invalid:
      return false;
    case Kind::RValue:
      return !asKnownRValue().isNull();
    case Kind::LValue:
      return asKnownLValue().isValid();
    case Kind::Expr:
      return asKnownExpr() != nullptr;
    }
    llvm_unreachable("bad kind");
  }

  CanType getSubstRValueType() const & {
    switch (StoredKind) {
    case Kind::Invalid:
      llvm_unreachable("argument source is invalid");
    case Kind::RValue:
      return asKnownRValue().getType();
    case Kind::LValue:
      return asKnownLValue().getSubstFormalType();
    case Kind::Expr:
      return asKnownExpr()->getType()->getInOutObjectType()->getCanonicalType();
    }
    llvm_unreachable("bad kind");
  }

  bool hasLValueType() const & {
    switch (StoredKind) {
    case Kind::Invalid: llvm_unreachable("argument source is invalid");
    case Kind::RValue:
      return false;
    case Kind::LValue: return true;
    case Kind::Expr: return asKnownExpr()->isSemanticallyInOutExpr();
    }
    llvm_unreachable("bad kind");    
  }

  SILLocation getLocation() const & {
    switch (StoredKind) {
    case Kind::Invalid:
      llvm_unreachable("argument source is invalid");
    case Kind::RValue:
      return getKnownRValueLocation();
    case Kind::LValue:
      return getKnownLValueLocation();
    case Kind::Expr:
      return asKnownExpr();
    }
    llvm_unreachable("bad kind");
  }

  bool isExpr() const & { return StoredKind == Kind::Expr; }
  bool isRValue() const & { return StoredKind == Kind::RValue; }
  bool isLValue() const & { return StoredKind == Kind::LValue; }

  /// Given that this source is storing an RValue, extract and clear
  /// that value.
  RValue &&asKnownRValue(SILGenFunction &SGF) && {
    return std::move(Storage.get<RValueStorage>(StoredKind).Value);
  }
  const RValue &asKnownRValue() const & {
    return Storage.get<RValueStorage>(StoredKind).Value;
  }
  SILLocation getKnownRValueLocation() const & {
    return Storage.get<RValueStorage>(StoredKind).Loc;
  }

  /// Given that this source is storing an LValue, extract and clear
  /// that value.
  LValue &&asKnownLValue() && {
    return std::move(Storage.get<LValueStorage>(StoredKind).Value);
  }
  const LValue &asKnownLValue() const & {
    return Storage.get<LValueStorage>(StoredKind).Value;
  }
  SILLocation getKnownLValueLocation() const & {
    return Storage.get<LValueStorage>(StoredKind).Loc;
  }

  /// Given that this source is an expression, extract and clear
  /// that expression.
  Expr *asKnownExpr() && {
    Expr *result = Storage.get<Expr*>(StoredKind);
    Storage.resetToEmpty<Expr*>(StoredKind, Kind::Invalid);
    StoredKind = Kind::Invalid;
    return result;
  }

  /// Return an unowned handle to the r-value stored in this source. Undefined
  /// if this ArgumentSource is not an rvalue.
  RValue &peekRValue() &;

  RValue getAsRValue(SILGenFunction &SGF, SGFContext C = SGFContext()) &&;
  ManagedValue getAsSingleValue(SILGenFunction &SGF,
                                SGFContext C = SGFContext()) &&;
  ManagedValue getAsSingleValue(SILGenFunction &SGF,
                                AbstractionPattern origFormalType,
                                SGFContext C = SGFContext()) &&;

  ManagedValue getConverted(SILGenFunction &SGF, const Conversion &conversion,
                            SGFContext C = SGFContext()) &&;

  void forwardInto(SILGenFunction &SGF, Initialization *dest) &&;
  void forwardInto(SILGenFunction &SGF, AbstractionPattern origFormalType,
                   Initialization *dest, const TypeLowering &destTL) &&;

  /// If we have an rvalue, borrow the rvalue into a new ArgumentSource and
  /// return the ArgumentSource. Otherwise, assert.
  ArgumentSource borrow(SILGenFunction &SGF) const &;

  ManagedValue materialize(SILGenFunction &SGF) &&;

  /// Emit this value to memory so that it follows the abstraction
  /// patterns of the original formal type.
  ///
  /// \param expectedType - the lowering of getSubstRValueType() under the
  ///   abstractions of origFormalType
  ManagedValue materialize(SILGenFunction &SGF,
                           AbstractionPattern origFormalType,
                           SILType expectedType = SILType()) &&;

  /// Whether this argument source is a TupleShuffleExpr.
  bool isShuffle() const;

  bool isObviouslyEqual(const ArgumentSource &other) const;

  ArgumentSource copyForDiagnostics() const;

  void dump() const;
  void dump(raw_ostream &os, unsigned indent = 0) const;

private:
  /// Private helper constructor for delayed borrowed rvalues.
  ArgumentSource(SILLocation loc, RValue &&rv, Kind kind);

  // Make this non-move accessor private to make it more difficult
  // to accidentally re-emit values.
  Expr *asKnownExpr() const & {
    return Storage.get<Expr*>(StoredKind);
  }
};

class PreparedArguments {
  SmallVector<AnyFunctionType::Param, 8> Params;
  std::vector<ArgumentSource> Arguments;
  unsigned IsScalar : 1;
  unsigned IsNull : 1;
public:
  PreparedArguments() : IsScalar(false), IsNull(true) {}
  PreparedArguments(ArrayRef<AnyFunctionType::Param> params, bool isScalar)
      : IsNull(true) {
    emplace(params, isScalar);
  }

  // Move-only.
  PreparedArguments(const PreparedArguments &) = delete;
  PreparedArguments &operator=(const PreparedArguments &) = delete;

  PreparedArguments(PreparedArguments &&other)
    : Params(std::move(other.Params)), Arguments(std::move(other.Arguments)),
      IsScalar(other.IsScalar), IsNull(other.IsNull) {}
  PreparedArguments &operator=(PreparedArguments &&other) {
    Params = std::move(other.Params);
    IsScalar = other.IsScalar;
    Arguments = std::move(other.Arguments);
    IsNull = other.IsNull;
    other.IsNull = true;
    return *this;
  }

  /// Returns true if this is a null argument list.  Note that this always
  /// indicates the total absence of an argument list rather than the
  /// possible presence of an empty argument list.
  bool isNull() const { return IsNull; }

  /// Returns true if this is a non-null and completed argument list.
  bool isValid() const {
    assert(!isNull());
    if (IsScalar)
      return Arguments.size() == 1;
    return Arguments.size() == Params.size();
  }

  /// Return the formal type of this argument list.
  ArrayRef<AnyFunctionType::Param> getParams() const {
    assert(!isNull());
    return Params;
  }

  /// Is this a single-argument list?  Note that the argument might be a tuple.
  bool isScalar() const {
    assert(!isNull());
    return IsScalar;
  }

  MutableArrayRef<ArgumentSource> getSources() && {
    assert(isValid());
    return Arguments;
  }

  /// Emplace a (probably incomplete) argument list.
  void emplace(ArrayRef<AnyFunctionType::Param> params, bool isScalar) {
    assert(isNull());
    Params.append(params.begin(), params.end());
    IsScalar = isScalar;
    IsNull = false;
  }

  /// Emplace an empty argument list.
  void emplaceEmptyArgumentList(SILGenFunction &SGF);

  /// Add an emitted r-value argument to this argument list.
  void add(SILLocation loc, RValue &&arg) {
    assert(!isNull());
    Arguments.emplace_back(loc, std::move(arg));
  }

  /// Add an arbitrary argument source to these arguments.
  ///
  /// An argument list with an arbtrary argument source can't generally
  /// be copied.
  void addArbitrary(ArgumentSource &&arg) {
    assert(!isNull());
    Arguments.emplace_back(std::move(arg));
  }

  /// Copy these prepared arguments.  This propagates null.
  PreparedArguments copy(SILGenFunction &SGF, SILLocation loc) const;

  bool isObviouslyEqual(const PreparedArguments &other) const;

  PreparedArguments copyForDiagnostics() const;
};

} // end namespace Lowering
} // end namespace swift

#endif

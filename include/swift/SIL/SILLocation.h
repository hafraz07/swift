//===--- SILLocation.h - Location information for SIL nodes -----*- C++ -*-===//
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

#ifndef SWIFT_SIL_LOCATION_H
#define SWIFT_SIL_LOCATION_H

#include "llvm/ADT/PointerUnion.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/SourceManager.h"
#include "swift/AST/TypeAlignments.h"

#include <cstddef>
#include <type_traits>

namespace swift {

class SourceLoc;
class ReturnStmt;
class BraceStmt;
class AbstractClosureExpr;
class AbstractFunctionDecl;

/// This is a pointer to the AST node that a SIL instruction was
/// derived from. This may be null if AST information is unavailable or
/// stripped.
///
/// FIXME: This should eventually include inlining history, generics
/// instantiation info, etc (when we get to it).
///
class SILLocation {
private:
  template <class T, class Enable = void>
  struct base_type;

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Decl, T>::value>::type> {
    using type = Decl;
  };

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Expr, T>::value>::type> {
    using type = Expr;
  };

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Stmt, T>::value>::type> {
    using type = Stmt;
  };

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Pattern, T>::value>::type> {
    using type = Pattern;
  };

  using ASTNodeTy = llvm::PointerUnion<Stmt *, Expr *, Decl *, Pattern *>;

public:
  enum LocationKind : unsigned {
    RegularKind = 1,
    ReturnKind = 2,
    ImplicitReturnKind = 3,
    InlinedKind = 4,
    MandatoryInlinedKind = 5,
    CleanupKind = 6,
    ArtificialUnreachableKind = 7
  };

  enum StorageKind : unsigned {
    UnknownKind = 0,
    ASTNodeKind = 1 << 3,
    SILFileKind = 1 << 4,
    DebugInfoKind = 1 << 3 | 1 << 4
  };

  struct DebugLoc {
    unsigned Line;
    unsigned Column;
    StringRef Filename;

    DebugLoc(unsigned Line = 0, unsigned Column = 0,
             StringRef Filename = StringRef())
        : Line(Line), Column(Column), Filename(Filename) {}

    inline bool operator==(const DebugLoc &R) const {
      return Line == R.Line && Column == R.Column &&
             Filename.equals(R.Filename);
    }
  };

protected:
  union UnderlyingLocation {
    UnderlyingLocation() : DebugInfoLoc({}) {}
    UnderlyingLocation(ASTNodeTy N) : ASTNode(N) {}
    UnderlyingLocation(SourceLoc L) : SILFileLoc(L) {}
    UnderlyingLocation(DebugLoc D)
        : DebugInfoLoc({D.Filename.data(), D.Line, D.Column}) {}
    struct ASTNodeLoc {
      ASTNodeLoc(ASTNodeTy N) : Primary(N) {}
      /// Primary AST location, always used for diagnostics.
      ASTNodeTy Primary;
      /// Sometimes the location for diagnostics needs to be
      /// different than the one used to emit the line table. If
      /// HasDebugLoc is set, this is used for the debug info.
      ASTNodeTy ForDebugger;
    } ASTNode;

    /// A location inside a textual .sil file.
    SourceLoc SILFileLoc;

    /// A deserialized source location.
    ///
    /// This represents \e most of the information in a SILLocation::DebugLoc,
    /// but for bit-packing purposes the length of the filename is stored in
    /// the SILLocation's ExtraStorageData instead of here.
    ///
    /// \sa SILLocation::getDebugInfoLoc
    struct CompactDebugLoc {
      const char *FilenameData;
      unsigned Line;
      unsigned Column;
    } DebugInfoLoc;
  } Loc;

  /// The kind of this SIL location.
  unsigned KindData;

  /// Extra data that's not in UnderlyingLocation to keep the size of
  /// SILLocation down.
  unsigned ExtraStorageData = 0;

  enum {
    LocationKindBits = 3,
    LocationKindMask = 7,

    StorageKindBits = 2,
    StorageKindMask = (1 << 3) | (1 << 4),
    SpecialFlagsMask = ~ (LocationKindMask | StorageKindMask),

    /// Used to mark this instruction as part of auto-generated
    /// code block.
    AutoGeneratedBit = 5,

    /// Used to redefine the default source location used to
    /// represent this SILLocation. For example, when the host instruction
    /// is known to correspond to the beginning or the end of the source
    /// range of the ASTNode.
    PointsToStartBit = 6,
    PointsToEndBit = 7,

    /// Used to notify that this instruction belongs to the top-
    /// level (module) scope.
    ///
    /// FIXME: If Module becomes a Decl, this could be removed.
    IsInTopLevel = 8,

    /// Marks this instruction as belonging to the function prologue.
    IsInPrologue = 9
  };

  template <typename T>
  T *getNodeAs(ASTNodeTy Node) const {
    using base = typename base_type<T>::type*;
    return dyn_cast_or_null<T>(Node.dyn_cast<base>());
  }

  template <typename T>
  bool isNode(ASTNodeTy Node) const {
    assert(isASTNode());
    if (Loc.ASTNode.Primary.is<typename base_type<T>::type*>())
      return isa<T>(Node.get<typename base_type<T>::type*>());
    return false;
  }

  template <typename T>
  T *castNodeTo(ASTNodeTy Node) const {
    return cast<T>(Node.get<typename base_type<T>::type*>());
  }

  /// \defgroup SILLocation constructors.
  /// @{

  /// This constructor is used to support getAs operation.
  SILLocation() { assert(Loc.DebugInfoLoc.Line == 0); }
  SILLocation(LocationKind K, unsigned Flags = 0) : KindData(K | Flags) {
    assert(Loc.DebugInfoLoc.Line == 0);
  }

  SILLocation(Stmt *S, LocationKind K, unsigned Flags = 0)
      : Loc(S), KindData(K | Flags) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }

  SILLocation(Expr *E, LocationKind K, unsigned Flags = 0)
      : Loc(E), KindData(K | Flags) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }

  SILLocation(Decl *D, LocationKind K, unsigned Flags = 0)
      : Loc(D), KindData(K | Flags) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }

  SILLocation(Pattern *P, LocationKind K, unsigned Flags = 0)
      : Loc(P), KindData(K | Flags) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }

  SILLocation(SourceLoc L, LocationKind K, unsigned Flags = 0)
      : Loc(L), KindData(K | Flags) {
    setStorageKind(SILFileKind);
    assert(isSILFile());
  }

  SILLocation(DebugLoc L, LocationKind K, unsigned Flags = 0)
      : KindData(K | Flags) {
    setDebugInfoLoc(L);
  }
  /// @}

private:
  friend class ImplicitReturnLocation;
  friend class MandatoryInlinedLocation;
  friend class InlinedLocation;
  friend class CleanupLocation;

  void setLocationKind(LocationKind K) { KindData |= (K & LocationKindMask); }
  void setStorageKind(StorageKind K) { KindData |= (K & StorageKindMask); }
  unsigned getSpecialFlags() const { return KindData & SpecialFlagsMask; }
  void setSpecialFlags(unsigned Flags) {
    KindData |= (Flags & SpecialFlagsMask);
  }

  SourceLoc getSourceLoc(ASTNodeTy N) const;
  SourceLoc getStartSourceLoc(ASTNodeTy N) const;
  SourceLoc getEndSourceLoc(ASTNodeTy N) const;

public:

  /// When an ASTNode gets implicitly converted into a SILLocation we
  /// construct a RegularLocation. Since RegularLocations represent the majority
  /// of locations, this greatly simplifies the user code.
  SILLocation(Stmt *S) : Loc(S), KindData(RegularKind) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }
  SILLocation(Expr *E) : Loc(E), KindData(RegularKind) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }
  SILLocation(Decl *D) : Loc(D), KindData(RegularKind) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }
  SILLocation(Pattern *P) : Loc(P), KindData(RegularKind) {
    setStorageKind(ASTNodeKind);
    assert(isASTNode());
  }

  static SILLocation invalid() { return SILLocation(); }

  /// Check if the location wraps an AST node or a valid SIL file
  /// location.
  ///
  /// Artificial locations and the top-level module locations will be null.
  bool isNull() const {
    switch (getStorageKind()) {
    case ASTNodeKind:   return Loc.ASTNode.Primary.isNull();
    case DebugInfoKind: return ExtraStorageData == 0;
    case SILFileKind:   return Loc.SILFileLoc.isInvalid();
    default:            return true;
    }
  }
  explicit operator bool() const { return !isNull(); }

  /// Return whether this location is backed by an AST node.
  bool isASTNode() const { return getStorageKind() == ASTNodeKind; }

  /// Return whether this location came from a SIL file.
  bool isSILFile() const { return getStorageKind() == SILFileKind; }

  /// Return whether this location came from a textual SIL file.
  bool isDebugInfoLoc() const { return getStorageKind() == DebugInfoKind; }

  /// Marks the location as coming from auto-generated body.
  void markAutoGenerated() { KindData |= (1 << AutoGeneratedBit); }

  /// Returns true if the location represents an artificially generated
  /// body, such as thunks or default destructors.
  ///
  /// These locations should not be included in the debug line table.
  /// These might also need special handling by the debugger since they might
  /// contain calls, which the debugger could be able to step into.
  bool isAutoGenerated() const { return KindData & (1 << AutoGeneratedBit); }

  /// Returns true if the line number of this location is zero.
  bool isLineZero(const SourceManager &SM) const {
    return decodeDebugLoc(SM).Line == 0;
  }

  /// Changes the default source location position to point to start of
  /// the AST node.
  void pointToStart() { KindData |= (1 << PointsToStartBit); }

  /// Changes the default source location position to point to the end of
  /// the AST node.
  void pointToEnd() { KindData |= (1 << PointsToEndBit); }

  /// Mark this location as the location corresponding to the top-level
  /// (module-level) code.
  void markAsInTopLevel() { KindData |= (1 << IsInTopLevel); }

  /// Check is this location is associated with the top level/module.
  bool isInTopLevel() const { return KindData & (1 << IsInTopLevel); }

  /// Mark this location as being part of the function
  /// prologue, which means that it deals with setting up the stack
  /// frame. The first breakpoint location in a function is at the end
  /// of the prologue.
  void markAsPrologue() { KindData |= (1 << IsInPrologue); }

  /// Check is this location is part of a function's implicit prologue.
  bool isInPrologue() const { return KindData & (1 << IsInPrologue); }

  /// Add an ASTNode to use as the location for debugging
  /// purposes if this location is different from the location used
  /// for diagnostics.
  template <typename T>
  void setDebugLoc(T *ASTNodeForDebugging) {
    assert(!hasDebugLoc() && "DebugLoc already present");
    assert(isASTNode() && "not an AST location");
    Loc.ASTNode.ForDebugger = ASTNodeForDebugging;
  }
  bool hasDebugLoc() const {
    return isASTNode() && !Loc.ASTNode.ForDebugger.isNull();
  }

  /// Populate this empty SILLocation with a DebugLoc.
  void setDebugInfoLoc(DebugLoc L) {
    ExtraStorageData = L.Filename.size();
    assert(ExtraStorageData == L.Filename.size() &&
           "file name is longer than 32 bits");
    Loc = L;
    setStorageKind(DebugInfoKind);
  }

  /// Check if the corresponding source code location definitely points
  ///  to the start of the AST node.
  bool alwaysPointsToStart() const { return KindData & (1 << PointsToStartBit);}

  /// Check if the corresponding source code location definitely points
  ///  to the end of the AST node.
  bool alwaysPointsToEnd() const { return KindData & (1 << PointsToEndBit); }

  LocationKind getKind() const {
    return LocationKind(KindData & LocationKindMask);
  }
  StorageKind getStorageKind() const {
    return StorageKind(KindData & StorageKindMask);
  }

  template <typename T>
  bool is() const {
    return T::isKind(*this);
  }

  template <typename T>
  T castTo() const {
    assert(T::isKind(*this));
    T t;
    SILLocation& tr = t;
    tr = *this;
    return t;
  }

  template <typename T>
  Optional<T> getAs() const {
    if (!T::isKind(*this))
      return Optional<T>();
    T t;
    SILLocation& tr = t;
    tr = *this;
    return t;
  }

  /// If the current value is of the specified AST unit type T,
  /// return it, otherwise return null.
  template <typename T> T *getAsASTNode() const {
    return isASTNode() ? getNodeAs<T>(Loc.ASTNode.Primary) : nullptr;
  }

  /// Returns true if the Location currently points to the AST node
  /// matching type T.
  template <typename T> bool isASTNode() const {
    return isASTNode() && isNode<T>(Loc.ASTNode.Primary);
  }

  /// Returns the primary value as the specified AST node type. If the
  /// specified type is incorrect, asserts.
  template <typename T> T *castToASTNode() const {
    assert(isASTNode());
    return castNodeTo<T>(Loc.ASTNode.Primary);
  }

  /// If the DebugLoc is of the specified AST unit type T,
  /// return it, otherwise return null.
  template <typename T> T *getDebugLocAsASTNode() const {
    assert(hasDebugLoc() && "no debug location");
    return getNodeAs<T>(Loc.ASTNode.ForDebugger);
  }

  /// Return the location as a DeclContext or null.
  DeclContext *getAsDeclContext() const;

  /// Convert a specialized location kind into a regular location.
  SILLocation getAsRegularLocation() {
    SILLocation RegularLoc = *this;
    RegularLoc.setLocationKind(RegularKind);
    return RegularLoc;
  }

  SourceLoc getDebugSourceLoc() const;
  SourceLoc getSourceLoc() const;
  SourceLoc getStartSourceLoc() const;
  SourceLoc getEndSourceLoc() const;
  SourceRange getSourceRange() const {
    return {getStartSourceLoc(), getEndSourceLoc()};
  }
  DebugLoc getDebugInfoLoc() const {
    assert(isDebugInfoLoc());
    return DebugLoc(Loc.DebugInfoLoc.Line, Loc.DebugInfoLoc.Column,
                    StringRef(Loc.DebugInfoLoc.FilenameData, ExtraStorageData));
  }

  /// Fingerprint a DebugLoc for use in a DenseMap.
  using DebugLocKey = std::pair<std::pair<unsigned, unsigned>, StringRef>;
  struct DebugLocHash : public DebugLocKey {
    DebugLocHash(DebugLoc L) : DebugLocKey({{L.Line, L.Column}, L.Filename}) {}
  };

  /// Extract the line, column, and filename.
  static DebugLoc decode(SourceLoc Loc, const SourceManager &SM);

  /// Return the decoded debug location.
  LLVM_NODISCARD DebugLoc decodeDebugLoc(const SourceManager &SM) const {
    return isDebugInfoLoc() ? getDebugInfoLoc()
                            : decode(getDebugSourceLoc(), SM);
  }

  /// Compiler-generated locations may be applied to instructions without any
  /// clear correspondence to an AST node in an otherwise normal function.
  static DebugLoc getCompilerGeneratedDebugLoc() {
    return {0, 0, "<compiler-generated>"};
  }
  
  /// Pretty-print the value.
  void dump(const SourceManager &SM) const;
  void print(raw_ostream &OS, const SourceManager &SM) const;

  /// Returns an opaque pointer value for the debug location that may
  /// be used to unique debug locations.
  const void *getOpaquePointerValue() const {
    if (isSILFile())
      return Loc.SILFileLoc.getOpaquePointerValue();
    if (isASTNode())
      return Loc.ASTNode.Primary.getOpaqueValue();
    else
      return 0;
  }
  unsigned getOpaqueKind() const { return KindData; }

  inline bool operator==(const SILLocation& R) const {
    return KindData == R.KindData &&
           Loc.ASTNode.Primary.getOpaqueValue() ==
               R.Loc.ASTNode.Primary.getOpaqueValue() &&
           Loc.ASTNode.ForDebugger.getOpaqueValue() ==
               R.Loc.ASTNode.ForDebugger.getOpaqueValue();
  }

  inline bool operator!=(const SILLocation &R) const { return !(*this == R); }
};

/// Allowed on any instruction.
class RegularLocation : public SILLocation {
public:
  RegularLocation(Stmt *S) : SILLocation(S, RegularKind) {}
  RegularLocation(Expr *E) : SILLocation(E, RegularKind) {}
  RegularLocation(Decl *D) : SILLocation(D, RegularKind) {}
  RegularLocation(Pattern *P) : SILLocation(P, RegularKind) {}
  RegularLocation(SourceLoc L) : SILLocation(L, RegularKind) {}
  RegularLocation(DebugLoc L) : SILLocation(L, RegularKind) {}

  /// Returns a location representing the module.
  static RegularLocation getModuleLocation() {
    RegularLocation Loc;
    Loc.markAsInTopLevel();
    return Loc;
  }

  /// If the current value is of the specified AST unit type T,
  /// return it, otherwise return null.
  template <typename T> T *getAs() const { return getNodeAs<T>(Loc.ASTNode); }

  /// Returns true if the Location currently points to the AST node
  /// matching type T.
  template <typename T> bool is() const { return isNode<T>(Loc.ASTNode); }

  /// Returns the primary value as the specified AST node type. If the
  /// specified type is incorrect, asserts.
  template <typename T> T *castTo() const { return castNodeTo<T>(Loc.ASTNode); }

  /// Compiler-generated locations may be applied to instructions without any
  /// clear correspondence to an AST node in an otherwise normal function.
  /// The auto-generated bit also turns off certain diagnostics passes such.
  static RegularLocation getAutoGeneratedLocation() {
    RegularLocation AL(getCompilerGeneratedDebugLoc());
    AL.markAutoGenerated();
    return AL;
  }

  /// Returns a location that is compiler-generated, but with a hint as to where
  /// it may have been generated from. These locations will have an artificial
  /// line location of zero in DWARF, but in CodeView we want to use the given
  /// line since line zero does not represent an artificial line in CodeView.
  static RegularLocation getAutoGeneratedLocation(SourceLoc L) {
    RegularLocation AL(L);
    AL.markAutoGenerated();
    return AL;
  }

private:
  RegularLocation() : SILLocation(RegularKind) {}

  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return L.getKind() == RegularKind;
  }
};

/// Used to represent a return instruction in user code.
///
/// Allowed on an BranchInst, ReturnInst.
class ReturnLocation : public SILLocation {
public:
  ReturnLocation(ReturnStmt *RS);

  /// Construct the return location for a constructor or a destructor.
  ReturnLocation(BraceStmt *BS);

  ReturnStmt *get();

private:
  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return L.getKind() == ReturnKind;
  }
};

/// Used on the instruction that was generated to represent an implicit
/// return from a function.
///
/// Allowed on an BranchInst, ReturnInst.
class ImplicitReturnLocation : public SILLocation {
public:

  ImplicitReturnLocation(AbstractClosureExpr *E);

  ImplicitReturnLocation(ReturnStmt *S);

  ImplicitReturnLocation(AbstractFunctionDecl *AFD);

  /// Construct from a RegularLocation; preserve all special bits.
  ///
  /// Note, this can construct an implicit return for an arbitrary expression
  /// (specifically, in case of auto-generated bodies).
  static SILLocation getImplicitReturnLoc(SILLocation L);

  AbstractClosureExpr *get();

private:
  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return L.getKind() == ImplicitReturnKind;
  }
  ImplicitReturnLocation() : SILLocation(ImplicitReturnKind) {}
};

/// Marks instructions that correspond to inlined function body and
/// setup code. This location should not be used for inlined transparent
/// bodies, see MandatoryInlinedLocation.
///
/// This location wraps the call site ASTNode.
///
/// Allowed on any instruction except for ReturnInst.
class InlinedLocation : public SILLocation {
public:
  InlinedLocation(Expr *CallSite) : SILLocation(CallSite, InlinedKind) {}
  InlinedLocation(Stmt *S) : SILLocation(S, InlinedKind) {}
  InlinedLocation(Pattern *P) : SILLocation(P, InlinedKind) {}
  InlinedLocation(Decl *D) : SILLocation(D, InlinedKind) {}

  /// Constructs an inlined location when the call site is represented by a
  /// SILFile location.
  InlinedLocation(SourceLoc L) : SILLocation(InlinedKind) {
    setStorageKind(SILFileKind);
    Loc.SILFileLoc = L;
  }

  static InlinedLocation getInlinedLocation(SILLocation L);

private:
  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return L.getKind() == InlinedKind;
  }
  InlinedLocation() : SILLocation(InlinedKind) {}

  InlinedLocation(Expr *E, unsigned F) : SILLocation(E, InlinedKind, F) {}
  InlinedLocation(Stmt *S, unsigned F) : SILLocation(S, InlinedKind, F) {}
  InlinedLocation(Pattern *P, unsigned F) : SILLocation(P, InlinedKind, F) {}
  InlinedLocation(Decl *D, unsigned F) : SILLocation(D, InlinedKind, F) {}
  InlinedLocation(SourceLoc L, unsigned F) : SILLocation(L, InlinedKind, F) {}

  static InlinedLocation getModuleLocation(unsigned Flags) {
    auto L = InlinedLocation();
    L.setSpecialFlags(Flags);
    return L;
  }

};

/// Marks instructions that correspond to inlined function body and
/// setup code for transparent functions, inlined as part of mandatory inlining
/// pass.
///
/// This location wraps the call site ASTNode.
///
/// Allowed on any instruction except for ReturnInst.
class MandatoryInlinedLocation : public SILLocation {
public:
  MandatoryInlinedLocation(Expr *CallSite) :
    SILLocation(CallSite, MandatoryInlinedKind) {}
  MandatoryInlinedLocation(Stmt *S) : SILLocation(S, MandatoryInlinedKind) {}
  MandatoryInlinedLocation(Pattern *P) : SILLocation(P, MandatoryInlinedKind) {}
  MandatoryInlinedLocation(Decl *D) : SILLocation(D, MandatoryInlinedKind) {}

  /// Constructs an inlined location when the call site is represented by a
  /// SILFile location.
  MandatoryInlinedLocation(SourceLoc L)
      : SILLocation(L, MandatoryInlinedKind) {}

  static MandatoryInlinedLocation getMandatoryInlinedLocation(SILLocation L);
  static MandatoryInlinedLocation getAutoGeneratedLocation();

  static MandatoryInlinedLocation getModuleLocation(unsigned Flags) {
    auto L = MandatoryInlinedLocation();
    L.setSpecialFlags(Flags);
    return L;
  }
  
private:
  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return L.getKind() == MandatoryInlinedKind;
  }
  MandatoryInlinedLocation() : SILLocation(MandatoryInlinedKind) {}
  MandatoryInlinedLocation(Expr *E, unsigned F)
      : SILLocation(E, MandatoryInlinedKind, F) {}
  MandatoryInlinedLocation(Stmt *S, unsigned F)
      : SILLocation(S, MandatoryInlinedKind, F) {}
  MandatoryInlinedLocation(Pattern *P, unsigned F)
      : SILLocation(P, MandatoryInlinedKind, F) {}
  MandatoryInlinedLocation(Decl *D, unsigned F)
      : SILLocation(D, MandatoryInlinedKind, F) {}
  MandatoryInlinedLocation(SourceLoc L, unsigned F)
      : SILLocation(L, MandatoryInlinedKind, F) {}
  MandatoryInlinedLocation(DebugLoc L, unsigned F)
      : SILLocation(L, MandatoryInlinedKind, F) {}
};

/// Used on the instruction performing auto-generated cleanup such as
/// deallocs, destructor calls.
///
/// The cleanups are performed after completing the evaluation of the AST Node
/// wrapped inside the SILLocation. This location wraps the statement
/// representing the enclosing scope, for example, FuncDecl, ParenExpr. The
/// scope's end location points to the SourceLoc that shows when the operation
/// is performed at runtime.
///
/// Allowed on any instruction except for ReturnInst.
/// Locations of an inlined destructor should also be represented by this.
class CleanupLocation : public SILLocation {
public:
  CleanupLocation(Expr *E) : SILLocation(E, CleanupKind) {}
  CleanupLocation(Stmt *S) : SILLocation(S, CleanupKind) {}
  CleanupLocation(Pattern *P) : SILLocation(P, CleanupKind) {}
  CleanupLocation(Decl *D) : SILLocation(D, CleanupKind) {}

  static CleanupLocation get(SILLocation L);

  /// Returns a location representing a cleanup on the module level.
  static CleanupLocation getModuleCleanupLocation() {
    CleanupLocation Loc;
    Loc.markAsInTopLevel();
    return Loc;
  }

private:
  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return L.getKind() == CleanupKind;
  }
  CleanupLocation() : SILLocation(CleanupKind) {}

  CleanupLocation(Expr *E, unsigned F) : SILLocation(E, CleanupKind, F) {}
  CleanupLocation(Stmt *S, unsigned F) : SILLocation(S, CleanupKind, F) {}
  CleanupLocation(Pattern *P, unsigned F) : SILLocation(P, CleanupKind, F) {}
  CleanupLocation(Decl *D, unsigned F) : SILLocation(D, CleanupKind, F) {}
};

/// Used to represent an unreachable location that was
/// auto-generated and has no correspondence to user code. It should
/// not be used in diagnostics or for debugging.
///
/// Differentiates an unreachable instruction, which is generated by
/// DCE, from an unreachable instruction in user code (output of SILGen).
/// Allowed on an unreachable instruction.
class ArtificialUnreachableLocation : public SILLocation {
public:
  ArtificialUnreachableLocation() : SILLocation(ArtificialUnreachableKind) {}
private:
  friend class SILLocation;
  static bool isKind(const SILLocation& L) {
    return (L.getKind() == ArtificialUnreachableKind);
  }
};

class SILDebugScope;

/// A SILLocation paired with a SILDebugScope.
class SILDebugLocation {
  const SILDebugScope *Scope = nullptr;
  SILLocation Location;

public:
  SILDebugLocation()
      : Scope(nullptr),
        Location(RegularLocation::getAutoGeneratedLocation()) {}
  SILDebugLocation(SILLocation Loc, const SILDebugScope *DS)
      : Scope(DS), Location(Loc) {}
  SILLocation getLocation() const { return Location; }
  const SILDebugScope *getScope() const { return Scope; }
};

} // end swift namespace


#endif

/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#ifndef TART_TYPE_FUNCTIONTYPE_H
#define TART_TYPE_FUNCTIONTYPE_H

#ifndef TART_TYPE_TYPE_H
#include "tart/Type/Type.h"
#endif

#ifndef TART_DEFN_DEFN_H
#include "tart/Defn/Defn.h"
#endif

#include "llvm/DerivedTypes.h"

namespace tart {

class FunctionDefn;

// -------------------------------------------------------------------
// Type class for functions and macros.
class FunctionType : public Type {
public:
  FunctionType(QualifiedType rtype, ParameterList & plist);
  FunctionType(QualifiedType rtype, ParameterDefn ** plist, size_t pcount);
  FunctionType(QualifiedType rtype, ParameterDefn * selfParam, ParameterDefn ** plist,
      size_t pcount);

  // isStatic
  bool isStatic() const { return isStatic_; }
  void setIsStatic(bool value) { isStatic_ = value; }

  // isInvocable
  bool isInvocable() const { return isInvocable_; }
  void setIsInvocable(bool value) { isInvocable_ = value; }

  // Return type
  QualifiedType returnType() const { return returnType_; }
  void setReturnType(QualifiedType type) { returnType_ = type; }

  /** True if this function type uses 'struct return' calling convention. */
  bool isStructReturn() const;

  llvm::Type * irType() const;

  // Parameter methods

  /** The 'self' parameter of the function. */
  ParameterDefn * selfParam() const { return selfParam_; }
  void setSelfParam(ParameterDefn * self) { selfParam_ = self; }

  ParameterList & params() { return params_; }
  const ParameterList & params() const { return params_; }
  const ParameterDefn * param(int index) const { return params_[index]; }
  ParameterDefn * param(int index) { return params_[index]; }
  const Type * paramType(int index) const;
  void addParam(ParameterDefn * param);
  ParameterDefn * addParam(StringRef name, const Type * type);

  /** Return the parameter types as a type vector. */
  TupleType * paramTypes() const;

  /** Given the name of a parameter, return the index of that parameter,
      or -1 if there is no such parameter. */
  int paramNameIndex(StringRef name) const;

  // Misc methods

  /** Print an explanation as to why this function type is not singular. */
  void whyNotSingular() const;

  /** Return the name of the 'invoke' trampoline function for this function type.
      This is used when calling methods via reflection. */
  StringRef invokeName() const;

  /** Return true if any of the parameter types in this function are of type BadType. */
  bool hasErrors() const;

  // Overrides

  llvm::Type * createIRType() const;
  llvm::FunctionType * createIRFunctionType(
      const Type * selfType, const ParameterList & params, QualifiedType returnType) const;
  llvm::Type * irEmbeddedType() const;
  llvm::Type * irParameterType() const;
  bool isReferenceType() const;
  Expr * nullInitValue() const;
  bool isSingular() const;
  TypeShape typeShape() const;
  void trace() const;
  void format(FormatStream & out) const;

  static inline bool classof(const FunctionType *) { return true; }
  static inline bool classof(const Type * type) {
    return type->typeClass() == Function;
  }

private:
  bool isStatic_;
  QualifiedType returnType_;
  ParameterDefn * selfParam_;
  ParameterList params_;
  mutable TupleType * paramTypes_;
  mutable llvm::Type * irType_;
  mutable bool isCreatingType;
  mutable bool isStructReturn_;
  mutable bool isInvocable_;
  mutable llvm::SmallString<0> invokeName_;
};

}

#endif

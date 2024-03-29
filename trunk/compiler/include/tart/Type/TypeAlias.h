/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#ifndef TART_TYPE_TYPEALIAS_H
#define TART_TYPE_TYPEALIAS_H

#ifndef TART_TYPE_TYPE_H
#include "tart/Type/Type.h"
#endif

#ifndef TART_TYPE_QUALIFIEDTYPE_H
#include "tart/Type/QualifiedType.h"
#endif

namespace tart {

/// -------------------------------------------------------------------
/// A named alias of a type.
class TypeAlias : public Type {
public:
  /** Construct a new typealias. */
  TypeAlias(const Type * val, TypeDefn * defn);

  /** The target of this alias. */
  QualifiedType value() const { return value_; }
  void setValue(QualifiedType value) { value_ = value; }

  // Overrides

  TypeDefn * typeDefn() const { return defn_; }
  bool isSingular() const { return value_->isSingular(); }
  bool isReferenceType() const { return value_->isReferenceType(); }
  TypeShape typeShape() const { return value_->typeShape(); }
  llvm::Type * irType() const;
  llvm::Type * irEmbeddedType() const;
  llvm::Type * irParameterType() const;
  llvm::Type * irReturnType() const;
  void expandImpl(QualifiedTypeSet & out, unsigned qualifiers) const {
    value_.expand(out, qualifiers);
  }
  Expr * nullInitValue() const { return value_->nullInitValue(); }
  void trace() const;
  void format(FormatStream & out) const;

  static inline bool classof(const TypeAlias *) { return true; }
  static inline bool classof(const Type * ty) {
    return ty->typeClass() == Alias;
  }

private:
  QualifiedType value_;
  TypeDefn * defn_;
};

}

#endif

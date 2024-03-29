/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#ifndef TART_TYPE_QUALIFIEDTYPE_H
#define TART_TYPE_QUALIFIEDTYPE_H

#ifndef TART_COMMON_FORMATTABLE_H
#include "tart/Common/Formattable.h"
#endif

#ifndef LLVM_SUPPORT_CASTING_H
#include "llvm/Support/Casting.h"
#endif

#ifndef LLVM_ADT_SMALLVECTOR_H
#include "llvm/ADT/SmallVector.h"
#endif

#ifndef LLVM_ADT_DENSESET_H
#include "llvm/ADT/DenseSet.h"
#endif

#ifndef LLVM_SUPPORT_COMPILER_H
#include "llvm/Support/Compiler.h"
#endif

namespace tart {

class Type;
struct SourceLocation;
class QualifiedTypeSet;

// Forward declaration of dealias.
const Type * dealias(const Type * t);

/** Combine type qualifiers. Right side qualifiers override left side precedence. */
unsigned combineQualifiers(unsigned left, unsigned right);

// Helper functions for debugging.
const char * typeTostr(const Type * ty, unsigned qual);
void dumpType(const Type * ty, unsigned qual);

/// -------------------------------------------------------------------
/// Value type containing a type reference and qualifier bits.
template<class T>
class Qualified {
public:
  /** Possible modifier bits. */
  enum Qualifiers {
    MUTABLE = (1<<0),
    IMMUTABLE = (1<<1),
    READONLY = (1<<2),
    ADOPTED = (1<<3),
    VARIADIC = (1<<4),
    VOLATILE = (1<<5),

    MUTABILITY_MASK = MUTABLE | IMMUTABLE | READONLY,
  };

  Qualified() : type_(NULL), qualifiers_(0) {}
  Qualified(const T * type) : type_(type), qualifiers_(0) {}
  Qualified(const T * type, unsigned qualifiers) : type_(type), qualifiers_(qualifiers) {}
  Qualified(const Qualified & qty) : type_(qty.type_), qualifiers_(qty.qualifiers_) {}

  /** The qualifier bits for this type. */
  unsigned qualifiers() const { return qualifiers_; }

  /** Set the qualifier bits for this type. */
  void setQualifiers(unsigned qual) { qualifiers_ = qual; }

  /** Add qualifier bits for this type. */
  void addQualifiers(unsigned qual) { qualifiers_ = combineQualifiers(qualifiers_, qual); }

  /** Remove qualifier bits for this type. */
  void removeQualifiers(unsigned qual) { qualifiers_ &= ~qual; }

  /** Convenience functions. */
  bool isMutable() const { return (qualifiers_ & MUTABLE) != 0; }
  bool isImmutable() const { return (qualifiers_ & IMMUTABLE) != 0; }
  bool isReadOnly() const { return (qualifiers_ & READONLY) != 0; }
  bool isAdopted() const { return (qualifiers_ & ADOPTED) != 0; }
  bool isVariadic() const { return (qualifiers_ & VARIADIC) != 0; }
  bool isVolatile() const { return (qualifiers_ & VOLATILE) != 0; }
  bool isWritable() const { return (qualifiers_ & (IMMUTABLE|READONLY)) == 0; }

  /** The unmodified base type. */
  const T * type() const { return type_; }

  /** The unmodified base type. */
  const T * unqualified() const { return type_; }

  /** Return this type with aliases removed. */
  Qualified dealias() const {
    return Qualified(static_cast<const T *>(::tart::dealias(type_)), qualifiers_);
  }

  /** Return all the possible concrete types that this type could be. */
  void expand(QualifiedTypeSet & out) const {
    type_->expandImpl(out, qualifiers_);
  }

  /** Return all the possible concrete types that this type could be. */
  void expand(QualifiedTypeSet & out, unsigned qualifiers) const {
    type_->expandImpl(out, combineQualifiers(qualifiers_, qualifiers));
  }

  /** Assignment. */
  Qualified & operator=(const Qualified & qty) {
    type_ = qty.type_;
    qualifiers_ = qty.qualifiers_;
    return *this;
  }

  /** Assignment from type. */
  Qualified & operator=(const T * ty) {
    type_ = ty;
    qualifiers_ = 0;
    return *this;
  }

  /** Can be used as a type pointer */
  const T * operator->() const {
    return type_;
  }

  /** Adding additional qualifiers via '|' */
  friend Qualified operator|(const Qualified & qty, unsigned qualifiers) {
    return Qualified(qty.type_, combineQualifiers(qty.qualifiers_, qualifiers));
  }

  friend Qualified operator|(const Qualified & qty, Qualifiers qualifiers) {
    return Qualified(qty.type_, combineQualifiers(qty.qualifiers_, qualifiers));
  }

  /** Equality. */
  bool operator==(const Qualified & qty) const {
    return type_ == qty.type_ && qualifiers_ == qty.qualifiers_;
  }

  /** Inequality. */
  bool operator!=(const Qualified & qty) const {
    return type_ != qty.type_ || qualifiers_ != qty.qualifiers_;
  }

  /** Static cast. */
  template <class T2>
  const Qualified<T2> as() const {
    return Qualified<T2>(static_cast<const T2 *>(type_), qualifiers_);
  }

  /** Checked cast. */
  template <class T2>
  const Qualified<T2> cast() const {
    return Qualified<T2>(llvm::cast<T2>(type_), qualifiers_);
  }

  /** Dynamic cast. */
  template <class T2>
  const Qualified<T2> dyn_cast() const {
    return Qualified<T2>(llvm::dyn_cast<T2>(type_), qualifiers_);
  }

  /** Dynamic cast. */
  template <class T2>
  const Qualified<T2> dyn_cast_or_null() const {
    return Qualified<T2>(llvm::dyn_cast_or_null<T2>(type_), qualifiers_);
  }

  /** Type check. */
  template <class T2>
  bool isa() const {
    return llvm::isa<T2>(type_);
  }

  /** Return true if the type pointer is NULL. */
  bool isNull() const { return type_ == NULL; }

  /** Implicitly casting to bool checks if the type is non-null. */
  operator bool() const {
    return type_ != NULL;
  }

  /** Helper methods for debugging. */
  LLVM_ATTRIBUTE_NOINLINE void dump() const { dumpType(type_, qualifiers_); }
  LLVM_ATTRIBUTE_NOINLINE const char * str() const { return typeTostr(type_, qualifiers_); }

  /** Structure used when using Qualified as a map key. */
  struct KeyInfo {
    static inline Qualified getEmptyKey() {
      return Qualified(reinterpret_cast<const Type *>(0), 0);
    }

    static inline Qualified getTombstoneKey() {
      return Qualified(reinterpret_cast<const Type *>(-1), 0);
    }

    static unsigned getHashValue(const Qualified & val) {
      return (uintptr_t(val.type_) >> 4) * 0x5bd1e995 ^ (val.qualifiers_ << 1);
    }

    static bool isEqual(const Qualified & lhs, const Qualified & rhs) {
      return lhs.type_ == rhs.type_ && lhs.qualifiers_ == rhs.qualifiers_;
    }

    static bool isPod() { return true; }
  };

  /** Sentinel value that indicates an uninitialized type reference. */
  static const Qualified NONE;

private:
  const T * type_;
  unsigned qualifiers_;
};

template<class T>
const Qualified<T> Qualified<T>::NONE(NULL);

/** QualifiedType gets used most often with Type. */
typedef Qualified<Type> QualifiedType;

/** A vector of QualifiedTypes. */
typedef llvm::SmallVector<QualifiedType, 8> QualifiedTypeList;

/** A set of QualifiedTypes. */
class QualifiedTypeSet : public llvm::DenseSet<QualifiedType, QualifiedType::KeyInfo> {};

/** True if 'lq' and 'rq' qualifiers are equivalent. */
bool areQualifiersEquivalent(unsigned lq, unsigned rq);

/** True if a value with 'from' qualifiers can be assigned to a variable with 'to' qualifiers. */
bool canAssignQualifiers(unsigned from, unsigned to);

/** Print a QualifiedType to an output stream. */
void formatQualifiedType(FormatStream & out, const Type * ty, unsigned qualifiers);
void formatQualifiedTypePrefix(FormatStream & out, unsigned qualifiers);
void formatQualifiedTypeSuffix(FormatStream & out, unsigned qualifiers);

template <class T>
inline FormatStream & operator<<(FormatStream & out, const Qualified<T> & qual) {
  formatQualifiedType(out, qual.unqualified(), qual.qualifiers());
  return out;
}

} // namespace tart

#endif // TART_TYPE_QUALIFIEDTYPE_H

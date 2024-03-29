/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/Defn/FunctionDefn.h"
#include "tart/Defn/Template.h"

#include "tart/Type/AmbiguousParameterType.h"
#include "tart/Type/AmbiguousResultType.h"
#include "tart/Type/AmbiguousTypeParamType.h"
#include "tart/Type/CompositeType.h"
#include "tart/Type/EnumType.h"
#include "tart/Type/NativeType.h"
#include "tart/Type/PrimitiveType.h"
#include "tart/Type/TupleType.h"
#include "tart/Type/Type.h"
#include "tart/Type/TypeAlias.h"
#include "tart/Type/TypeFunction.h"
#include "tart/Type/TypeLiteral.h"
#include "tart/Type/TypeRelation.h"
#include "tart/Type/UnionType.h"
#include "tart/Type/UnitType.h"

#include "tart/Common/Diagnostics.h"

#include "tart/Objects/Builtins.h"
#include "tart/Objects/SystemDefs.h"

#include "tart/Sema/Infer/TypeAssignment.h"
#include "tart/Sema/CallCandidate.h"

#include "llvm/ADT/BitVector.h"

namespace tart {

static bool isEqualTuple(Qualified<TupleType> ltt, Qualified<TupleType> rtt) {
  if (ltt.qualifiers() != rtt.qualifiers()) {
    return false;
  }
  if (ltt.unqualified() == rtt.unqualified()) {
    return true;
  }
  if (ltt->size() == rtt->size()) {
    size_t size = ltt->size();
    for (size_t i = 0; i < size; ++i) {
      if (!TypeRelation::isEqual(ltt->member(i), rtt->member(i))) {
        return false;
      }
    }
    return true;
  }
  return false;
}

static bool isEqualComposite(Qualified<CompositeType> lct, Qualified<CompositeType> rct) {
  if (lct.qualifiers() != rct.qualifiers()) {
    return false;
  }
  // Check if both classes derive from the same AST.
  TypeDefn * ldef = lct->typeDefn();
  TypeDefn * rdef = rct->typeDefn();
  if (ldef != NULL &&
      rdef != NULL &&
      ldef->ast() != NULL &&
      ldef->ast() == rdef->ast()) {
    // Now just need to test if the type params are the same.
    while (ldef != NULL && rdef != NULL) {
      // They should both be instances, or neither.
      DASSERT(ldef->isTemplateInstance() == rdef->isTemplateInstance());
      if (ldef->isTemplateInstance()) {
        // Compare type parameters for equivalence.
        return isEqualTuple(
            ldef->templateInstance()->typeArgs(),
            rdef->templateInstance()->typeArgs());
      } else if (ldef->isTemplate() && rdef->isTemplate()) {
        DFAIL("Implement");
      }

      // It's possible that both sides are template instance members. Try again at the
      // enclosing level.
      ldef = dyn_cast<TypeDefn>(ldef->parentDefn());
      rdef = dyn_cast<TypeDefn>(rdef->parentDefn());
    }
  }
  return false;
}

static bool isEqualFunction(Qualified<FunctionType> lfn, Qualified<FunctionType> rfn) {
  if (lfn.qualifiers() != rfn.qualifiers()) {
    return false;
  }
  if (lfn->params().size() != rfn->params().size() ||
      lfn->isStatic() != rfn->isStatic()) {
    return false;
  }

  // Note that selfParam types are not compared. I *think* that's right, but
  // I'm not sure.

  // Also note that we aren't comparing type parameters, just function parameters.
  // Again, I *think* that's right. Having two functions with identical names & signatures
  // but having different type params would be ...weird.

  DASSERT(lfn->returnType());
  DASSERT(rfn->returnType());
  if (!TypeRelation::isEqual(lfn->returnType(), rfn->returnType())) {
    return false;
  }

  size_t numParams = lfn->params().size();
  for (size_t i = 0; i < numParams; ++i) {
    if (!TypeRelation::isEqual(lfn->param(i)->type(), rfn->param(i)->type())) {
      return false;
    }
    if (lfn->param(i)->isVariadic() != rfn->param(i)->isVariadic()) {
      return false;
    }
  }

  return true;
}

static bool isEqualUnion(Qualified<UnionType> lut, Qualified<UnionType> rut) {
  if (lut.qualifiers() != rut.qualifiers()) {
    return false;
  }
  if (isEqualTuple(lut->typeArgs(), rut->typeArgs())) {
    return true;
  }

//  // Handle the case where the union members might be in a different order because of type aliases.
//  // Make sure that all types in 'lut' are in 'rut'.
//  for (TupleType::const_iterator it = lut->begin(); it != lut->end(); ++it) {
//    if (rut->getTypeIndex(*it) < 0) {
//      return false;
//    }
//  }
//
//  // Make sure that all types in 'rut' are in 'lut'.
//  for (TupleType::const_iterator it = rut->begin(); it != rut->end(); ++it) {
//    if (lut->getTypeIndex(*it) < 0) {
//      return false;
//    }
//  }

  //return true;
  return false;
}

bool TypeRelation::isEqual(const QualifiedType & lt, const QualifiedType & rt) {
  // Early out
  if (lt.unqualified() == rt.unqualified() && lt.qualifiers() == rt.qualifiers()) {
    return true;
  }

  switch (rt->typeClass()) {
    case Type::Alias:
      return isEqual(lt, rt.as<TypeAlias>()->value());

    case Type::AmbiguousParameter:
    case Type::AmbiguousPhi:
    case Type::AmbiguousResult:
    case Type::AmbiguousTypeParam: {
      QualifiedTypeSet expansion;
      rt.expand(expansion);
      if (expansion.empty()) {
        return false;
      }
      for (QualifiedTypeSet::iterator it = expansion.begin(); it != expansion.end(); ++it) {
        if (!isEqual(lt, *it)) {
          return false;
        }
      }
      return true;
    }

    case Type::Assignment: {
      Qualified<TypeAssignment> ta = rt.as<TypeAssignment>();
      if (ta->value()) {
        return isEqual(lt, ta->value());
      }
      return false;
    }

    case Type::TypeFnCall: {
      Qualified<TypeFunctionCall> rcall = rt.as<TypeFunctionCall>();
      if (const TypeFunction * tfn = dyn_cast<TypeFunction>(dealias(rcall->fnVal()))) {
        return isEqual(lt, tfn->apply(rcall->args()) | rt.qualifiers());
      }
      if (Qualified<TypeFunctionCall> tfcall = lt.dyn_cast<TypeFunctionCall>()) {
        return isEqual(tfcall->fnVal(), rcall->fnVal()) && isEqual(tfcall->args(), rcall->args());
      }
      // Left side might be an alias to a TypeFunction, so keep going.
      break;
    }

    default:
      break;
  }

  switch (lt->typeClass()) {
    case Type::Alias:
      return isEqual(lt.as<TypeAlias>()->value() | lt.qualifiers(), rt);

    case Type::Primitive:
      if (lt->isUnsizedIntType() && rt->isUnsizedIntType()) {
        DASSERT(lt.qualifiers() == 0) << "Qualifiers not allowed on integer constants";
        DASSERT(rt.qualifiers() == 0) << "Qualifiers not allowed on integer constants";
        Qualified<UnsizedIntType> lint = lt.as<UnsizedIntType>();
        Qualified<UnsizedIntType> rint = rt.as<UnsizedIntType>();
        return lint->intVal() == rint->intVal();
      }
      // Qualifiers are meaningless for primitive types.
      return (lt.unqualified() == rt.unqualified());

    case Type::Enum:
    case Type::TypeVar:
      // Qualifiers are meaningless for these types.
      return (lt.unqualified() == rt.unqualified());

    case Type::Class:
    case Type::Struct:
    case Type::Interface:
    case Type::Protocol: {
      if (rt.isa<CompositeType>()) {
        return isEqualComposite(lt.as<CompositeType>(), rt.as<CompositeType>());
      }

      return false;
    }

    case Type::NAddress: {
      if (rt.isa<AddressType>()) {
        return isEqual(lt->typeParam(0), rt->typeParam(0)) && lt.qualifiers() == rt.qualifiers();
      }
      return false;
    }

    case Type::NArray: {
      if (rt.isa<NativeArrayType>()) {
        Qualified<NativeArrayType> lnat = lt.as<NativeArrayType>();
        Qualified<NativeArrayType> rnat = rt.as<NativeArrayType>();
        return isEqual(lnat->typeParam(0), rnat->typeParam(0))
            && lt.qualifiers() == rt.qualifiers()
            && lnat->size() == rnat->size();
      }
      return false;
    }

    case Type::FlexibleArray: {
      if (rt.isa<FlexibleArrayType>()) {
        return isEqual(lt->typeParam(0), rt->typeParam(0)) && lt.qualifiers() == rt.qualifiers();
      }
      return false;
    }

    case Type::Function: {
      if (rt.isa<FunctionType>()) {
        return isEqualFunction(lt.as<FunctionType>(), rt.as<FunctionType>());
      }
      return false;
    }

    case Type::Unit: {
      if (rt.isa<UnitType>()) {
        return lt.as<UnitType>()->value()->isEqual(rt.as<UnitType>()->value());
      }
      return false;
    }

    case Type::Tuple: {
      if (rt.isa<TupleType>()) {
        return isEqualTuple(lt.as<TupleType>(), rt.as<TupleType>());
      }
      return false;
    }

    case Type::Union: {
      if (rt.isa<UnionType>()) {
        return isEqualUnion(lt.as<UnionType>(), rt.as<UnionType>());
      }
      return false;
    }

    case Type::TypeLiteral: {
      if (rt.isa<TypeLiteralType>()) {
        return isEqual(
            lt.as<TypeLiteralType>()->literalType(), rt.as<TypeLiteralType>()->literalType());
      }
      return false;
    }

    case Type::AmbiguousParameter:
    case Type::AmbiguousPhi:
    case Type::AmbiguousResult:
    case Type::AmbiguousTypeParam: {
      QualifiedTypeSet expansion;
      lt.expand(expansion);
      if (expansion.empty()) {
        return false;
      }
      for (QualifiedTypeSet::iterator it = expansion.begin(); it != expansion.end(); ++it) {
        if (!isEqual(*it, rt)) {
          return false;
        }
      }
      return true;
    }

    case Type::Assignment: {
      Qualified<TypeAssignment> ta = lt.as<TypeAssignment>();
      if (ta->value()) {
        return isEqual(ta->value(), rt);
      }
      return false;
    }

    case Type::TypeFnCall: {
      Qualified<TypeFunctionCall> tfcall = lt.as<TypeFunctionCall>();
      if (const TypeFunction * tfn = dyn_cast<TypeFunction>(dealias(tfcall->fnVal()))) {
        return isEqual(tfn->apply(tfcall->args()) | lt.qualifiers(), rt);
      }
      return false;
    }

    case Type::TypeFnQual: {
      Qualified<QualifyingTypeFunction> lqt = lt.as<QualifyingTypeFunction>();
      if (Qualified<QualifyingTypeFunction> rqt = rt.dyn_cast<QualifyingTypeFunction>()) {
        return lqt.qualifiers() == rqt.qualifiers();
      }
      return false;
    }

    case Type::KindCount:
      DASSERT(false) << "Type class not supported by isEqual(): " << lt->typeClass();
      break;
  }
  return false;
}

bool TypeRelation::isSubtype(const QualifiedType & ty, const QualifiedType & base) {
  if (!canAssignQualifiers(ty.qualifiers(), base.qualifiers())) {
    return false;
  } else if (ty.unqualified() == base.unqualified()) {
    return true;
  }

  // Special cases for ambiguous base types.
  switch (base->typeClass()) {
    case Type::Alias:
      return isSubtype(ty, base.as<TypeAlias>()->value() | base.qualifiers());

    case Type::Protocol:
      // Special case for protocols - implicit inheritance
      if (base.as<CompositeType>()->isSupportedBy(ty.type())) {
        return true;
      }
      // Fall through and treat as a regular type
      break;

    case Type::AmbiguousParameter:
    case Type::AmbiguousPhi:
    case Type::AmbiguousResult:
    case Type::AmbiguousTypeParam: {
      QualifiedTypeSet expansion;
      base.expand(expansion);
      if (expansion.empty()) {
        return false;
      }
      for (QualifiedTypeSet::iterator it = expansion.begin(); it != expansion.end(); ++it) {
        if (!isSubtype(ty, *it)) {
          return false;
        }
      }
      return true;
    }

    case Type::Assignment: {
      Qualified<TypeAssignment> ta = base.as<TypeAssignment>();
      if (ta->value()) {
        return isSubtype(ty, ta->value());
      } else {
        bool any = false;
        for (ConstraintSet::const_iterator si = ta->begin(), sEnd = ta->end(); si != sEnd; ++si) {
          Constraint * cst = *si;
          if (cst->visited()) {
            any = true;
          } else if (cst->checkProvisions()) {
            if (cst->kind() == Constraint::UPPER_BOUND) {
              // There's no way to determine if this is true, so return false.
              return false;
            }

            cst->setVisited(true);
            if (!isSubtype(ty, cst->value())) {
              cst->setVisited(false);
              return false;
            }
            cst->setVisited(false);
            any = true;
          }
        }
        return any;
      }
    }

    case Type::TypeFnCall: {
      Qualified<TypeFunctionCall> tfcall = base.as<TypeFunctionCall>();
      if (const TypeFunction * tfn = dyn_cast<TypeFunction>(dealias(tfcall->fnVal()))) {
        return isSubtype(ty, tfn->apply(tfcall->args()) | ty.qualifiers());
      }
      return false;
    }

    default:
      break;
  }

  switch (ty->typeClass()) {
    case Type::Alias:
      return isSubtype(ty.as<TypeAlias>()->value() | ty.qualifiers(), base);

    case Type::Primitive: {
      // TODO: Factor in qualifiers
      const PrimitiveType * pType = static_cast<const PrimitiveType *>(ty.type());
      if (const PrimitiveType * pBase = dyn_cast<PrimitiveType>(base.type())) {
        return pType->isSubtypeOf(pBase);
      }
      return false;
    }

    case Type::Class:
    case Type::Struct:
    case Type::Interface:
    case Type::Protocol: {
      // TODO: Factor in qualifiers
      Qualified<CompositeType> ctType = ty.as<CompositeType>();
      if (Qualified<CompositeType> ctBase = base.dyn_cast<CompositeType>()) {
        if (isEqualComposite(ctType, ctBase)) {
          return true;
        }

        // Interfaces are always considered to be subclasses of Object.
        if (ctType->typeClass() == Type::Interface && ctBase == Builtins::typeObject.get()) {
          return true;
        }

        // They aren't the same, check all base classes
        const ClassList & bases = ctType->bases();
        for (ClassList::const_iterator it = bases.begin(); it != bases.end(); ++it) {
          if (isSubtype(*it, base)) {
            return true;
          }
        }
      }

      return false;
    }

    case Type::Enum: {
      // TODO: Factor in qualifiers
      Qualified<EnumType> eTy = ty.as<EnumType>();
      if (base.isa<PrimitiveType>()) {
        return isSubtype(eTy->baseType(), base);
      }
      return false;
    }

    case Type::NAddress:
    case Type::NArray:
    case Type::FlexibleArray:
    case Type::Function:
    case Type::Unit:
    case Type::Tuple:
    case Type::Union:
    case Type::TypeLiteral:
    case Type::TypeVar:
      // None of these types support a subclass relationship, so equality is the only option.
      return isEqual(ty, base);

    case Type::AmbiguousParameter:
    case Type::AmbiguousPhi:
    case Type::AmbiguousResult:
    case Type::AmbiguousTypeParam: {
      QualifiedTypeSet expansion;
      ty.expand(expansion);
      if (expansion.empty()) {
        return false;
      }
      for (QualifiedTypeSet::iterator it = expansion.begin(); it != expansion.end(); ++it) {
        if (!isSubtype(*it, base)) {
          return false;
        }
      }
      return true;
    }

    case Type::Assignment: {
      Qualified<TypeAssignment> ta = ty.as<TypeAssignment>();
      if (ta->value()) {
        return isSubtype(ta->value(), base);
      } else {
        bool any = false;
        for (ConstraintSet::const_iterator si = ta->begin(), sEnd = ta->end(); si != sEnd; ++si) {
          Constraint * cst = *si;
          if (cst->visited()) {
            any = true;
          } else if (cst->checkProvisions()) {
            if (cst->kind() == Constraint::UPPER_BOUND) { // TODO: Wrong!
              // There's no way to determine if this is true, so return false.
              return false;
            }

            cst->setVisited(true);
            if (!isSubtype(cst->value(), base)) {
              cst->setVisited(false);
              return false;
            }
            cst->setVisited(false);
            any = true;
          }
        }
        return any;
      }
      return false;
    }

    case Type::TypeFnQual: {
      Qualified<QualifyingTypeFunction> qty = ty.as<QualifyingTypeFunction>();
      if (Qualified<QualifyingTypeFunction> qbase = ty.dyn_cast<QualifyingTypeFunction>()) {
        return canAssignQualifiers(qty.qualifiers(), qbase.qualifiers());
      }
      return false;
    }

    case Type::TypeFnCall: {
      Qualified<TypeFunctionCall> tfcall = ty.as<TypeFunctionCall>();
      if (const TypeFunction * tfn = dyn_cast<TypeFunction>(dealias(tfcall->fnVal()))) {
        return isSubtype(tfn->apply(tfcall->args()) | ty.qualifiers(), base);
      }
      return false;
    }

    case Type::KindCount:
      DASSERT(false) << "Type class " << ty->typeClass() << " not supported by isSubtype()";
      break;
  }

  return false;
}

bool TypeRelation::isSubclass(const QualifiedType & ty, const QualifiedType & base) {
  if (!canAssignQualifiers(ty.qualifiers(), base.qualifiers())) {
    return false;
  } else if (ty.unqualified() == base.unqualified()) {
    return true;
  }

  // Special cases for ambiguous base types.
  switch (base->typeClass()) {
    case Type::Alias:
      return isSubclass(ty, base.as<TypeAlias>()->value() | base.qualifiers());

    case Type::AmbiguousParameter:
    case Type::AmbiguousPhi:
    case Type::AmbiguousResult:
    case Type::AmbiguousTypeParam: {
      QualifiedTypeSet expansion;
      base.expand(expansion);
      if (expansion.empty()) {
        return false;
      }
      for (QualifiedTypeSet::iterator it = expansion.begin(); it != expansion.end(); ++it) {
        if (!isSubclass(ty, *it)) {
          return false;
        }
      }
      return true;
    }

    case Type::Assignment: {
      Qualified<TypeAssignment> ta = base.as<TypeAssignment>();
      if (ta->value()) {
        return isSubclass(ty, ta->value());
      } else {
        bool any = false;
        for (ConstraintSet::const_iterator si = ta->begin(), sEnd = ta->end(); si != sEnd; ++si) {
          Constraint * cst = *si;
          if (cst->visited()) {
            any = true;
          } else if (cst->checkProvisions()) {
            if (cst->kind() == Constraint::UPPER_BOUND) {
              // There's no way to determine if this is true, so return false.
              return false;
            }

            cst->setVisited(true);
            if (!isSubclass(ty, cst->value())) {
              cst->setVisited(false);
              return false;
            }
            cst->setVisited(false);
            any = true;
          }
        }
        return any;
      }
    }

    case Type::TypeFnCall: {
      Qualified<TypeFunctionCall> tfcall = base.as<TypeFunctionCall>();
      if (const TypeFunction * tfn = dyn_cast<TypeFunction>(dealias(tfcall->fnVal()))) {
        return isSubclass(ty, tfn->apply(tfcall->args()) | ty.qualifiers());
      }
      return false;
    }

    default:
      break;
  }

  switch (ty->typeClass()) {
    case Type::Alias:
      return isSubclass(ty.as<TypeAlias>()->value() | ty.qualifiers(), base);

    case Type::Primitive: {
      return false;
    }

    case Type::Class:
    case Type::Interface:
    case Type::Protocol: {
      // TODO: Factor in qualifiers
      Qualified<CompositeType> ctType = ty.as<CompositeType>();
      if (Qualified<CompositeType> ctBase = base.dyn_cast<CompositeType>()) {
        if (isEqualComposite(ctType, ctBase)) {
          return true;
        }

        // Interfaces are always considered to be subclasses of Object.
        if (ctType->typeClass() == Type::Interface && ctBase == Builtins::typeObject.get()) {
          return true;
        }

        // They aren't the same, check all base classes
        const ClassList & bases = ctType->bases();
        for (ClassList::const_iterator it = bases.begin(); it != bases.end(); ++it) {
          if (isSubclass(*it, base)) {
            return true;
          }
        }
      }

      return false;
    }

    case Type::AmbiguousParameter:
    case Type::AmbiguousPhi:
    case Type::AmbiguousResult:
    case Type::AmbiguousTypeParam: {
      QualifiedTypeSet expansion;
      ty.expand(expansion);
      if (expansion.empty()) {
        return false;
      }
      for (QualifiedTypeSet::iterator it = expansion.begin(); it != expansion.end(); ++it) {
        if (!isSubclass(*it, base)) {
          return false;
        }
      }
      return true;
    }

    case Type::Assignment: {
      Qualified<TypeAssignment> ta = ty.as<TypeAssignment>();
      if (ta->value()) {
        return isSubclass(ta->value(), base);
      } else {
        bool any = false;
        for (ConstraintSet::const_iterator si = ta->begin(), sEnd = ta->end(); si != sEnd; ++si) {
          Constraint * cst = *si;
          if (cst->visited()) {
            any = true;
          } else if (cst->checkProvisions()) {
            if (cst->kind() == Constraint::UPPER_BOUND) { // TODO: Wrong!
              // There's no way to determine if this is true, so return false.
              return false;
            }

            cst->setVisited(true);
            if (!isSubclass(cst->value(), base)) {
              cst->setVisited(false);
              return false;
            }
            cst->setVisited(false);
            any = true;
          }
        }
        return any;
      }
      return false;
    }

    case Type::TypeFnCall: {
      Qualified<TypeFunctionCall> tfcall = ty.as<TypeFunctionCall>();
      if (const TypeFunction * tfn = dyn_cast<TypeFunction>(dealias(tfcall->fnVal()))) {
        return isSubclass(tfn->apply(tfcall->args()) | ty.qualifiers(), base);
      }
      return false;
    }

    default:
      return false;
  }

  return false;
}

TypeRelation::RelativeSpecificity TypeRelation::isMoreSpecific(
    const QualifiedType & lhs, const QualifiedType & rhs) {
  if (lhs.isa<TypeAlias>()) {
    return isMoreSpecific(lhs.as<TypeAlias>()->value() | lhs.qualifiers(), rhs);
  }

  if (rhs.isa<TypeAlias>()) {
    return isMoreSpecific(lhs, rhs.as<TypeAlias>()->value() | rhs.qualifiers());
  }

  if (lhs.isa<TypeAssignment>()) {
    const TypeVariable * ltv = lhs.as<TypeAssignment>()->target();
    const QualifiedTypeList & lu = ltv->upperBounds();
    if (rhs.isa<TypeAssignment>()) {
      // Both sides are type variables. Check upper bounds.
      const TypeVariable * rtv = rhs.as<TypeAssignment>()->target();
      const QualifiedTypeList & ru = rtv->upperBounds();
      RelativeSpecificity result = EQUAL_SPECIFICITY;
      // Upper bounds: To say that left's upper bounds are *more* specific than right's, requires
      // that every member of right is a supertype of some member of left.
      // To say that left's upper bounds are *equally* specific as right's, requires that every
      // member of right is equal to some member of left, and not a supertype of another member of
      // left - and that there aren't any members of left that are also unmatched.
      llvm::BitVector leftUnmatched(lu.size(), true);
      for (QualifiedTypeList::const_iterator ri = ru.begin(), rEnd = ru.end(); ri != rEnd; ++ri) {
        int bestTypeIndex = -1;
        for (int index = 0; index < int(lu.size()); ++index) {
          if (isSubtype(lu[index], *ri)) {
            if (bestTypeIndex < 0 || isSubtype(lu[index], lu[bestTypeIndex])) {
              bestTypeIndex = index;
            }
          }
        }

        // If no type in left was a subtype of 'ri', then left is not more specific.
        if (bestTypeIndex < 0) {
          return NOT_MORE_SPECIFIC;
        } else {
          leftUnmatched[bestTypeIndex] = false;
        }
      }

      if (leftUnmatched.any()) {
        return MORE_SPECIFIC;
      } else {
        return result;
      }
    } else {
      // Only the left side is a type variable. To say that left's upper bounds are more specific
      // than the right-hand type, requires that right is a supertype of some member of left.
      // (They can't ever be equal.)
      const QualifiedTypeList & lu = ltv->upperBounds();
      for (QualifiedTypeList::const_iterator li = lu.begin(); li != lu.end(); ++li) {
        // lhs is a type var whose upper bound is more specific than rhs, so lhs counts as
        // more specific.
        if (isSubtype(*li, rhs) && !isEqual(*li, rhs)) {
          return MORE_SPECIFIC;
        }
      }
      // A type variable is less specific than a concrete type.
      return NOT_MORE_SPECIFIC;
    }
  } else if (rhs.isa<TypeAssignment>()) {
    // Only the right side is a type variable. To say that left is more specific than the
    // upper bounds of right would be true only if left is a subtype of *every* type in right.
    const TypeVariable * rtv = rhs.as<TypeAssignment>()->target();
    const QualifiedTypeList & ru = rtv->upperBounds();
    for (QualifiedTypeList::const_iterator ri = ru.begin(); ri != ru.end(); ++ri) {
      if (!isSubtype(lhs, *ri)) {
        // A type variable is less specific than a concrete type.
        return NOT_MORE_SPECIFIC;
      }
    }

    return MORE_SPECIFIC;
  } else {
    switch (lhs->typeClass()) {
//      case Type::Class:
//      case Type::Interface:
//        if (rhs->typeClass() != Type::Class &&
//            rhs->typeClass() != Type::Interface &&
//            rhs->typeClass() != Type::Protocol) {
//          return NOT_MORE_SPECIFIC;
//        }
//        break;
//
//      case Type::Struct:
//        if (rhs->typeClass() != Type::Struct &&
//            rhs->typeClass() != Type::Protocol) {
//          return NOT_MORE_SPECIFIC;
//        }
//        break;
//
//      case Type::Protocol:
//        if (rhs->typeClass() != Type::Class &&
//            rhs->typeClass() != Type::Interface &&
//            rhs->typeClass() != Type::Struct &&
//            rhs->typeClass() != Type::Protocol) {
//          return NOT_MORE_SPECIFIC;
//        }
//        break;

      case Type::NAddress:
      case Type::NArray:
      case Type::FlexibleArray:
        if (lhs->typeClass() != rhs->typeClass()) {
          return NOT_MORE_SPECIFIC;
        }
        return isMoreSpecific(lhs->typeParam(0), rhs->typeParam(0));

      default:
//        if (lhs->typeClass() != rhs->typeClass()) {
//          return NOT_MORE_SPECIFIC;
//        }

        break;
    }

    if (isEqual(lhs, rhs)) {
      // Ensure that equality is symmetrical.
      DASSERT_OBJ(TypeRelation::isEqual(rhs, lhs), lhs);
      return EQUAL_SPECIFICITY;
    } else if (isSubtype(lhs, rhs)) {
      return MORE_SPECIFIC;
    } else {
      return NOT_MORE_SPECIFIC;
    }
  }
}

} // namespace tart

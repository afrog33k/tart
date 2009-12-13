/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/CFG/Template.h"
#include "tart/CFG/TemplateConditions.h"
#include "tart/CFG/Constant.h"
#include "tart/CFG/FunctionType.h"
#include "tart/CFG/FunctionDefn.h"
#include "tart/CFG/CompositeType.h"
#include "tart/CFG/NativeType.h"
#include "tart/CFG/TupleType.h"
#include "tart/CFG/UnitType.h"
#include "tart/Sema/BindingEnv.h"
#include "tart/Sema/ScopeBuilder.h"
#include "tart/Sema/TypeTransform.h"
#include "tart/Common/Diagnostics.h"
#include "tart/Objects/Builtins.h"

namespace tart {

/** The set of traits that should be copied from the template to its
    instantiation. */
static const Defn::Traits INSTANTIABLE_TRAITS = Defn::Traits::of(
  Defn::Final,
  Defn::Abstract,
  Defn::ReadOnly,
  Defn::Extern,
  Defn::Ctor
);

// -------------------------------------------------------------------
// Class to discover all pattern variables in a type parameter list.

class FindPatternVars : public TypeTransform {
public:
  FindPatternVars(PatternVarList & vars) : vars_(vars) {}

  const Type * visitPatternVar(const PatternVar * in) {
    vars_.push_back(const_cast<PatternVar *>(in));
    return in;
  }

private:
  PatternVarList & vars_;
};

// -------------------------------------------------------------------
// PatternVar

PatternVar::PatternVar(const SourceLocation & location, const char * name, const Type * valueType)
  : TypeImpl(Pattern)
  , location_(location)
  , valueType_(valueType ? valueType : Builtins::typeTypeDescriptor)
  , name_(name)
{}

const llvm::Type * PatternVar::createIRType() const {
  DFAIL("Invalid");
}

ConversionRank PatternVar::convertImpl(const Conversion & cn) const {
  // The only place where this conversion function is called is when attempting to
  // determine if a custom coercion can be done (without actually doing it.)
  if (cn.resultValue != NULL) {
    DFAIL("Shouldn't be attempting to call convert on a Pattern Var (I think).");
  }

  return NonPreferred;
}

bool PatternVar::canBindTo(const Type * value) const {
  if (const UnitType * nt = dyn_cast<UnitType>(value)) {
    if (valueType_ == NULL) {
      return false;
    }

    ConstantExpr * expr = nt->value();
    return valueType_->canConvert(expr);
  } else if (valueType_ == NULL || valueType_->isSubtype(Builtins::typeTypeDescriptor)) {
    return true;
  } else {
    return false;
  }
}

bool PatternVar::isSubtype(const Type * other) const {
  return false;
}

bool PatternVar::isReferenceType() const {
  return false;
}

bool PatternVar::isSingular() const {
  return false;
}

void PatternVar::trace() const {
  TypeImpl::trace();
  location_.trace();
}

void PatternVar::format(FormatStream & out) const {
  if (out.getShowQualifiedName()) {
    out << name_ << "%" << name();
  } else {
    out << "%" << name();
  }
}

/// -------------------------------------------------------------------
/// TemplateSignature

TemplateSignature * TemplateSignature::get(Defn * v, Scope * parent) {
  if (v->templateSignature() == NULL) {
    v->setTemplateSignature(new TemplateSignature(v, parent));
  }

  return v->templateSignature();
}

TemplateSignature::TemplateSignature(Defn * v, Scope * parentScope)
  : value_(v)
  , ast_(NULL)
  , paramScope_(parentScope)
  , typeParams_(NULL)
{
  paramScope_.setScopeName("template-params");
}

void TemplateSignature::setTypeParams(const TupleType * typeParams) {
  DASSERT(typeParams_ == NULL);
  typeParams_ = typeParams;
  FindPatternVars(vars_).transform(typeParams_);
  for (PatternVarList::const_iterator it = vars_.begin(); it != vars_.end(); ++it) {
    PatternVar * var = *it;
    DASSERT(paramScope_.lookupSingleMember(var->name()) == NULL);
    TypeDefn * tdef = new TypeDefn(value_->module(), var->name(), var);
    paramScope_.addMember(tdef);
  }
}

const Type * TemplateSignature::typeParam(int index) const {
  return (*typeParams_)[index];
}

PatternVar * TemplateSignature::patternVar(const char * name) const {
  Defn * de = paramScope_.lookupSingleMember(name);
  if (TypeDefn * tdef = dyn_cast_or_null<TypeDefn>(de)) {
    return cast<PatternVar>(tdef->typeValue());
  }

  return NULL;
}

PatternVar * TemplateSignature::patternVar(int index) const {
  return vars_[index];
}

size_t TemplateSignature::patternVarCount() const {
  return vars_.size();
}

void TemplateSignature::trace() const {
  safeMark(ast_);
  safeMark(typeParams_);
  markList(conditions_.begin(), conditions_.end());
  for (SpecializationMap::const_iterator it = specializations_.begin();
      it != specializations_.end(); ++it) {
    it->first->mark();
    it->second->mark();
  }

  paramScope_.trace();
}

void TemplateSignature::format(FormatStream & out) const {
  out << "[" << typeParams_ << "]";
}

Defn * TemplateSignature::findSpecialization(const TupleType * tv) const {
  SpecializationMap::const_iterator it = specializations_.find(tv);
  if (it != specializations_.end()) {
    return it->second;
  }

  return NULL;
}

Defn * TemplateSignature::instantiate(const SourceLocation & loc, const BindingEnv & env,
    bool singular) {
  bool isPartial = false;

  // Check to make sure that the parameters are of the correct type.
  TypeRefList paramValues;
  bool noCache = false;
  for (PatternVarList::iterator it = vars_.begin(); it != vars_.end(); ++it) {
    PatternVar * var = *it;
    if (var->valueType() == NULL) {
      // TODO: This is needed because some pattern vars are created before
      // typeTypeDescriptor is loaded. We should fix that.
      DASSERT(Builtins::typeTypeDescriptor != NULL);
      var->setValueType(Builtins::typeTypeDescriptor);
    }

    const Type * value = env.subst(var);
    DASSERT_OBJ(value != NULL, var);
    if (!var->canBindTo(value)) {
      diag.fatal(loc) << "Type of expression " << value <<
          " incompatible with template parameter " << var << ":" << var->valueType();
      DASSERT(var->canBindTo(value));
    }

    if (isa<PatternValue>(value)) {
      noCache = true;
    }

    // We might need to do some coercion here...
    paramValues.push_back(value);
  }

  const TupleType * typeArgs = cast<TupleType>(env.subst(typeParams_));
  if (!typeArgs->isSingular()) {
    if (singular) {
      diag.fatal(loc) << "Non-singular parameters [" << typeArgs << "]";
    }

    isPartial = true;
  }

  // See if we can find an existing specialization that matches the arguments.
  // TODO: Canonicalize and create a key from the args.
  if (!noCache) {
    Defn * sp = findSpecialization(typeArgs);
    if (sp != NULL) {
      return sp;
    }
  }

  // Create the template instance
  DASSERT(value_->definingScope() != NULL);
  TemplateInstance * tinst = new TemplateInstance(value_, typeArgs);
  tinst->instantiatedFrom() = loc;

  // Create the definition
  Defn * result = NULL;
  if (value_->ast() != NULL) {
    result = ScopeBuilder::createDefn(tinst, &Builtins::syntheticModule, value_->ast());
    tinst->setValue(result);
    result->setQualifiedName(value_->qualifiedName());
    result->addTrait(Defn::Synthetic);
    result->traits().addAll(value_->traits() & INSTANTIABLE_TRAITS);
    result->setParentDefn(value_->parentDefn());
    result->setDefiningScope(tinst);
    if (isPartial) {
      result->addTrait(Defn::PartialInstantiation);
    }
  } else {
    DFAIL("Invalid template type");
  }

  // Copy over certain attributes
  if (FunctionDefn * fdef = dyn_cast<FunctionDefn>(result)) {
    fdef->setIntrinsic(static_cast<FunctionDefn *>(value_)->intrinsic());
  }

  if (!noCache) {
    specializations_[typeArgs] = result;
  }

  // Create a symbol for each template parameter.
  bool isSingular = true;

  for (size_t i = 0; i < vars_.size(); ++i) {
    PatternVar * var = vars_[i];
    TypeRef & value = paramValues[i];

    Defn * argDefn;
    if (UnitType * ntc = dyn_cast<UnitType>(value.type())) {
      argDefn = new VariableDefn(Defn::Let, result->module(), var->name(), ntc->value());
    } else {
      argDefn = new TypeDefn(result->module(), var->name(), value.type());
    }

    argDefn->setSingular(value.isSingular());
    isSingular &= value.isSingular();
    argDefn->addTrait(Defn::Synthetic);
    tinst->addMember(argDefn);
  }

  if (singular && !isSingular) {
    diag.fatal(loc) << Format_Verbose << "Expected " << result << " to be singular, why isn't it?";
  }

  // One additional parameter, which is the name of the instantiated symbol.
  if (TypeDefn * tdef = dyn_cast<TypeDefn>(result)) {
    if (CompositeType * ctype = dyn_cast<CompositeType>(tdef->typeValue())) {
      TypeDefn * nameAlias = new TypeDefn(result->module(), tdef->name(), ctype);
      nameAlias->setSingular(ctype->isSingular());
      nameAlias->addTrait(Defn::Synthetic);
      tinst->addMember(nameAlias);
    }
  }

  result->setSingular(isSingular);
  result->setTemplateInstance(tinst);

  DASSERT(isSingular == result->isSingular());

  if (singular && !result->isSingular()) {
    diag.fatal(loc) << Format_Verbose << "Expected " << result << " to be singular, why isn't it?";
    DFAIL("Non-singular");
  }

  //diag.info(loc) << "Creating template " << result;
  return result;
}

Type * TemplateSignature::instantiateType(const SourceLocation & loc, const BindingEnv & env) {

  if (value_->ast() != NULL) {
    TypeDefn * tdef = cast<TypeDefn>(instantiate(loc, env));
    return tdef->typeValue();
  }

  // Create the definition
  TypeDefn * tdef = static_cast<TypeDefn *>(value_);
  Type * proto = tdef->typeValue();
  if (proto->typeClass() != Type::NAddress &&
      proto->typeClass() != Type::NPointer &&
      proto->typeClass() != Type::NArray) {
    TypeDefn * tdef = cast<TypeDefn>(instantiate(loc, env));
    return tdef->typeValue();
  }

  // TODO: Can TypeTransform do this?
  // Check to make sure that the parameters are of the correct type.
  TypeList paramValues;
  for (PatternVarList::iterator it = vars_.begin(); it != vars_.end(); ++it) {
    PatternVar * var = *it;
    const Type * value = env.subst(var);
    DASSERT_OBJ(value != NULL, var);
    if (!var->canBindTo(value)) {
      diag.fatal(loc) << "Type of expression " << value <<
          " incompatible with template parameter " << var << ":" << var->valueType();
      DASSERT(var->canBindTo(value));
    }

    if (!value->isSingular()) {
      /*if (singular) {
        diag.fatal(loc) << "Non-singular parameter '" << var << "' = '" << value << "'";
      }*/
    }

    // We might need to do some coercion here...
    paramValues.push_back(const_cast<Type *>(value));
  }

  switch (tdef->typeValue()->typeClass()) {
    case Type::NAddress:
      return AddressType::get(paramValues[0]);

    case Type::NPointer:
      return PointerType::get(paramValues[0]);

    case Type::NArray: {
      return NativeArrayType::get(TupleType::get(paramValues));
    }

    default:
      DFAIL("Invalid template type");
      break;
  }

  return NULL;
}

/// -------------------------------------------------------------------
/// TemplateInstance

TemplateInstance::TemplateInstance(Defn * templateDefn, const TupleType * templateArgs)
  : value_(NULL)
  , templateDefn_(templateDefn)
  , typeArgs_(templateArgs)
  , parentScope_(templateDefn->definingScope())
{
}

const Type * TemplateInstance::typeArg(int index) const {
  return typeArgs_->member(index);
}

void TemplateInstance::addMember(Defn * d) {
  DASSERT(d->storageClass() != Storage_Local);
  DASSERT(d->definingScope() == NULL);
  SymbolTable::Entry * entry = paramDefns_.add(d);
  d->setDefiningScope(this);
}

bool TemplateInstance::lookupMember(const char * ident, DefnList & defs, bool inherit) const {
  const SymbolTable::Entry * entry = paramDefns_.findSymbol(ident);
  if (entry != NULL) {
    defs.append(entry->begin(), entry->end());
    return true;
  }

  return false;
}

void TemplateInstance::dumpHierarchy(bool full) const {
  std::string out;
  out.append("[template-instance] ");
  paramDefns_.getDebugSummary(out);
  diag.writeLnIndent(out);
}

void TemplateInstance::format(FormatStream & out) const {
  out << "[";
  for (TupleType::const_iterator it = typeArgs_->begin(); it != typeArgs_->end(); ++it) {
    if (it != typeArgs_->begin()) {
      out << ", ";
    }

    out << *it;
  }

  out << "]";
}

void TemplateInstance::trace() const {
  paramDefns_.trace();
  safeMark(value_);
  safeMark(typeArgs_);
}

} // namespace Tart

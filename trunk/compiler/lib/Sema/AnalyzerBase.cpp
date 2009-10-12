/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/Sema/AnalyzerBase.h"
#include "tart/Sema/ClassAnalyzer.h"
#include "tart/Sema/EnumAnalyzer.h"
#include "tart/Sema/FunctionAnalyzer.h"
#include "tart/Sema/PropertyAnalyzer.h"
#include "tart/Sema/ExprAnalyzer.h"
#include "tart/Sema/TypeAnalyzer.h"
#include "tart/Sema/VarAnalyzer.h"
#include "tart/Sema/ScopeBuilder.h"
#include "tart/Sema/BindingEnv.h"
#include "tart/Sema/SpecializeCandidate.h"
#include "tart/CFG/Defn.h"
#include "tart/CFG/TypeDefn.h"
#include "tart/CFG/FunctionDefn.h"
#include "tart/CFG/PrimitiveType.h"
#include "tart/CFG/NativeType.h"
#include "tart/CFG/Module.h"
#include "tart/CFG/Template.h"
#include "tart/Common/PackageMgr.h"
#include "tart/Common/Diagnostics.h"
#include "tart/Common/InternedString.h"
#include "tart/Objects/Builtins.h"

namespace tart {

AnalyzerBase::AnalysisQueue AnalyzerBase::queue;
size_t AnalyzerBase::queuePos = 0;

bool AnalyzerBase::lookupName(ExprList & out, const ASTNode * ast, bool absPath) {
  std::string path;
  lookupNameRecurse(out, ast, path, absPath);
  return !out.empty();
}

// Here's how this works:
// If we return true then it means we found something.
// If we return false, but path is non-empty, it means we found nothing,
//    but there's a chance it might be a package reference.
// If we return false and path is empty, then it means that we found nothing,
//    and there's no hope of finding anything.
bool AnalyzerBase::lookupNameRecurse(ExprList & out, const ASTNode * ast, std::string & path,
    bool absPath) {

  SLC & loc = ast->location();
  if (ast->nodeType() == ASTNode::Id) {
    const ASTIdent * ident = static_cast<const ASTIdent *>(ast);
    const char * name = ident->value();
    if (!absPath && activeScope != NULL && lookupIdent(out, name, loc)) {
      return true;
    }

    path.assign(name);
    return importName(out, path, absPath, loc);
  } else if (ast->nodeType() == ASTNode::Member) {
    const ASTMemberRef * mref = static_cast<const ASTMemberRef *>(ast);
    const ASTNode * qual = mref->qualifier();
    ExprList lvals;
    if (lookupNameRecurse(lvals, qual, path, absPath)) {
      if (lvals.size() > 1) {
        diag.fatal(ast) << "Multiply defined symbol " << qual;
        path.clear();
        return false;
      }

      Expr * context = lvals.front();
      if (findMemberOf(out, context, mref->memberName(), loc)) {
        return true;
      }

      path.clear();
      return false;
    }

    if (!path.empty()) {
      path.push_back('.');
      path.append(mref->memberName());
      return importName(out, path, absPath, loc);
    }

    return false;
  } else if (ast->nodeType() == ASTNode::Specialize) {
    const ASTSpecialize * spec = static_cast<const ASTSpecialize *>(ast);
    ExprList lvals;
    if (!lookupNameRecurse(lvals, spec->templateExpr(), path, absPath)) {
      diag.error(spec) << "Undefined symbol: " << spec->templateExpr();
      dumpScopeHierarchy();
      return false;
    }

    Expr * expr = resolveSpecialization(loc, lvals, spec->args());
    if (expr == NULL) {
      diag.error(spec) << "No template found matching expression: " << spec->templateExpr();
      return false;
    }

    out.push_back(expr);
    return true;
  } else {
    // It's not a name or anything like that.
    path.clear();

    // See if it's an expression.
    ExprAnalyzer ea(module, activeScope);
    ea.setSourceDefn(sourceDefn);
    //Expr * result = ea.inferTypes(ea.reduceExpr(ast, NULL), NULL);
    Expr * result = ea.reduceExpr(ast, NULL);
    if (!isErrorResult(result)) {
      out.push_back(result);
      return true;
    }

    return false;
  }
}

bool AnalyzerBase::lookupIdent(ExprList & out, const char * name, SLC & loc) {
  // Search the current active scopes.
  for (Scope * sc = activeScope; sc != NULL; sc = sc->parentScope()) {
    if (findInScope(out, name, sc, sc->baseExpr(), loc)) {
      return true;
    }
  }

  return false;
}

bool AnalyzerBase::findMemberOf(ExprList & out, Expr * context, const char * name, SLC & loc) {
  if (ScopeNameExpr * scopeName = dyn_cast<ScopeNameExpr>(context)) {
    if (Module * m = dyn_cast<Module>(scopeName->value())) {
      AnalyzerBase::analyzeDefn(m, Task_PrepMemberLookup);
      if (findInScope(out, name, m, NULL, loc)) {
        return true;
      }
    } else if (NamespaceDefn * ns = dyn_cast<NamespaceDefn>(scopeName->value())) {
      AnalyzerBase::analyzeDefn(ns, Task_PrepMemberLookup);
      if (findInScope(out, name, &ns->memberScope(), NULL, loc)) {
        return true;
      }
    }
  }

  if (LValueExpr * lvalue = dyn_cast<LValueExpr>(context)) {
    Type * type = inferType(lvalue->value());
    if (type == NULL) {
      return false;
    }

    TypeDefn * typeDef = dealias(type)->typeDefn();
    if (typeDef != NULL && type->memberScope() != NULL) {
      DASSERT_OBJ(typeDef->isSingular(), typeDef);
      AnalyzerBase::analyzeTypeDefn(typeDef, Task_PrepMemberLookup);
      if (findInScope(out, name, type->memberScope(), context, loc)) {
        return true;
      }
    }
  } else if (TypeLiteralExpr * ctype = dyn_cast<TypeLiteralExpr>(context)) {
    TypeDefn * typeDef = dealias(ctype->value())->typeDefn();
    if (typeDef != NULL && ctype->value()->memberScope() != NULL) {
      if (typeDef->isTemplate()) {
        return findStaticTemplateMember(out, typeDef, name, loc);
      }

      DASSERT_OBJ(ctype->isSingular(), typeDef);
      AnalyzerBase::analyzeTypeDefn(typeDef, Task_PrepMemberLookup);
      if (findInScope(out, name, ctype->value()->memberScope(), context, loc)) {
        return true;
      }
    }
  } else if (context->type() != NULL) {
    TypeDefn * typeDef = dealias(context->type())->typeDefn();
    if (typeDef != NULL && context->type()->memberScope() != NULL) {
      if (typeDef->typeValue()->isUnsizedIntType()) {
        diag.error(loc) << "Attempt to access member of integer of unknown size";
        return false;
      }

      DASSERT_OBJ(typeDef->isSingular(), typeDef);
      AnalyzerBase::analyzeTypeDefn(typeDef, Task_PrepMemberLookup);
      if (findInScope(out, name, context->type()->memberScope(), context, loc)) {
        return true;
      }
    }
  }

  return false;
}

bool AnalyzerBase::findInScope(ExprList & out, const char * name, Scope * scope, Expr * context,
    SLC & loc) {
  DefnList defns;
  if (scope->lookupMember(name, defns, true)) {
    return getDefnListAsExprList(loc, defns, context, out);
  }

  return false;
}

bool AnalyzerBase::findStaticTemplateMember(ExprList & out, TypeDefn * typeDef, const char * name,
    SLC & loc) {
  DefnList defns;
  if (lookupTemplateMember(defns, typeDef, name, loc)) {
    int numStaticDefns = 0;
    for (DefnList::const_iterator it = defns.begin(); it != defns.end(); ++it) {
      Defn * de = *it;
      if (de->storageClass() == Storage_Static) {
        ++numStaticDefns;
        out.push_back(getDefnAsExpr(de, NULL, loc));
      }
    }

    if (numStaticDefns > 0) {
      return true;
    }

    if (defns.size() > 0) {
      diag.error(loc) << "Invalid reference to non-static member '" << name << "' of type '" <<
          typeDef << "'.";
    }

    return false;
  }

  return false;
}

bool AnalyzerBase::lookupTemplateMember(DefnList & out, TypeDefn * typeDef, const char * name,
    SLC & loc) {
  AnalyzerBase::analyzeTypeDefn(typeDef, Task_PrepMemberLookup);
  if (typeDef->beginPass(Pass_CreateMembers)) {
    if (typeDef->ast() != NULL) {
      ScopeBuilder::createScopeMembers(typeDef);
    }

    typeDef->finishPass(Pass_CreateMembers);
  }

  if (CompositeType * ctype = dyn_cast<CompositeType>(typeDef->typeValue())) {
    if (ctype->memberScope()->lookupMember(name, out, false)) {
      return true;
    }

    const ASTTypeDecl * ast = cast_or_null<const ASTTypeDecl>(ctype->typeDefn()->ast());
    if (ast == NULL) {
      return false;
    }

    const ASTNodeList & astBases = ast->bases();
    if (!astBases.empty()) {
      DFAIL("Implement search of template base classes");
    }
  }

  return false;
}

Expr * AnalyzerBase::resolveSpecialization(SLC & loc, const ExprList & exprs,
    const ASTNodeList & args) {
  TypeList argList; // Template args, not function args.
  bool isSingularArgList = true;  // True if all args are fully resolved.

  // Resolve all the arguments. Note that we don't support type inference on template args,
  // so the resolution is relatively straightforward.
  ExprAnalyzer ea(module, activeScope);
  ea.setSourceDefn(sourceDefn);
  for (ASTNodeList::const_iterator it = args.begin(); it != args.end(); ++it) {
    ConstantExpr * cb = ea.reduceConstantExpr(*it, NULL);
    if (isErrorResult(cb)) {
      return NULL;
    }

    Type * typeArg = NULL;
    if (TypeLiteralExpr * ctype = dyn_cast<TypeLiteralExpr>(cb)) {
      typeArg = dealias(ctype->value());
      if (TypeDefn * tdef = typeArg->typeDefn()) {
        typeArg = tdef->typeValue();
      }
    }

    if (typeArg == NULL) {
      typeArg = NonTypeConstant::get(cb);
    }

    if (!cb->isSingular()) {
      isSingularArgList = false;
    }

    argList.push_back(typeArg);
  }

  // Examine all of the possible candidates for specialization.
  SpCandidateSet candidates;
  for (ExprList::const_iterator it = exprs.begin(); it != exprs.end(); ++it) {
    if (TypeLiteralExpr * tref = dyn_cast<TypeLiteralExpr>(*it)) {
      Type * type = dealias(tref->value());
      TypeDefn * typeDefn = type->typeDefn();
      if (typeDefn != NULL) {
        if (typeDefn->isTemplate() || typeDefn->isTemplateInstance()) {
          addSpecCandidate(loc, candidates, NULL, typeDefn, argList);
        }
      } else if (AddressType * np = dyn_cast<AddressType>(type)) {
        addSpecCandidate(loc, candidates, NULL, &AddressType::typedefn, argList);
      } else if (NativePointerType * np = dyn_cast<NativePointerType>(type)) {
        addSpecCandidate(loc, candidates, NULL, &NativePointerType::typedefn, argList);
      } else if (NativeArrayType * np = dyn_cast<NativeArrayType>(type)) {
        addSpecCandidate(loc, candidates, NULL, &NativeArrayType::typedefn, argList);
      }
    } else if (LValueExpr * lv = dyn_cast<LValueExpr>(*it)) {
      ValueDefn * val = lv->value();
      if (val->isTemplate() || val->isTemplateInstance()) {
        addSpecCandidate(loc, candidates, lv->base(), val, argList);
      }
    }
  }

  if (candidates.empty()) {
    diag.error(loc) << "No templates found which match template arguments [" << args << "]";
    for (ExprList::const_iterator it = exprs.begin(); it != exprs.end(); ++it) {
      diag.info(*it) << Format_Type << "candidate: " << *it;
    }

    ea.dumpScopeHierarchy();
    return NULL;
  }

  //if (!isSingularArgList) {
  //  return new
  //}

  // TODO: Do template overload resolution.
  // TODO: Use parameter assignments.
  if (candidates.size() == 1) {
    SpecializeCandidate * spc = *candidates.begin();
    Defn * defn = spc->templateDefn();
    TemplateSignature * tsig = spc->templateDefn()->templateSignature();
    if (TypeDefn * typeDefn = dyn_cast<TypeDefn>(defn)) {
      Type * type = tsig->instantiateType(loc, spc->env());
      if (type != NULL) {
        return new TypeLiteralExpr(loc, type);
      }
    }

    defn = tsig->instantiate(loc, spc->env());
    if (defn != NULL) {
      return getDefnAsExpr(defn, spc->base(), loc);
    }
  } else {
    DFAIL("Implement");
  }
}

void AnalyzerBase::addSpecCandidate(SLC & loc, SpCandidateSet & spcs, Expr * base, Defn * defn,
    TypeList & args) {
  if (defn->isTemplate()) {
    DefnAnalyzer::analyzeTemplateSignature(defn);
    const TemplateSignature * tsig = defn->templateSignature();
    if (tsig->params().size() == args.size()) {
      // Attempt unification of pattern variables with template args.
      SpecializeCandidate * spc = new SpecializeCandidate(base, defn);
      SourceContext candidateSite(defn->location(), NULL, defn, Format_Type);
      if (spc->unify(&candidateSite, args)) {
        spcs.insert(spc);
      }
    }
  } else if (defn->isTemplateInstance()) {
    DefnList defns;
    defn->templateInstance()->parentScope()->lookupMember(defn->name(), defns, true);
    for (DefnList::iterator it = defns.begin(); it != defns.end(); ++it) {
      Defn * d = *it;
      if (d->isTemplate()) {
        addSpecCandidate(loc, spcs, base, d, args);
      }
    }
  } else {
    DFAIL("Not a template");
  }
}

bool AnalyzerBase::importName(ExprList & out, const std::string & path, bool absPath, SLC & loc) {
  if (module != NULL) {
    DefnList defns;
    if (module->import(path.c_str(), defns, absPath)) {
      return getDefnListAsExprList(loc, defns, NULL, out);
    }
  }

  //Module * m = PackageMgr::get().getModuleForImportPath(path);
  //if (m != NULL) {
  //  out.push_back(new ScopeNameExpr(loc, m));
  //  return true;
  //}

  return false;
}

bool AnalyzerBase::getDefnListAsExprList(SLC & loc, DefnList & defs, Expr * context,
    ExprList & out) {
  for (DefnList::iterator it = defs.begin(); it != defs.end(); ++it) {
    if (ExplicitImportDefn * imp = dyn_cast<ExplicitImportDefn>(*it)) {
      // TODO: Should we allow mixing of import defns with non-imports of
      // the same name?
      ExprList & importedDefns = imp->importValues();
      out.append(importedDefns.begin(), importedDefns.end());
    } else {
      out.push_back(getDefnAsExpr(*it, context, loc));
    }
  }

  return !out.empty();
}

Expr * AnalyzerBase::getDefnAsExpr(Defn * de, Expr * context, SLC & loc) {
  if (TypeDefn * tdef = dyn_cast<TypeDefn>(de)) {
    return tdef->asExpr();
  } else if (ValueDefn * vdef = dyn_cast<ValueDefn>(de)) {
    if (vdef->storageClass() == Storage_Instance && context == NULL) {
      diag.fatal(loc) << "Cannot access non-static member '" <<
          vdef->name() << "' from static method.";
    }

    LValueExpr * result = new LValueExpr(loc, context, vdef);

    // If it's a variadic parameter, then the actual type is an array of the declared type.
    if (ParameterDefn * param = dyn_cast<ParameterDefn>(vdef)) {
      DASSERT_OBJ(param->internalType().isDefined(), param);
      result->setType(param->internalType());
    }

    return result;
  } else if (NamespaceDefn * ns = dyn_cast<NamespaceDefn>(de)) {
    return new ScopeNameExpr(loc, ns);
  } else if (Module * m = dyn_cast<Module>(de)) {
    return new ScopeNameExpr(loc, m);
  } else {
    diag.fatal(de) << Format_Verbose << de;
    DFAIL("IllegalState");
  }
}

bool AnalyzerBase::getTypesFromExprs(SLC & loc, ExprList & in, TypeList & out) {
  int numNonTypes = 0;
  for (ExprList::iterator it = in.begin(); it != in.end(); ++it) {
    if (TypeLiteralExpr * tle = dyn_cast<TypeLiteralExpr>(*it)) {
      out.push_back(tle->value());
    } else {
      numNonTypes++;
    }
  }

  if (out.empty()) {
    return false;
  }

  if (numNonTypes > 0) {
    diag.fatal(loc) << "Incompatible definitions for '" << out.front() << "'";
    for (ExprList::iterator it = in.begin(); it != in.end(); ++it) {
      diag.info(*it) << *it;
    }
  }

  return true;
}

Type * AnalyzerBase::inferType(ValueDefn * valueDef) {
  if (!valueDef->type().isDefined()) {
    if (!analyzeDefn(valueDef, Task_InferType)) {
      return NULL;
    }
  }

  if (valueDef->type().isDefined() && valueDef->type().isSingular()) {
    if (ParameterDefn * param = dyn_cast<ParameterDefn>(valueDef)) {
      return param->internalType().type();
    }

    return valueDef->type().type();
  }

  if (valueDef->type().isDefined()) {
    diag.info(valueDef) << valueDef << ":" << valueDef->type();
  } else {
    diag.info(valueDef) << valueDef;
  }

  DFAIL("Failed to determine type of value.");
}

bool AnalyzerBase::analyzeType(Type * in, AnalysisTask pass) {
  if (in != NULL) {
    TypeDefn * de = in->typeDefn();
    if (de != NULL) {
      return analyzeTypeDefn(de, pass);
    }
  }

  return true;
}


bool AnalyzerBase::analyzeType(const TypeRef & in, AnalysisTask pass) {
  return analyzeType(in.type(), pass);
}

void AnalyzerBase::analyzeTypeLater(Type * in) {
  if (in != NULL && in->isSingular()) {
    TypeDefn * de = in->typeDefn();
    if (de != NULL) {
      analyzeLater(de);
    } else {
      size_t numTypeParams = in->numTypeParams();
      for (size_t n = 0; n < numTypeParams; ++n) {
        analyzeTypeLater(in->typeParam(n));
      }
    }
  }
}

bool AnalyzerBase::analyzeModule(Module * mod) {
  DefnAnalyzer da(mod, mod);
  return da.analyzeModule();
}

bool AnalyzerBase::analyzeDefn(Defn * in, AnalysisTask task) {
  switch (in->defnType()) {
    case Defn::Typedef:
      return analyzeTypeDefn(static_cast<TypeDefn *>(in), task);

    case Defn::Namespace:
      return analyzeNamespace(static_cast<NamespaceDefn *>(in), task);

    case Defn::Var:
    case Defn::Let:
    case Defn::Parameter:
    case Defn::TemplateParam: {
      return VarAnalyzer(static_cast<ValueDefn *>(in)).analyze(task);
    }

    case Defn::Property:
    case Defn::Indexer: {
      return PropertyAnalyzer(static_cast<PropertyDefn *>(in)).analyze(task);
    }

    case Defn::Function:
    case Defn::Macro: {
      return FunctionAnalyzer(static_cast<FunctionDefn *>(in)).analyze(task);
    }

    case Defn::Mod: {
      Module * m = static_cast<Module *>(in);
      return DefnAnalyzer(m, m).analyzeModule();
    }

    case Defn::ExplicitImport:
      return true;
      //DFAIL("IllegalState");

    case Defn::DefnTypeCount:
    default:
      DFAIL("IllegalState");
  }
}

bool AnalyzerBase::analyzeTypeDefn(TypeDefn * in, AnalysisTask pass) {
  Type * type = in->typeValue();
  switch (type->typeClass()) {
    case Type::Primitive:
      return true;

    case Type::Class:
    case Type::Struct:
    case Type::Interface:
    case Type::Protocol: {
      return ClassAnalyzer(in).analyze(pass);
    }

    case Type::Enum:
      return EnumAnalyzer(in).analyze();

    case Type::NativePointer: {
      NativePointerType * np = static_cast<NativePointerType *>(type);
      analyzeTypeLater(np->typeParam(0));
      return true;
    }

    case Type::Address: {
      AddressType * ma = static_cast<AddressType *>(type);
      analyzeTypeLater(ma->typeParam(0));
      return true;
    }

    case Type::NativeArray: {
      NativeArrayType * na = static_cast<NativeArrayType *>(type);
      analyzeTypeLater(na->typeParam(0));
      return true;
    }

    case Type::Alias:
      // TODO: Analyze what we are pointing to.
      return true;

    case Type::Pattern:
      return true;

    default:
      diag.debug(in) << in;
      DFAIL("IllegalState");
  }
}

bool AnalyzerBase::analyzeValueDefn(ValueDefn * in, AnalysisTask pass) {
  return analyzeDefn(in, pass);
}

void AnalyzerBase::analyzeLater(Defn * de) {
  DASSERT_OBJ(de->isSingular(), de);
  if (queue.insert(de)) {
    //diag.debug() << de << " added to queue";
  }
}

void AnalyzerBase::flushAnalysisQueue() {
  while (queuePos < queue.size()) {
    Defn * de = queue[queuePos++];
    analyzeDefn(de, Task_PrepCodeGeneration);
  }
}

bool AnalyzerBase::analyzeNamespace(NamespaceDefn * in, AnalysisTask task) {
  // Analyze namespace imports.

  DefnPasses passesToRun;
  switch (task) {
    case Task_PrepMemberLookup:
    case Task_PrepCallOrUse:
      DefnAnalyzer::addPass(in, passesToRun, Pass_ResolveImport);
      DefnAnalyzer::addPass(in, passesToRun, Pass_CreateMembers);
      break;

    case Task_PrepCodeGeneration:
      DefnAnalyzer::addPass(in, passesToRun, Pass_ResolveImport);
      DefnAnalyzer::addPass(in, passesToRun, Pass_CreateMembers);
      DefnAnalyzer::addPass(in, passesToRun, Pass_ResolveStaticInitializers);
      break;
  }

  if (in->beginPass(Pass_ResolveImport)) {
    if (in->ast() != NULL) {
      DefnAnalyzer da(in->module(), &in->memberScope());
      const ASTNodeList & imports = in->ast()->imports();
      for (ASTNodeList::const_iterator it = imports.begin(); it != imports.end(); ++it) {
        da.importIntoScope(cast<ASTImport>(*it), &in->memberScope());
      }
    }

    in->finishPass(Pass_ResolveImport);
  }

  if (in->beginPass(Pass_CreateMembers)) {
    if (in->ast() != NULL) {
      ScopeBuilder::createScopeMembers(in);
    }

    in->finishPass(Pass_CreateMembers);
  }

  if (in->beginPass(Pass_ResolveStaticInitializers)) {
    for (Defn * m = in->memberScope().firstMember(); m != NULL; m = m->nextInScope()) {
      DefnAnalyzer da(in->module(), &in->memberScope());
      da.analyzeDefn(m, Task_PrepCodeGeneration);
      if (m->isSingular()) {
        in->module()->addSymbol(m);
      }
    }

    in->finishPass(Pass_ResolveStaticInitializers);
  }

  return true;
}

CompositeType * AnalyzerBase::getArrayTypeForElement(Type * elementType) {
  // Look up the array class
  TemplateSignature * arrayTemplate = Builtins::typeArray->typeDefn()->templateSignature();

  // Do analysis on template if needed.
  if (arrayTemplate->ast() != NULL) {
    DefnAnalyzer da(&Builtins::module, &Builtins::module);
    da.analyzeTemplateSignature(Builtins::typeArray->typeDefn());
  }

  DASSERT_OBJ(arrayTemplate->paramScope().count() == 1, elementType);

  // Special case for when the elementType is Array.ElementType
  if (elementType == arrayTemplate->params()[0]) {
    return cast<CompositeType>(Builtins::typeArray);
  }

  BindingEnv arrayEnv(arrayTemplate);
  arrayEnv.addSubstitution(arrayTemplate->patternVar(0), elementType);
  return cast<CompositeType>(cast<TypeDefn>(
      arrayTemplate->instantiate(SourceLocation(), arrayEnv))->typeValue());
}

ArrayLiteralExpr * AnalyzerBase::createArrayLiteral(SLC & loc, const TypeRef & elementType) {
  CompositeType * arrayType = getArrayTypeForElement(elementType.type());
  ArrayLiteralExpr * array = new ArrayLiteralExpr(loc);
  array->setType(arrayType);

  return array;
}

// Determine if the target is able to be accessed from the current source defn.
void AnalyzerBase::checkAccess(const SourceLocation & loc, Defn * target) {
  if (!canAccess(sourceDefn, target)) {
    diag.fatal(loc) << "'" << target->name() << "' is " <<
        (target->visibility() == Protected ? "protected." : "private.");
  }
}

void AnalyzerBase::checkAccess(const SourceLocation & loc, Defn * source, Defn * target) {
  if (!canAccess(source, target)) {
    diag.fatal(loc) << "'" << target->name() << "' is " <<
        (target->visibility() == Protected ? "protected." : "private.");
  }
}

/** The set of definition types that represent namespaces that can grant access to their
    members. */
static const DefnTypeSet ACCESS_CONTEXTS = DefnTypeSet::of(
    Defn::Typedef, Defn::Namespace, Defn::Mod);

bool AnalyzerBase::canAccess(Defn * source, Defn * target) {
  if (target->storageClass() == Storage_Local || target->defnType() == Defn::Parameter) {
    return true;
  }

  if (target->visibility() != Public) {
    // The destination context is the scope that defines who can see the visible symbol.
    // This is never the symbol itself - it is the next outer name-space-like symbol.
    Defn * dstContext = target;
    if ((dstContext == target || !ACCESS_CONTEXTS.contains(dstContext->defnType()))
        && dstContext->parentDefn() != NULL) {
      dstContext = dstContext->parentDefn();
    }

    if (source != NULL) {
      for (Defn * de = source; de != NULL; de = de->parentDefn()) {
        if (de == dstContext) {
          return true;
        }
      }

      if (target->visibility() == Protected) {
        if (TypeDefn * dstTypeDef = dyn_cast<TypeDefn>(dstContext)) {
          if (CompositeType * dstType = dyn_cast_or_null<CompositeType>(dstTypeDef->typeValue())) {
            for (Defn * de = source; de != NULL; de = de->parentDefn()) {
              if (TypeDefn * srcTypeDef = dyn_cast<TypeDefn>(de)) {
                if (CompositeType * srcType =
                    dyn_cast_or_null<CompositeType>(srcTypeDef->typeValue())) {
                  if (srcType->isSubclassOf(dstType)) {
                    return true;
                  }
                }
              }
            }
          }
        }
      }
    }

    return false;
  }

  return true;
}

void AnalyzerBase::dumpScopeHierarchy() {
  int level = diag.getIndentLevel();
  for (Scope * s = activeScope; s != NULL; s = s->parentScope()) {
    s->dumpHierarchy(true);
    diag.indent();
  }

  diag.setIndentLevel(level);
  diag.recovered();
}

} // namespace tart

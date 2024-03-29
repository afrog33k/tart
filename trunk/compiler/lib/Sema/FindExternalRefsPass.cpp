/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/Expr/Exprs.h"
#include "tart/Type/PrimitiveType.h"
#include "tart/Type/CompositeType.h"
#include "tart/Type/FunctionType.h"
#include "tart/Defn/FunctionDefn.h"
#include "tart/Defn/TypeDefn.h"
#include "tart/Defn/Template.h"
#include "tart/Defn/Module.h"
#include "tart/Type/EnumType.h"

#include "tart/Sema/FindExternalRefsPass.h"
#include "tart/Sema/CallCandidate.h"
#include "tart/Sema/AnalyzerBase.h"

#include "tart/Common/Diagnostics.h"

#include "tart/Objects/Builtins.h"
#include "tart/Objects/Intrinsics.h"
#include "tart/Objects/SystemDefs.h"

namespace tart {

/// -------------------------------------------------------------------
/// FindExternalRefsPass

Defn * FindExternalRefsPass::run(Module * m, Defn * in) {
  FindExternalRefsPass instance(m);
  return instance.runImpl(in);
}

void FindExternalRefsPass::visit(FunctionDefn * in) {
  function_ = in;
  CFGPass::visit(in);
  function_ = NULL;
}

Defn * FindExternalRefsPass::runImpl(Defn * in) {
  if (TypeDefn * tdef = dyn_cast<TypeDefn>(in)) {
    if (const CompositeType * ctype = dyn_cast<CompositeType>(tdef->typePtr())) {
      if (ctype->typeClass() == Type::Interface || ctype->typeClass() == Type::Protocol) {
        return in;
      }

      if (tdef->isSynthetic()) {
        ctype->addClassExportsToModule(module_);
      }

      for (Defn * de = ctype->firstMember(); de != NULL; de = de->nextInScope()) {
        if (de->isSingular() && de->storageClass() == Storage_Static) {
          // Add static members
          module_->addSymbol(de);
        } else if (TypeDefn * td = dyn_cast<TypeDefn>(de)) {
          // Add inner types
          module_->addSymbol(td);
        }
      }

      const DefnList & staticFields = ctype->staticFields();
      for (DefnList::const_iterator it = staticFields.begin(); it != staticFields.end(); ++it) {
        if (VariableDefn * var = dyn_cast<VariableDefn>(*it)) {
          if (var->initValue()) {
            visitExpr(var->initValue());
          }
        }
      }
    }
  } else if (FunctionDefn * fn = dyn_cast<FunctionDefn>(in)) {
    if (!fn->isIntrinsic() && !fn->isExtern()) {
      visit(fn);
      if (fn->isReflected()) {
        addTypeRef(fn->type().unqualified());
      }

      fn = fn->mergeTo();
      if (fn != NULL) {
        visit(fn);
        if (fn->isReflected()) {
          addTypeRef(fn->type().unqualified());
        }
      }
    }
  } else if (VariableDefn * var = dyn_cast<VariableDefn>(in)) {
    if (var->initValue()) {
      visitExpr(var->initValue());
    }
  }

  return in;
}

void FindExternalRefsPass::addSymbol(Defn * de) {
  module_->addModuleDependency(de);

  if (FunctionDefn * fn = dyn_cast<FunctionDefn>(de)) {
    addFunction(fn);
  } else if (de->storageClass() == Storage_Static || de->storageClass() == Storage_Global) {
    if (de->isSynthetic()) {
      module_->addSymbol(de);
    }
  } else if (de->storageClass() == Storage_Local) {
    if (VariableDefn * var = dyn_cast<VariableDefn>(de)) {
      if (var->initValue() != NULL) {
        visitExpr(var->initValue());
      }
    }
  }
}

bool FindExternalRefsPass::addTypeRef(const Type * type) {
  if (type->typeDefn() != NULL) {
    return module_->addSymbol(type->typeDefn());
  } else if (const FunctionType * fnType = dyn_cast<FunctionType>(type)) {
    for (ParameterList::const_iterator it = fnType->params().begin(); it != fnType->params().end();
        ++it) {
      addTypeRef((*it)->type().unqualified());
    }
  }

  return false;
}

bool FindExternalRefsPass::addFunction(FunctionDefn * fn) {
  if (!fn->isIntrinsic() && !fn->isExtern()) {
    if (fn->mergeTo()) {
      fn = fn->mergeTo();
    }
    AnalyzerBase::analyzeType(fn->type(), Task_PrepTypeGeneration);
    if (fn->module() != module_ && fn->module() != NULL) {
      module_->addModuleDependency(fn->module());
    }

    return module_->addSymbol(fn);
  }

  return false;
}

Expr * FindExternalRefsPass::visitLValue(LValueExpr * in) {
  visitExpr(in->base());
  addSymbol(in->value());
  return in;
}

Expr * FindExternalRefsPass::visitBoundMethod(BoundMethodExpr * in) {
  visitExpr(in->selfArg());
  addSymbol(in->method());
  return in;
}

Expr * FindExternalRefsPass::visitFnCall(FnCallExpr * in) {
  // If this is a memory allocation intrinsic, make sure alloc state is available.
  Intrinsic * intrinsic = in->function()->intrinsic();
  if (intrinsic != NULL) {
    if (intrinsic == &DefaultAllocIntrinsic::instance ||
        intrinsic == &FlexAllocIntrinsic::instance) {
      if (function_ != NULL) {
        function_->setFlag(FunctionDefn::MakesAllocs, true);
      }
    }
  }

  if (function_ != NULL) {
    function_->setFlag(FunctionDefn::HasSafePoints, true);
  }
  if (addFunction(in->function())) {
    CFGPass::visitFnCall(in);
  } else {
    if (in->selfArg() != NULL) {
      AnalyzerBase::analyzeType(in->selfArg()->type(), Task_PrepTypeGeneration);
    }
    visitExpr(in->selfArg());
    visitExprArgs(in);
  }

  return in;
}

Expr * FindExternalRefsPass::visitNew(NewExpr * in) {
  if (function_ != NULL) {
    function_->setFlag(FunctionDefn::MakesAllocs, true);
    function_->setFlag(FunctionDefn::HasSafePoints, true);
  }
  addTypeRef(in->type().unqualified());
  return in;
}

Expr * FindExternalRefsPass::visitConstantObjectRef(ConstantObjectRef * in) {
  const CompositeType * ctype = cast<CompositeType>(in->type().unqualified());
  module_->addSymbol(ctype->typeDefn());
  return CFGPass::visitConstantObjectRef(in);
}

Expr * FindExternalRefsPass::visitConstantEmptyArray(ConstantEmptyArray * in) {
  const CompositeType * ctype = cast<CompositeType>(in->type().unqualified());
  module_->addSymbol(ctype->typeDefn());
  return CFGPass::visitConstantEmptyArray(in);
}

Expr * FindExternalRefsPass::visitCast(CastExpr * in) {
  if (in->type()->typeDefn() != NULL) {
    module_->addSymbol(in->type()->typeDefn());
  }

  return CFGPass::visitCast(in);
}

Expr * FindExternalRefsPass::visitArrayLiteral(ArrayLiteralExpr * in) {
  const CompositeType * arrayType = cast<CompositeType>(in->type().unqualified());
  DASSERT(arrayType->passes().isFinished(CompositeType::ScopeCreationPass));
  Defn * allocFunc = arrayType->lookupSingleMember("alloc");
  DASSERT(allocFunc != NULL);
  addSymbol(arrayType->typeDefn());
  addSymbol(allocFunc);
  return in;
}

Expr * FindExternalRefsPass::visitTypeLiteral(TypeLiteralExpr * in) {
  TypeDefn * td = in->value()->typeDefn();
  if (td != NULL) {
    module_->addSymbol(td);
  }

  if (!in->value().isa<CompositeType>()) {
    if (td != NULL) {
      module_->reflect(td);
    }
    if (in->value().isa<PrimitiveType>()) {
      module_->addSymbol(Builtins::typePrimitiveType.typeDefn());
    }
  }

  return in;
}

Expr * FindExternalRefsPass::visitInstanceOf(InstanceOfExpr * in) {
  if (const CompositeType * cls = dyn_cast<CompositeType>(in->toType())) {
    addSymbol(cls->typeDefn());
  }

  return CFGPass::visitInstanceOf(in);
}

Expr * FindExternalRefsPass::visitClosureScope(ClosureEnvExpr * in) {
  if (function_ != NULL) {
    function_->setFlag(FunctionDefn::MakesAllocs, true);
    function_->setFlag(FunctionDefn::HasSafePoints, true);
  }
  return CFGPass::visitClosureScope(in);
}

} // namespace tart

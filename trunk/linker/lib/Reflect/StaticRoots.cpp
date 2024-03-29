/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "llvm/Module.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Support/raw_ostream.h"

#include "tart/Reflect/StaticRoots.h"

namespace tart {

char StaticRoots::ID = 0;

static RegisterPass<StaticRoots> X(
    "staticroots", "Generate table of static roots",
    false /* Only looks at CFG */,
    false /* Analysis Pass */);

StaticRoots::~StaticRoots() {
  // TODO: Delete what we created.
}

void StaticRoots::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.setPreservesCFG();
}

bool StaticRoots::runOnModule(llvm::Module & module) {
  LLVMContext & context = module.getContext();
  GlobalVariable * gcStaticRoots = module.getGlobalVariable("GC_static_roots", true);
  GlobalVariable * gcStaticRootsArray = module.getGlobalVariable("GC_static_roots_array", true);

  if (gcStaticRoots == NULL) {
    errs() << "Can't find GC_static_roots pointer\n";
    abort();
  }

  StructType * rootType = module.getTypeByName("tart.gc.StaticRoot");
  if (rootType == NULL) {
    errs() << "Can't find type: tart.gc.StaticRoot\n";
    abort();
  }

  Type * int8PtrTy = rootType->getContainedType(0);
  Type * traceDescriptorPtrTy = rootType->getContainedType(1);

  std::vector<Constant *> rootStructs;
  Constant * rootStruct;
  Constant * members[2];
  for (Module::const_named_metadata_iterator
      it = module.named_metadata_begin(), itEnd = module.named_metadata_end(); it != itEnd; ++it) {
    const NamedMDNode & node = *it;
    if (node.getName().startswith("roots.")) {
      for (size_t i = 0; i < node.getNumOperands(); ++i) {
        MDNode * m = node.getOperand(i);
        if (m->getNumOperands() == 2) {
          GlobalVariable * gv = cast_or_null<GlobalVariable>(m->getOperand(0));
          Constant * traceTable = cast_or_null<Constant>(m->getOperand(1));
          if (gv != NULL) {
            if (traceTable == NULL) {
              //outs() << "Null Root: " << gv->getName() << "\n";
            } else {
              //outs() << "Root: " << gv->getName() << "\n";
              members[0] = llvm::ConstantExpr::getPointerCast(gv, int8PtrTy);
              members[1] = traceTable;
              rootStruct = ConstantStruct::get(rootType, members);
              rootStructs.push_back(rootStruct);
            }
          }
        }
      }
    }
  }

  members[0] = llvm::ConstantPointerNull::get(cast<PointerType>(int8PtrTy));
  members[1] = llvm::ConstantPointerNull::get(cast<PointerType>(traceDescriptorPtrTy));
  rootStruct = ConstantStruct::get(rootType, members);
  rootStructs.push_back(rootStruct);

  Constant * rootArray = ConstantArray::get(
      ArrayType::get(rootType, rootStructs.size()),
      rootStructs);

  // Create a replacement array
  GlobalVariable * gcStaticRootsArrayInternal = new GlobalVariable(
      module, rootArray->getType(), true, GlobalValue::InternalLinkage,
      rootArray, "GC_static_roots_internal");
  Constant * indices[2];
  indices[0] = indices[1] = ConstantInt::get(llvm::Type::getInt32Ty(context), 0);
  gcStaticRoots->setInitializer(
      llvm::ConstantExpr::getGetElementPtr(gcStaticRootsArrayInternal, indices, true));

  if (gcStaticRootsArray != NULL) {
    gcStaticRootsArray->eraseFromParent();
  }
  return true;
}

} // namespace tart

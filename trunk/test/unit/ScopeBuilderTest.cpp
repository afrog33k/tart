/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include <gtest/gtest.h>
#include "tart/AST/ASTDecl.h"
#include "tart/Defn/Module.h"
#include "tart/Defn/TypeDefn.h"
#include "tart/Objects/Builtins.h"
#include "tart/Sema/ScopeBuilder.h"
#include "tart/Common/Diagnostics.h"
#include "FakeSourceFile.h"
#include "TestHelpers.h"

using namespace tart;

class ScopeBuilderTest : public testing::Test {
protected:
  Module testModule;
  ScopeBuilder builder;

public:
  ScopeBuilderTest()
    : testModule("test.test", &Builtins::module)
    , builder()
  {
    testModule.setModuleSource(new FakeSourceFile(""));
  }


  virtual void SetUp() {}
  virtual void TearDown() {}

  ASTTypeDecl * createTestClassDecl(const char * name) {
    return new ASTTypeDecl(ASTNode::Class, SourceLocation(), name, ASTNodeList(),
        DeclModifiers());
  }

  bool hasMember(Defn * de, const char * name) {
    if (TypeDefn * td = dyn_cast<TypeDefn>(de)) {
        return td->typePtr()->memberScope()->lookupSingleMember(
            name, false) != NULL;
    }

    return false;
  }
};

TEST_F(ScopeBuilderTest, TestAddMember) {
  ASTTypeDecl * cl1 = createTestClassDecl("cl1");
  Defn * cl1Defn = builder.createMemberDefn(&testModule, &testModule, cl1, Storage_Global);

  ASSERT_EQ(Defn::Typedef, cl1Defn->defnType());
  ASSERT_EQ("cl1", cl1Defn->name());
  ASSERT_EQ(&testModule, cl1Defn->module());
  ASSERT_EQ(&testModule, cl1Defn->definingScope());
  //ASSERT_EQ(&testModule, cl1Defn->parentDefn());
  ASSERT_EQ("test.cl1", cl1Defn->qualifiedName());
  ASSERT_EQ(cl1Defn, testModule.lookupSingleMember("cl1", false));

  DefnList defs;
  bool success = testModule.lookupMember("cl1", defs, false);
  ASSERT_TRUE(success);
  ASSERT_EQ(1u, defs.size());
  ASSERT_EQ(cl1Defn, defs[0]);
}

TEST_F(ScopeBuilderTest, TestNameConflict) {

  ASTTypeDecl * cl1 = createTestClassDecl("cl1");
  ASTTypeDecl * cl2 = createTestClassDecl("cl2");
  ASTTypeDecl * cl3 = createTestClassDecl("cl2");

  // Adding two classes with dissimilar names should not produce an error
  Defn * cl1Defn = builder.createMemberDefn(&testModule, &testModule, cl1, Storage_Global);
  Defn * cl2Defn = builder.createMemberDefn(&testModule, &testModule, cl2, Storage_Global);
  (void)cl1Defn;
  (void)cl2Defn;
  EXPECT_EQ(0, diag.getErrorCount());

  // But adding a class with the same name should produce an error
  diag.setMinSeverity(Diagnostics::Off);
  Defn * cl3Defn = builder.createMemberDefn(&testModule, &testModule, cl3, Storage_Global);
  (void)cl3Defn;
  builder.checkNameConflicts(&testModule);
  EXPECT_EQ(1, diag.getErrorCount());
  diag.reset();
  diag.setMinSeverity(Diagnostics::Debug);

  // TODO: Add test with overloaded function names
}

TEST_F(ScopeBuilderTest, CreateMembers) {
  ASTTypeDecl * cldecl = createTestClassDecl("cl");
  ASTTypeDecl * innerdecl = createTestClassDecl("inner");
  cldecl->members().push_back(innerdecl);

  Defn * cl = builder.createMemberDefn(&testModule, &testModule, cldecl, Storage_Global);
  DASSERT(cl->definingScope() != NULL);
  builder.createScopeMembers(cl);
  ASSERT_TRUE(hasMember(cl, "inner"));
}

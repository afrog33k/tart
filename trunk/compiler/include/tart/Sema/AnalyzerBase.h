/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#ifndef TART_SEMA_ANALYZERBASE_H
#define TART_SEMA_ANALYZERBASE_H

#ifndef TART_CFG_DEFN_H
#include "tart/CFG/Defn.h"
#endif

#include <llvm/ADT/SetVector.h>

namespace tart {

class Module;
class Scope;
class Expr;
class SourceLocation;
class NamespaceDefn;
class ArrayLiteralExpr;

typedef llvm::SmallSetVector<SpecializeCandidate *, 8> SpCandidateSet;

/// -------------------------------------------------------------------
/// Represents the set of possible operations that are done on a definition.
/// Since definitions are evaluated lazily, we need to ensure that the
/// appropriate analysis has been performed on the definition before the
/// task can begin. Each analysis task requires one or more analysis passes
/// to be run on the definition
enum AnalysisTask {
  Task_PrepTypeComparison,    // Prepare to compare with other types.
  Task_PrepMemberLookup,      // Prepare the definition for member lookup.
  Task_PrepOverloadSelection, // Prepare type information for selection.
  Task_PrepCallOrUse,         // Prepare a field for calling or using.
  Task_PrepCodeGeneration,    // Prepare for code generation.
  Task_InferType,             // Do type inference
};

/// -------------------------------------------------------------------
/// Base class of analyzers. Contains the machinery needed to do
/// qualified and unqualified name lookups.
class AnalyzerBase {
public:
  /** Constructor. */
  AnalyzerBase(Module * mod, Scope * parent)
    : module(mod)
    , activeScope(parent)
    , sourceDefn(NULL)
  {}

  /** Replace the current active scope with a new scope. Returns the old
      scope. */
  Scope * setActiveScope(Scope * newScope) {
    Scope * prevScope = activeScope;
    activeScope = newScope;
    return prevScope;
  }

  /** Represents the current scope from which accesses are being made. Used to check
      whether private/protected variables can be seen. */
  void setSourceDefn(Defn * defn) { sourceDefn = defn; }

  /** This method accepts an AST representing either an identifier or
      a dotted path of the form 'a.b.c'. It will attempt to resolve the
      path and return a list of matching definitions (there can be more
      than one result if the definition is overloaded).

      The method will attempt to look up the path in the following ways:

      -- In the current scope.
      -- In the scopes enclosing the current scope.
      -- In the scopes inherited by the current and enclosing scopes.
      -- By doing an implicit import from the current package.
      -- By doing an implicit import from tart.core.
      -- By doing an explicit import using an absolute package path.

      However, if 'absPath' is true, then only the absolute package path
      method will be used.
  */
  bool lookupName(ExprList & out, const ASTNode * ast, bool absPath = false);

  /** Given a list of expression, ensures that they are either all types or
      that none of them are (an error message is emitted otherwise.) If they
      are all types, then the type definitions are added to the output list. */
  static bool getTypesFromExprs(SLC & loc, ExprList & in, TypeList & out);

  /** Given a value definition, infer its type. */
  Type * inferType(ValueDefn * valueDef);

  /** Do the requested analysis passes on the type. */
  static bool analyzeType(Type * in, AnalysisTask pass);

  /** Do the requested analysis passes on the type. */
  static bool analyzeType(const TypeRef & in, AnalysisTask pass);

  /** Do the a complete analysis of the module. */
  static bool analyzeModule(Module * mod);

  /** Do the requested analysis passes on the definition. */
  static bool analyzeDefn(Defn * in, AnalysisTask pass);

  /** Do the requested analysis passes on the type definition. */
  static bool analyzeValueDefn(ValueDefn * in, AnalysisTask pass);

  /** Queue a definition to be fully analyzed later. */
  static void analyzeLater(Defn * de);

  /** Queue a type to be fully analyzed later. */
  static void analyzeTypeLater(Type * in);

    /** Do the requested analysis pass on the namesapce. */
  static bool analyzeNamespace(NamespaceDefn * ns, AnalysisTask pass);

  /** Given an element type, return the corresponding array type. The element
      type must already have been fully resolved. */
  static CompositeType * getArrayTypeForElement(Type * elementType);

  /** Create an empty array literal, with elements of the specified type.
      Also add to the given module the external symbols needed to support
      construction of the array. */
  static ArrayLiteralExpr * createArrayLiteral(SLC & loc, const TypeRef & elementType);

  // Determine if the target is able to be accessed from the current source defn.
  void checkAccess(const SourceLocation & loc, Defn * target);
  static void checkAccess(const SourceLocation & loc, Defn * source, Defn * target);
  static bool canAccess(Defn * source, Defn * target);

  /** Dump the current set of search scopes. */
  void dumpScopeHierarchy();

protected:
  Module * module;
  Scope * activeScope;
  Defn * sourceDefn;

  typedef llvm::SmallSetVector<Defn *, 128> AnalysisQueue;

  // Queue of types to analyze
  static AnalysisQueue queue;

  // Position in queue. Can't use iterator because queue may reallocate.
  static size_t queuePos;

  // Finish up all unanalyzed definitions.
  static void flushAnalysisQueue();

  // Recursive name-lookup helper function
  bool lookupNameRecurse(ExprList & out, const ASTNode * ast, std::string & path, bool absPath);

  // Lookup an unqualified identifier in the current scope.
  bool lookupIdent(ExprList & out, const char * name, SLC & loc);

  // Look up a name in an explicit scope.
  bool findMemberOf(ExprList & out, Expr * context, const char * name, SLC & loc);

  // Find a name in a scope and return a list of matching expressions.
  bool findInScope(ExprList & out, const char * name, Scope * scope, Expr * context, SLC & loc);

  // Special lookup function for static members of templated types.
  bool findStaticTemplateMember(ExprList & out, TypeDefn * type, const char * name, SLC & loc);

  // Special lookup function for static members of templated types.
  bool lookupTemplateMember(DefnList & out, TypeDefn * typeDef, const char * name,
      SLC & loc);

  // Given a list of expressions, find which ones are LValues that have template parameters,
  // and attempt to specialize those templates.
  Expr * resolveSpecialization(SLC & loc, const ExprList & exprs, const ASTNodeList & args);

  // Add a candidate to the list of specializations being considered.
  void addSpecCandidate(SLC & loc, SpCandidateSet & spcs, Expr * base, Defn * de, TypeList & args);

    // Lookup helper function that attempts to load a module from 'path'.
  bool importName(ExprList & out, const std::string & path, bool absPath, SLC & loc);

  // Create a reference to a definition.
  Expr * getDefnAsExpr(Defn * de, Expr * context, SLC & loc);

  // Given a list of definitions produced by a symbol lookup, convert
  // each definition into an expression (or in the case of an imported
  // symbol, multiple expressions) representing a reference to the definition.
  bool getDefnListAsExprList(SLC & loc, DefnList & defs, Expr * context, ExprList & out);

  /** Do the requested analysis passes on the type definition. */
  static bool analyzeTypeDefn(TypeDefn * in, AnalysisTask pass);
};

} // namespace tart

#endif

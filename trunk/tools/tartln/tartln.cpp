/* ================================================================ *
   TART - A Sweet Programming Language.
 * ================================================================ */

#include "llvm/LinkAllVMCore.h"
#include "llvm/Linker.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"

#include "tart/Reflect/ReflectorPass.h"
#include "tart/Reflect/StaticRoots.h"

#include <memory>
#include <cstring>

using namespace llvm;

namespace tart {
extern void addTartGC();
}

enum OutputType {
  Unset = 0,
  BitcodeFile,
  AssemblyFile,
  ObjectFile,
  ExecutableFile,
  // native dynamic lib
};

enum OptLevel {
  O0, O1, O2, O3,
};

// Input/Output Options

static cl::list<std::string> optInputFilenames(cl::Positional, cl::OneOrMore,
    cl::desc("<input bitcode files>"));

static cl::opt<std::string> optOutputFilename("o",
    cl::desc("Override output filename"),
    cl::value_desc("filename"));

static cl::list<std::string> optModulePaths("i", cl::Prefix,
    cl::desc("Specify a module search path"),
    cl::value_desc("directory"));

static cl::opt<bool> optVerbose("v",
    cl::desc("Verbose output"));

static cl::list<std::string> optLibPaths("L", cl::Prefix,
    cl::desc("Specify a library search path"),
    cl::value_desc("directory"));

static cl::list<std::string> optFrameworkPaths("F", cl::Prefix,
    cl::desc("Specify a framework search path"),
    cl::value_desc("directory"));

static cl::list<std::string> optLibraries("l", cl::Prefix,
    cl::desc("Specify libraries to link to"),
    cl::value_desc("library prefix"));

static cl::list<std::string> optFrameworks("framework",
    cl::desc("Specify frameworks to link to"),
    cl::value_desc("framework"));

static cl::opt<bool> optDumpAsm("dump-asm",
    cl::desc("Print resulting IR"));

static cl::opt<bool> optDumpDebug("dump-debug",
    cl::desc("Print debugging information"));

static cl::opt<bool> optShowSizes("show-size",
    cl::desc("Print size of debugging and reflection data"));

// Options to control the linking, optimization, and code gen processes

cl::opt<OutputType> optOutputType("filetype", cl::init(Unset),
    cl::desc("Choose a file type (not all types are supported by all targets):"),
    cl::values(
        clEnumValN(BitcodeFile, "bc", "  Emit a bitcode ('.bc') file"),
        clEnumValN(ObjectFile, "obj", "Emit a native object ('.o') file [experimental]"),
        clEnumValN(AssemblyFile, "asm","  Emit an assembly ('.s') file"),
        clEnumValN(ExecutableFile, "exec","  Emit a native executable file"),
        clEnumValEnd));

static cl::opt<bool> optLinkAsLibrary("link-as-library",
    cl::desc("Link the .bc files together as a library, not an executable"));

//Don't verify at the end
static cl::opt<bool> optDontVerify("disable-verify", cl::ReallyHidden);

static cl::opt<OptLevel> optOptimizationLevel(cl::init(O0),
    cl::desc("Choose optimization level:"),
    cl::values(
        clEnumVal(O0, "No optimizations"),
        clEnumVal(O1, "Enable trivial optimizations"),
        clEnumVal(O2, "Enable default optimizations"),
        clEnumVal(O3, "Enable expensive optimizations"),
        clEnumValEnd));

static cl::opt<bool> optDisableInline("disable-inlining",
    cl::desc("Do not run the inliner pass"));

static cl::opt<bool> optInternalize("internalize",
    cl::desc("Mark all symbols as internal except for 'main'"));

static cl::opt<bool> optVerifyEach("verify-each",
    cl::desc("Verify intermediate results of all passes"));

static cl::opt<bool> optStrip("strip-all",
    cl::desc("Strip all symbol info from executable"));

static cl::opt<bool> optStripDebug("strip-debug",
    cl::desc("Strip debugger symbol info from executable"));

// Code generation options

static cl::opt<std::string> optTargetTriple("mtriple",
    cl::desc("Override target triple for module"));

static cl::opt<std::string> optMArch("march",
    cl::desc("Architecture to generate code for (see --version)"));

static cl::opt<std::string> optMCPU("mcpu",
  cl::desc("Target a specific cpu type (-mcpu=help for details)"),
  cl::value_desc("cpu-name"),
  cl::init(""));

static cl::list<std::string> optMAttrs("mattr",
  cl::CommaSeparated,
  cl::desc("Target specific attributes (-mattr=help for details)"),
  cl::value_desc("a1,+a2,-a3,..."));

static cl::opt<bool> optNoImplicitFloats("no-implicit-float",
  cl::desc("Don't generate implicit floating point instructions (x86-only)"),
  cl::init(false));

/// printAndExit - Prints a message to standard error and exits with error code
///
/// Inputs:
///  Message  - The message to print to standard error.
///
static void printAndExit(const std::string &Message, int errcode = 1) {
  errs() << "tartln: " << Message << "\n";
  llvm_shutdown();
  exit(errcode);
}

// A utility function that adds a pass to the pass manager but will also add
// a verifier pass after if we're supposed to verify.
static inline void addPass(PassManagerBase & pm, Pass * pass) {
  // Add the pass to the pass manager...
  pm.add(pass);

  // If we are verifying all of the intermediate steps, add the verifier...
  if (optVerifyEach) {
    pm.add(createVerifierPass());
  }
}

/// Optimize - Perform link time optimizations. This will run the scalar
/// optimizations, any loaded plugin-optimization modules, and then the
/// inter-procedural optimizations if applicable.
void optimize(Module * module, const TargetData * targetData) {

  // Instantiate the pass manager to organize the passes.
  FunctionPassManager fpm(module);
  PassManager passes;
  bool runFunctionPasses = false;

  // If we're verifying, start off with a verification pass.
  if (optVerifyEach) {
    passes.add(createVerifierPass());
  }

  // Add an appropriate TargetData instance for this module...
  addPass(passes, new TargetData(*targetData));
  if (!optLinkAsLibrary && optInternalize) {
    std::vector<const char *> externs;
    externs.push_back("main");
    externs.push_back("String_create");
    externs.push_back("TraceAction_traceDescriptors");
    externs.push_back("GC_static_roots");
    passes.add(createInternalizePass(externs)); // Internalize all but exported API symbols.
  }

  // If the -s or -S command line options were specified, strip the symbols out
  // of the resulting program to make it smaller.  -s and -S are GNU ld options
  // that we are supporting; they alias -strip-all and -strip-debug.
  if (optStrip || optStripDebug) {
    addPass(passes, createStripSymbolsPass(optStripDebug && !optStrip));
  } else {
    passes.add(createStripDeadDebugInfoPass());
  }

  if (optOptimizationLevel > O0) {
    // Add an appropriate TargetData instance for this module...
    if (targetData) {
      runFunctionPasses = true;
      fpm.add(new TargetData(*targetData));
    }

    PassManagerBuilder Builder;
    if (!optDisableInline) {
      Builder.Inliner = createFunctionInliningPass();
    }
    Builder.OptLevel = int(optOptimizationLevel);
    Builder.DisableSimplifyLibCalls = false;
    Builder.populateFunctionPassManager(fpm);
    Builder.populateModulePassManager(passes);
    Builder.populateLTOPassManager(
        passes,
        /*Internalize=*/ false,
        /*RunInliner=*/ !optDisableInline);
  }

  // The user's passes may leave cruft around. Clean up after them them but
  // only if we haven't got DisableOptimizations set
  if (optOptimizationLevel > O0) {
    addPass(passes, createInstructionCombiningPass());
    addPass(passes, createCFGSimplificationPass());
    addPass(passes, createAggressiveDCEPass());
    addPass(passes, createGlobalDCEPass());
  } else { //if (optInternalize) {
    addPass(passes, createGlobalDCEPass());
  }

  if (!optLinkAsLibrary) {
    addPass(passes, new tart::StaticRoots());
    addPass(passes, new tart::ReflectorPass());
  }

  // Make sure everything is still good.
  if (!optDontVerify) {
    passes.add(createVerifierPass());
  }

  if (optDumpDebug) {
    passes.add(createDbgInfoPrinterPass());
  }

  if (runFunctionPasses) {
    fpm.doInitialization();
    for (Module::iterator it = module->begin(); it != module->end(); ++it) {
      fpm.run(*it);
    }
    fpm.doFinalization();
  }

  // Run our queue of passes all at once now, efficiently.
  passes.run(*module);
}

std::auto_ptr<TargetMachine> selectTarget(Module & mod) {
  // If we are supposed to override the target triple, do so now.
  //if (!optTargetTriple.empty()) {
  //  mod.setTargetTriple(optTargetTriple);
  //}

  Triple theTriple(mod.getTargetTriple());
  if (theTriple.getTriple().empty()) {
    theTriple.setTriple(sys::getDefaultTargetTriple());
  }

  // Allocate target machine.  First, check whether the user has explicitly
  // specified an architecture to compile for. If so we have to look it up by
  // name, because it might be a backend that has no mapping to a target triple.
  const Target * theTarget = 0;
  if (!optMArch.empty()) {
    for (TargetRegistry::iterator it = TargetRegistry::begin(); it != TargetRegistry::end(); ++it) {
      if (optMArch == it->getName()) {
        theTarget = &*it;
        break;
      }
    }

    if (!theTarget) {
      errs() << "tartln: error: invalid target '" << optMArch << "'.\n";
      llvm_shutdown();
      exit(1);
    }

    // Adjust the triple to match (if known), otherwise stick with the
    // module/host triple.
    Triple::ArchType archType = Triple::getArchTypeForLLVMName(optMArch);
    if (archType != Triple::UnknownArch) {
      theTriple.setArch(archType);
    }
  } else {
    std::string errMsg;
    theTarget = TargetRegistry::lookupTarget(theTriple.getTriple(), errMsg);
    if (theTarget == 0) {
      errs() << "tartln: error auto-selecting target for module '"
             << errMsg << "'.  Please use the -march option to explicitly "
             << "pick a target.\n";
      llvm_shutdown();
      exit(1);
    }
  }

  // Package up features to be passed to target/subtarget
  std::string featuresStr;
  if (optMAttrs.size()) {
    errs() << "tartln: -mattrs is currently broken due to llvm link error.\n";
    llvm_shutdown();
    exit(1);
#if 0
    SubtargetFeatures features;
    for (unsigned i = 0; i != optMAttrs.size(); ++i) {
      features.AddFeature(optMAttrs[i]);
    }

    featuresStr = features.getString();
#endif
  }

  TargetOptions options;
  return std::auto_ptr<TargetMachine>(
      theTarget->createTargetMachine(theTriple.getTriple(), optMCPU, StringRef(featuresStr),
          options));
}

/// GenerateBitcode - generates a bitcode file from the module provided
void generateBitcode(Module * module, const sys::Path & outputFilePath) {

  if (optVerbose) {
    outs() << "Generating bitcode to " << outputFilePath.str() << '\n';
  }

  // Create the output file.
  std::string errorInfo;
  raw_fd_ostream bcOut(outputFilePath.c_str(), errorInfo, raw_fd_ostream::F_Binary);
  if (!errorInfo.empty()) {
    printAndExit(errorInfo);
  }

  // Ensure that the bitcode file gets removed from the disk if we get a
  // terminating signal.
  sys::RemoveFileOnSignal(outputFilePath);

  // Write it out
  WriteBitcodeToFile(module, bcOut);

  // Close the bitcode file.
  bcOut.close();
}

static void generateMachineCode(std::auto_ptr<Module> & mod, const sys::Path & assemblyFile,
    TargetMachine & target, TargetMachine::CodeGenFileType codeGenType) {
  std::string errMsg;

  if (optVerbose) {
    outs() << "Generating assembly to " << assemblyFile.str() << '\n';
  }

  // Figure out where we are going to send the output...
  sys::RemoveFileOnSignal(assemblyFile);

  std::auto_ptr<formatted_raw_ostream> asOut;
  formatted_raw_ostream * pOut;

  if (assemblyFile.str() == "-") {
    outs() << "Generating assembly to stdout\n";
    pOut = &fouts();
  } else {
    raw_fd_ostream * fdOut =
        new raw_fd_ostream(assemblyFile.c_str(), errMsg, raw_fd_ostream::F_Binary);
    if (!errMsg.empty()) {
      errs() << errMsg << '\n';
      delete fdOut;
      sys::Path(assemblyFile).eraseFromDisk();
      llvm_shutdown();
      exit(1);
    }

    asOut.reset(new formatted_raw_ostream(*fdOut, formatted_raw_ostream::DELETE_STREAM));
    pOut = asOut.get();
  }

  if (!asOut.get()) {
    sys::Path(assemblyFile).eraseFromDisk();
    llvm_shutdown();
    exit(1);
  }

  CodeGenOpt::Level optLevel = CodeGenOpt::Default;
  switch (optOptimizationLevel) {
    case O0: optLevel = CodeGenOpt::None; break;
    case O1: optLevel = CodeGenOpt::Less; break;
    case O2: optLevel = CodeGenOpt::Default; break;
    case O3: optLevel = CodeGenOpt::Aggressive; break;
  }

  // Build up all of the passes that we want to do to the module.
  PassManager pm;

  // Add the target data from the target machine, if it exists, or the module.
  if (const TargetData * targetData = target.getTargetData()) {
    pm.add(new TargetData(*targetData));
  } else {
    pm.add(new TargetData(mod.get()));
  }

  // Override default to generate verbose assembly.
  target.setAsmVerbosityDefault(true);

  if (target.addPassesToEmitFile(pm, *asOut, codeGenType, optLevel)) {
    errs() << "tartln: target does not support generation of this file type!\n";
    goto fail;
  }

  pm.run(*mod);
  return;

fail:
  sys::Path(assemblyFile).eraseFromDisk();
  llvm_shutdown();
  exit(1);
}

// BuildLinkItems -- This function generates a LinkItemList for the LinkItems
// linker function by combining the Files and Libraries in the order they were
// declared on the command line.
static void buildLinkItems(Linker::ItemList & items, const cl::list<std::string> & files,
    const cl::list<std::string> & libraries) {

  // Build the list of linkage items for LinkItems.

  cl::list<std::string>::const_iterator fileIt = files.begin();
  cl::list<std::string>::const_iterator libIt = libraries.begin();

  int libPos = -1, filePos = -1;
  while (libIt != libraries.end() || fileIt != files.end()) {
    if (libIt != libraries.end()) {
      libPos = libraries.getPosition(libIt - libraries.begin());
    } else {
      libPos = -1;
    }

    if (fileIt != files.end()) {
      filePos = files.getPosition(fileIt - files.begin());
    } else {
      filePos = -1;
    }

    if (filePos != -1 && (libPos == -1 || filePos < libPos)) {
      // Add a source file
      items.push_back(std::make_pair(*fileIt++, false));
    } else if (libPos != -1 && (filePos == -1 || libPos < filePos)) {
      // Add a library
      items.push_back(std::make_pair(*libIt++, true));
    }
  }
}

int main(int argc, char **argv, char **envp) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  LLVMContext &context = getGlobalContext();
  llvm_shutdown_obj Y; // Call llvm_shutdown() on exit.

  tart::addTartGC();

  // Initialize targets first, so that --version shows registered targets.
  InitializeAllTargets();
  InitializeAllAsmPrinters();

//  try {
    // Parse the command line options
    cl::ParseCommandLineOptions(argc, argv, "tartln\n");

    // Construct a Linker (now that Verbose is set)
    Linker linker("tartln", optOutputFilename, context, optVerbose);

    // Keep track of the native link items (versus the bitcode items)
    Linker::ItemList nativeLinkItems;

    // Add library paths to the linker
    linker.addPaths(optModulePaths);
    linker.addSystemPaths();

    // Remove any consecutive duplicates of the same library...
    optLibraries.erase(std::unique(optLibraries.begin(), optLibraries.end()), optLibraries.end());

    if (optLinkAsLibrary) {
      std::vector<sys::Path> filePaths;
      for (unsigned i = 0; i < optInputFilenames.size(); ++i) {
        filePaths.push_back(sys::Path(optInputFilenames[i]));
      }

      if (linker.LinkInFiles(filePaths)) {
        return 1; // Error already printed
      }

      // The libraries aren't linked in but are noted as "dependent" in the module.
      for (cl::list<std::string>::const_iterator it = optLibraries.begin();
          it != optLibraries.end(); ++it) {
        linker.getModule()->addLibrary(*it);
      }
    } else {
      // Build a list of the items from our command line
      Linker::ItemList items;
      buildLinkItems(items, optInputFilenames, optLibraries);

      // Link all the items together
      if (linker.LinkInItems(items, nativeLinkItems)) {
        return 1; // Error already printed
      }
    }

    std::auto_ptr<Module> composite(linker.releaseModule());
    std::auto_ptr<TargetMachine> targetMachine = selectTarget(*composite.get());

    // Optimize the module
    optimize(composite.get(), targetMachine->getTargetData());

    if (optDumpAsm) {
      errs() << "-------------------------------------------------------------\n";
      errs() << composite.get();
      errs() << "-------------------------------------------------------------\n";
    }

    // Determine output file name - and possibly deduce file type
    sys::Path outputFilename;
    bool outputToStdout = (optOutputFilename == "-");
    if (!outputToStdout) {
      outputFilename = optOutputFilename;
      std::string suffix = llvm::sys::path::extension(outputFilename.str());

      if (optOutputType == Unset) {
        if (suffix.empty()) {
          errs() << "tartln: output type not specified.\n";
          return 1;
        } else if (suffix == ".bc") {
          optOutputType = BitcodeFile;
        } else if (suffix == ".s") {
          optOutputType = AssemblyFile;
        } else if (suffix == ".o" || suffix == ".obj") {
            optOutputType = ObjectFile;
        } else {
          errs() << "tartln: unknown output file type suffix '" <<
          suffix << "'.\n";
          return 1;
        }
      }

      // If no output file name was set, use the first input name
      if (outputFilename.empty()) {
        outputFilename = optInputFilenames[0];
      }

      outputFilename.eraseSuffix();
      switch (optOutputType) {
        case AssemblyFile:
          outputFilename.appendSuffix("s");
          break;

        case BitcodeFile:
          outputFilename.appendSuffix("bc");
          break;

        case ObjectFile:
          #if defined(_WIN32) || defined(__CYGWIN__)
            outputFilename.appendSuffix("obj");
          #else
            outputFilename.appendSuffix("o");
          #endif
            break;

        case ExecutableFile:
          #if defined(_WIN32) || defined(__CYGWIN__)
            if (!optLinkAsLibrary) {
              outputFilename.appendSuffix("exe");
            } else {
              outputFilename.appendSuffix("lib");
            }
          #endif
          break;

        default:
          break;
      }
    }

    if (optOutputType == BitcodeFile) {
      generateBitcode(composite.get(), outputFilename);
    } else if (optOutputType == AssemblyFile) {
      generateMachineCode(composite, outputFilename, *targetMachine.get(),
          TargetMachine::CGFT_AssemblyFile);
    } else if (optOutputType == ObjectFile) {
      generateMachineCode(composite, outputFilename, *targetMachine.get(),
          TargetMachine::CGFT_ObjectFile);
    } else {
      printAndExit("Unsupported output type");
    }
//  } catch (const std::string & msg) {
//    printAndExit(msg, 2);
//  } catch (...) {
//    printAndExit("Unexpected unknown exception occurred.", 2);
//  }

  // Graceful exit
  return 0;
}

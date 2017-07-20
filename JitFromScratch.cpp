#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "SimpleOrcJit.h"

llvm::Expected<std::string> codegenIR(llvm::Module *module) {
  using namespace llvm;

  LLVMContext &ctx = module->getContext();
  IRBuilder<> Builder(ctx);

  auto name = "abssub";
  auto voidTy = Type::getVoidTy(ctx);
  auto intTy = Type::getInt32Ty(ctx);
  auto intPtrTy = intTy->getPointerTo();
  auto signature =
      FunctionType::get(voidTy, {intPtrTy, intPtrTy, intPtrTy}, false);
  auto linkage = Function::ExternalLinkage;

  auto fn = Function::Create(signature, linkage, name, module);
  fn->setName(name); // so the CompileLayer can find it

  Builder.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
  {
    auto argIt = fn->arg_begin();
    Argument &argPtrX = *argIt;
    Argument &argPtrY = *(++argIt);
    Argument &argPtrOut = *(++argIt);

    argPtrX.setName("x0");
    argPtrY.setName("y0");
    argPtrOut.setName("result0");

    Value *x0 = Builder.CreateLoad(&argPtrX, "x0");
    Value *y0 = Builder.CreateLoad(&argPtrY, "y0");
    Value *difference = Builder.CreateSub(x0, y0, "dist");

    auto absSig = FunctionType::get(intTy, {intTy}, false);
    Value *absFunction = module->getOrInsertFunction("abs", absSig);
    Value *absDifference = Builder.CreateCall(absFunction, {difference});

    Builder.CreateStore(absDifference, &argPtrOut);
    Builder.CreateRetVoid();
  }

  std::string error;
  raw_string_ostream es(error);

  if (verifyFunction(*fn, &es))
    return make_error<StringError>(
        Twine("Function verification failed:\n", es.str()), std::error_code());

  if (verifyModule(*module, &es))
    return make_error<StringError>(
        Twine("Module verification failed:\n", es.str()), std::error_code());

  return name;
}

// Determine the size of a C array at compile-time.
template <typename T, size_t sizeOfArray>
constexpr unsigned arrayElements(T (&)[sizeOfArray]) {
  return sizeOfArray;
}

// This function will be called from JITed code.
int *customIntAllocator(unsigned items) {
  static int memory[100];
  static unsigned allocIdx = 0;

  if (allocIdx + items > arrayElements(memory))
    exit(-1);

  int *block = memory + allocIdx;
  allocIdx += items;

  return block;
}

// This function will be replaced by a runtime-time compiled version.
template <size_t sizeOfArray>
int *integerDistances(const int (&x)[sizeOfArray], int *y,
                      std::function<void(int *, int *, int *)> jitedFn) {
  unsigned items = arrayElements(x);
  int *results = customIntAllocator(items);

  jitedFn(const_cast<int *>(x), y, results);

  return results;
}

int main(int argc, char **argv) {
  using namespace llvm;

  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);

  atexit(llvm_shutdown);

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  // Parse -debug and -debug-only options.
  cl::ParseCommandLineOptions(argc, argv, "JitFromScratch example project\n");

  int x[]{0, 1, 2};
  int y[]{3, 1, -1};

  auto targetMachine = EngineBuilder().selectTarget();
  auto jit = std::make_unique<SimpleOrcJit>(*targetMachine);

  LLVMContext context;
  auto module = std::make_unique<Module>("JitFromScratch", context);
  module->setDataLayout(targetMachine->createDataLayout());

  // Generate LLVM IR for the function.
  Expected<std::string> jitedFnName = codegenIR(module.get());
  if (!jitedFnName)
    outs() << toString(jitedFnName.takeError());

  // Compile to machine code and link.
  jit->submitModule(std::move(module));
  auto jitedFnPtr = jit->getFunction<void(int *, int *, int *)>(*jitedFnName);

  int *z = integerDistances(x, y, jitedFnPtr);

  outs() << "Integer Distances: ";
  outs() << z[0] << ", " << z[1] << ", " << z[2] << "\n\n";
  outs().flush();

  return 0;
}

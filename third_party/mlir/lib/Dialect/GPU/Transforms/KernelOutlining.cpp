//===- KernelOutlining.cpp - Implementation of GPU kernel outlining -------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements the GPU dialect kernel outlining pass.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/GPU/Passes.h"
#include "mlir/Dialect/StandardOps/Ops.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

template <typename OpTy>
static void createForAllDimensions(OpBuilder &builder, Location loc,
                                   SmallVectorImpl<Value *> &values) {
  for (StringRef dim : {"x", "y", "z"}) {
    Value *v = builder.create<OpTy>(loc, builder.getIndexType(),
                                    builder.getStringAttr(dim));
    values.push_back(v);
  }
}

// Add operations generating block/thread ids and gird/block dimensions at the
// beginning of `kernelFunc` and replace uses of the respective function args.
static void injectGpuIndexOperations(Location loc, FuncOp kernelFunc) {
  OpBuilder OpBuilder(kernelFunc.getBody());
  SmallVector<Value *, 12> indexOps;
  createForAllDimensions<gpu::BlockId>(OpBuilder, loc, indexOps);
  createForAllDimensions<gpu::ThreadId>(OpBuilder, loc, indexOps);
  createForAllDimensions<gpu::GridDim>(OpBuilder, loc, indexOps);
  createForAllDimensions<gpu::BlockDim>(OpBuilder, loc, indexOps);
  // Replace the leading 12 function args with the respective thread/block index
  // operations. Iterate backwards since args are erased and indices change.
  for (int i = 11; i >= 0; --i) {
    auto &firstBlock = kernelFunc.front();
    firstBlock.getArgument(i)->replaceAllUsesWith(indexOps[i]);
    firstBlock.eraseArgument(i);
  }
}

// Move all constant arguments of the given kernel function into the function,
// thereby reducing the number of kernel arguments.
static gpu::LaunchFuncOp inlineConstants(FuncOp kernelFunc,
                                         gpu::LaunchFuncOp launch) {
  OpBuilder kernelBuilder(kernelFunc.getBody());
  auto &firstBlock = kernelFunc.getBody().front();
  llvm::SmallVector<Value *, 8> newLaunchArgs;
  for (int i = launch.getNumKernelOperands() - 1; i >= 0; --i) {
    auto operandOp = launch.getKernelOperand(i)->getDefiningOp();
    auto constant = dyn_cast_or_null<ConstantOp>(operandOp);
    if (!constant) {
      newLaunchArgs.push_back(launch.getKernelOperand(i));
      continue;
    }
    auto newConstant = kernelBuilder.clone(*operandOp);
    firstBlock.getArgument(i)->replaceAllUsesWith(newConstant->getResult(0));
    firstBlock.eraseArgument(i);
  }
  if (newLaunchArgs.size() == launch.getNumKernelOperands())
    return launch;

  std::reverse(newLaunchArgs.begin(), newLaunchArgs.end());
  OpBuilder LaunchBuilder(launch);
  SmallVector<Type, 8> newArgumentTypes;
  newArgumentTypes.reserve(firstBlock.getNumArguments());
  for (auto value : firstBlock.getArguments()) {
    newArgumentTypes.push_back(value->getType());
  }
  kernelFunc.setType(LaunchBuilder.getFunctionType(newArgumentTypes, {}));
  auto newLaunch = LaunchBuilder.create<gpu::LaunchFuncOp>(
      launch.getLoc(), kernelFunc, launch.getGridSizeOperandValues(),
      launch.getBlockSizeOperandValues(), newLaunchArgs);
  launch.erase();
  return newLaunch;
}

// Outline the `gpu.launch` operation body into a kernel function. Replace
// `gpu.return` operations by `std.return` in the generated function.
static FuncOp outlineKernelFunc(gpu::LaunchOp launchOp) {
  Location loc = launchOp.getLoc();
  SmallVector<Type, 4> kernelOperandTypes(launchOp.getKernelOperandTypes());
  FunctionType type =
      FunctionType::get(kernelOperandTypes, {}, launchOp.getContext());
  std::string kernelFuncName =
      Twine(launchOp.getParentOfType<FuncOp>().getName(), "_kernel").str();
  FuncOp outlinedFunc = FuncOp::create(loc, kernelFuncName, type);
  outlinedFunc.getBody().takeBody(launchOp.getBody());
  Builder builder(launchOp.getContext());
  outlinedFunc.setAttr(gpu::GPUDialect::getKernelFuncAttrName(),
                       builder.getUnitAttr());
  injectGpuIndexOperations(loc, outlinedFunc);
  outlinedFunc.walk([](gpu::Return op) {
    OpBuilder replacer(op);
    replacer.create<ReturnOp>(op.getLoc());
    op.erase();
  });
  return outlinedFunc;
}

// Replace `gpu.launch` operations with an `gpu.launch_func` operation launching
// `kernelFunc`. The kernel func contains the body of the `gpu.launch` with
// constant region arguments inlined.
static void convertToLaunchFuncOp(gpu::LaunchOp &launchOp, FuncOp kernelFunc) {
  OpBuilder builder(launchOp);
  SmallVector<Value *, 4> kernelOperandValues(
      launchOp.getKernelOperandValues());
  auto launchFuncOp = builder.create<gpu::LaunchFuncOp>(
      launchOp.getLoc(), kernelFunc, launchOp.getGridSizeOperandValues(),
      launchOp.getBlockSizeOperandValues(), kernelOperandValues);
  inlineConstants(kernelFunc, launchFuncOp);
  launchOp.erase();
}

namespace {

/// Pass that moves the kernel of each LaunchOp into its separate nested module.
///
/// This pass moves the kernel code of each LaunchOp into a function created
/// inside a nested module. It also creates an external function of the same
/// name in the parent module.
///
/// The kernel modules are intended to be compiled to a cubin blob independently
/// in a separate pass. The external functions can then be annotated with the
/// symbol of the cubin accessor function.
class GpuKernelOutliningPass : public ModulePass<GpuKernelOutliningPass> {
public:
  void runOnModule() override {
    ModuleManager moduleManager(getModule());
    bool modified = false;
    for (auto func : getModule().getOps<FuncOp>()) {
      // Insert just after the function.
      Block::iterator insertPt(func.getOperation()->getNextNode());
      func.walk([&](gpu::LaunchOp op) {
        FuncOp outlinedFunc = outlineKernelFunc(op);

        // Create nested module and insert outlinedFunc. The module will
        // originally get the same name as the function, but may be renamed on
        // insertion into the parent module.
        auto kernelModule = createKernelModule(outlinedFunc, moduleManager);
        moduleManager.insert(insertPt, kernelModule);

        // Potentially changes signature, pulling in constants.
        convertToLaunchFuncOp(op, outlinedFunc);
        modified = true;
      });
    }

    // If any new module was inserted in this module, annotate this module as
    // a container module.
    if (modified)
      getModule().setAttr(gpu::GPUDialect::getContainerModuleAttrName(),
                          UnitAttr::get(&getContext()));
  }

private:
  // Returns a module containing kernelFunc and all callees (recursive).
  ModuleOp createKernelModule(FuncOp kernelFunc,
                              const ModuleManager &parentModuleManager) {
    auto context = getModule().getContext();
    Builder builder(context);
    auto kernelModule =
        ModuleOp::create(builder.getUnknownLoc(), kernelFunc.getName());
    kernelModule.setAttr(gpu::GPUDialect::getKernelModuleAttrName(),
                         builder.getUnitAttr());
    ModuleManager moduleManager(kernelModule);

    llvm::SmallVector<FuncOp, 8> funcsToInsert = {kernelFunc};
    while (!funcsToInsert.empty()) {
      FuncOp func = funcsToInsert.pop_back_val();
      moduleManager.insert(func);

      // TODO(b/141098412): Support any op with a callable interface.
      func.walk([&](CallOp call) {
        auto callee = call.callee();
        if (moduleManager.lookupSymbol<FuncOp>(callee))
          return;

        auto calleeFromParent =
            parentModuleManager.lookupSymbol<FuncOp>(callee);
        funcsToInsert.push_back(calleeFromParent.clone());
      });
    }

    return kernelModule;
  }
};

} // namespace

std::unique_ptr<OpPassBase<ModuleOp>> mlir::createGpuKernelOutliningPass() {
  return std::make_unique<GpuKernelOutliningPass>();
}

static PassRegistration<GpuKernelOutliningPass>
    pass("gpu-kernel-outlining",
         "Outline gpu.launch bodies to kernel functions.");

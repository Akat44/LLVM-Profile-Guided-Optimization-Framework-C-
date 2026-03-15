/*
  profile.cpp
  =================
  A pass which inserts profiling calls into a program
*/

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>

#include "profile-data.hpp"

using namespace llvm;
using namespace std;

class ProfilePass : public AnalysisInfoMixin<ProfilePass> {
public:
	PreservedAnalyses run(Function& F, FunctionAnalysisManager& AM) {
		auto void_ty = Type::getVoidTy(F.getContext());

		IRBuilder builder(F.getContext());

		Module& M = *F.getParent();

		auto func_name_str = builder.CreateGlobalString(get_func_name(F), "", 0, &M);

		auto enter_function = M.getOrInsertFunction("enter_function", void_ty, func_name_str->getType());
		auto exit_function  = M.getOrInsertFunction("exit_function", void_ty, func_name_str->getType());
		auto record_block   = M.getOrInsertFunction("record_block", void_ty, func_name_str->getType());

		for (auto& block : F) {
			builder.SetInsertPoint(block.getFirstInsertionPt());

			auto block_name_str = builder.CreateGlobalString(get_block_name(block), "", 0, &M);

			builder.CreateCall(record_block, {block_name_str});

			if (succ_empty(&block)) {
				builder.SetInsertPoint(&block, block.getTerminator()->getIterator());
				builder.CreateCall(exit_function, {func_name_str});
			}
		}

		builder.SetInsertPoint(F.getEntryBlock().getFirstInsertionPt());
		builder.CreateCall(enter_function, {func_name_str});

		return PreservedAnalyses::all();
	}
};

//-------------------- Plugin Entry Point --------------------
extern "C" LLVM_ATTRIBUTE_WEAK __attribute__((visibility("default"))) PassPluginLibraryInfo llvmGetPassPluginInfo() {
	return {LLVM_PLUGIN_API_VERSION, "UnifiedPass", "v0.1", [](PassBuilder& PB) {
		        PB.registerPipelineParsingCallback(
		            [](StringRef Name, FunctionPassManager& FPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
			            if (Name == "profile") {
				            FPM.addPass(ProfilePass());
				            return true;
			            }
			            return false;
		            });
	        }};
}
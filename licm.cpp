// ECE/CS 5544 S25 Assignment 2: unifiedpass.cpp
/*
  unifiedpass.cpp
  =================
  Minimal skeleton for a unified LLVM pass plugin.
  This file provides:
    - Minimal available-support stubs (Expression, getShortValueName, printSet)
    - A placeholder for a dataflow framework (students implement their own)
    - Four pass stubs:
         AvailableExpressions ("available")
         Liveness ("liveness")
         Reaching ("reaching")
         ConstantPropagation ("constantprop")

  Build as a plugin for LLVM's new pass manager.
*/

#include <fstream>
#include <numeric>
#include <queue>
#include <ranges>
#include <unordered_set>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "profile-data.hpp"

using namespace llvm;
using namespace std;

enum DataflowDirection { FORWARD, BACKWARD };
enum InitialCondition { EMPTY, ALL };

/// @brief Helper class that inserts a meet operator of set union into any class which inherits
struct MeetUnion {
	static constexpr InitialCondition INITIAL = EMPTY;

	static BitVector meet(std::vector<BitVector> in) {
		BitVector re = in[0];
		for (size_t i = 1; i < in.size(); i++) { re |= in[i]; }
		return re;
	}
};

/// @brief Helper class that inserts a meet operator of set intersection into any class which inherits
struct MeetIntersect {
	static constexpr InitialCondition INITIAL = ALL;

	static BitVector meet(std::vector<BitVector> in) {
		BitVector re = in[0];
		for (size_t i = 1; i < in.size(); i++) { re &= in[i]; }
		return re;
	}
};

/// @brief Unified dataflow framework. Uses static polymorphism by way of CRTP - inherit from `Dataflow<T>` for your
/// specialized type T and the entire pass will be ready.
/// @tparam T The specialized child class.
template <typename T>
class Dataflow : public AnalysisInfoMixin<T> {
public:
	/// @brief The analysis information attached to each basic block.
	struct BlockInfo {
		BitVector in, out;

		/// @brief The specialized "scratch" space that each pass can define for each block by defining a `Scratch`
		/// type.
		T::Scratch scratch;

		/// @brief Whether or not this block is an initial block - i.e. connected to the fictitious entry/exit block
		/// which is the "source" for this analysis (entry for forward, exit for backward).
		bool initial_block = false;

		/// @brief The original basic block this information is attached to.
		BasicBlock* block;
	};

	struct Result {
		const Function& func;

		DenseMap<const BasicBlock*, BlockInfo> infos;
		std::vector<typename T::Domain> universe;
		std::vector<uint64_t> sorted_indices;

		/// @brief Helper function for printing all BlockInfo in the BlockInfo map used in the fixed point iteration.
		/// Mainly
		/// used for final output.
		/// @param out The stream to print to
		void print_block_infos(llvm::raw_ostream& out) const {
			for (const auto& block : func) {
				const BlockInfo& info = infos.at(&block);
				out << "BB: ";
				info.block->printAsOperand(out, false);
				out << "\n";
				T::print_scratch(out, info.scratch, universe, sorted_indices);

				if constexpr (T::DIRECTION == FORWARD) {
					println_bitvector_elements(out, info.in, universe, sorted_indices, "IN");
					println_bitvector_elements(out, info.out, universe, sorted_indices, "OUT");
				} else {
					println_bitvector_elements(out, info.in, universe, sorted_indices, "OUT");
					println_bitvector_elements(out, info.out, universe, sorted_indices, "IN");
				}
			}
		}

		bool invalidate(Function& F, const PreservedAnalyses& PA, FunctionAnalysisManager::Invalidator& Inv) {
			outs() << "Invalidating...\n";
			llvm_unreachable("idk what to do here tbh");
			return false;
		}
	};

	Dataflow() {}

	Result run(Function& F, FunctionAnalysisManager& AM) { return run_inner(F); }

	Result run_inner(Function& F) {
		using Domain  = T::Domain;
		using Scratch = T::Scratch;

		// Get the universe set from the child class. This is allowed to be unsorted and have duplicate elements in
		// it for ease of calculation, and to align with the ground truth, which often prints values in the order
		// that llvm iterates.
		std::vector<Domain> original_universe = T::getUniverse(std::cref(F));

		// Since the child class can give us an unsorted universe with duplicated values, sort the universe and dedup
		// it. As well, keep track of the original order that the child class gave us values for printing later (in
		// sorted_indices).
		auto [universe, sorted_indices] = sort_dedup_universe(original_universe);

		// Construct the initial BlockInfo for each block, and decide the order in which the fixed step algorithm should
		// iterate through them.
		DenseMap<const BasicBlock*, BlockInfo> infos = get_infos(F, universe);
		std::vector<BlockInfo*> prop_order           = get_prop_order(infos);

		// Run the fixed point algorithm, with the given boundary value
		BitVector boundary(universe.size(), std::bool_constant < T::BOUNDARY == ALL > ::value);
		run_fixed_point(infos, prop_order, boundary);

		return Result{
		    .func           = F,
		    .infos          = infos,
		    .universe       = universe,
		    .sorted_indices = sorted_indices,
		};
	}

	/// @brief Helper function for printing bitvectors as sets of elements from a universe set
	/// @param out The stream to print to
	/// @param vec The bitvector set to print
	/// @param universe The universe of elements that `vec` is referencing
	/// @param sorted_indices The original sorting order of `universe`, for lining up with the ground truth
	/// @param pre An optional prefix to name the set with
	template <typename U>
	    requires(std::is_same_v<typename T::Domain, U>)
	static void println_bitvector_elements(llvm::raw_ostream& out, const BitVector& vec, const std::vector<U>& universe,
	                                       const std::vector<size_t>& sorted_indices, std::string pre = "") {
		if (!pre.empty()) out << "  " << pre << ": ";

		out << "{ ";
		// Iterate over sorted_indices rather than vec, since it indicates the correct output order
		for (size_t i = 0; i < sorted_indices.size(); i++) {
			// Only print elements which are in range of the set and corrrectly indicate they are in the set
			if (sorted_indices[i] < vec.size() && vec.test(sorted_indices[i])) {
				out << universe[sorted_indices[i]];

				// Look ahead and see if there is another element which will be printed after this one; if so, print a
				// separator.
				for (size_t j = i + 1; j < vec.size(); j++) {
					if (sorted_indices[j] < vec.size() && vec.test(sorted_indices[j])) {
						out << "; ";

						// Since we know that j is the index of the next item which will print, then advance iteration
						// to that step
						i = j - 1;
						break;
					}
				}
			}
		}

		out << " }\n";
	}

private:
	/// @brief Helper function to sort and de-duplicate a universe set from the child class.
	/// @param original_universe The original universe set given to us by the child class, which is potentially unsorted
	/// and contains duplicate elements.
	/// @return A tuple of a new sorted and de-duped universe, along with a list of indices that indicate the original
	/// order of the universe set provided by the child class, for purposes of pretty printing.
	template <typename U>
	    requires(std::is_same_v<typename T::Domain, U>)
	static std::tuple<std::vector<U>, std::vector<size_t>> sort_dedup_universe(
	    const std::vector<U>& original_universe) {
		// Calculate the new order of indices that would sort original_universe by sorting the list [1, 2, ..., n]
		// according to whether original_universe[i] < original_universe[j], rather than i < j
		std::vector<size_t> original_indices(original_universe.size());
		std::iota(original_indices.begin(), original_indices.end(), 0);
		std::sort(original_indices.begin(), original_indices.end(),
		          [&](size_t left, size_t right) { return original_universe[left] < original_universe[right]; });

		// Then, re-order original_universe using those indices by mapping i -> original_universe[i]. As well, we will
		// store the original index along with each entry to preserve it through the dedup process.
		std::vector<std::tuple<U, size_t>> sorted_universe_indices(original_universe.size());
		std::transform(original_indices.cbegin(), original_indices.cend(), sorted_universe_indices.begin(),
		               [&](size_t i) { return std::make_tuple(original_universe[i], i); });

		// De-dup the now sorted universe by ignoring the original indices
		auto last =
		    std::unique(sorted_universe_indices.begin(), sorted_universe_indices.end(),
		                [](const auto& left, const auto& right) { return std::get<0>(left) == std::get<0>(right); });
		sorted_universe_indices.erase(last, sorted_universe_indices.end());

		// Finally, discard the original indices to produce the new sorted, de-duped universe
		std::vector<U> universe(sorted_universe_indices.size());
		std::transform(sorted_universe_indices.cbegin(), sorted_universe_indices.cend(), universe.begin(),
		               [](const auto& tuple) { return std::get<0>(tuple); });

		// Then calculate the inverse sort mapping by once again sorting [1, 2, ..., n] according to the original
		// indices, and their position in the newly sorted sorted_universe_indices list.
		std::vector<size_t> sorted_indices(universe.size());
		std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
		std::sort(sorted_indices.begin(), sorted_indices.end(), [&](size_t left, size_t right) {
			return std::get<1>(sorted_universe_indices[left]) < std::get<1>(sorted_universe_indices[right]);
		});

		return std::make_tuple(universe, sorted_indices);
	}

	/// @brief Generate the initial BlockInfo map for a function
	/// @param F The function to generate BlockInfos for
	/// @param universe The universe set, made available to the child class for the purpose of calculating the scratch
	/// for each block
	template <typename U>
	    requires(std::is_same_v<typename T::Domain, U>)
	static DenseMap<const BasicBlock*, BlockInfo> get_infos(Function& F, const std::vector<U>& universe) {
		DenseMap<const BasicBlock*, BlockInfo> infos;

		// Initial state for OUT - if ALL, then we set the set equal to Universe (represented by all true),
		// otherwise it's EMPTY and should be the empty set (represented by all false). Also the constant state for the
		// "source" block.j
		BitVector init(universe.size(), T::INITIAL == ALL);

		// Initialize all block infos
		for (auto& block : F) {
			BlockInfo& info = infos.insert({&block, {}}).first->getSecond();
			info.block      = &block;
			info.out        = init;

			if constexpr (T::DIRECTION == FORWARD) {
				// In the forward direction, the initial block is the only entry block
				info.initial_block = &block == &F.getEntryBlock();
			} else {
				// In the backwards direction, every block which returns (and therefore has no successors) is an initial
				// block
				info.initial_block = succ_empty(&block);
			}

			// Allow child class to calculate the scratch for this block
			info.scratch = T::calc_scratch(std::cref(block), universe);
		}

		return infos;
	}

	/// @brief Generate the propagation/iteration order of the fixed point algorithm
	/// @param infos The BlockInfo map for the fixed point algorithm
	/// @return A vector of BlockInfo* indicating iteration order.
	static std::vector<BlockInfo*> get_prop_order(DenseMap<const BasicBlock*, BlockInfo>& infos) {
		std::vector<BlockInfo*> prop_order;

		// Begin with all initial blocks
		for (auto& info_pair : infos) {
			if (info_pair.getSecond().initial_block) { prop_order.push_back(&info_pair.getSecond()); }
		}

		// Construct propagation order - in breadth-first order (called depth-first order in class)
		for (size_t i = 0; i < prop_order.size(); i++) {
			// Define the DAG traversal based on the propagation direction
			auto next = [&]() {
				if constexpr (T::DIRECTION == FORWARD) {
					return successors(prop_order[i]->block);
				} else {
					return predecessors(prop_order[i]->block);
				}
			}();
			for (BasicBlock* succ : next) {
				// Only add each block once
				if (std::find_if(prop_order.begin(), prop_order.end(),
				                 [&](BlockInfo* info) { return info->block == succ; }) == prop_order.end()) {
					prop_order.push_back(&infos[succ]);
				}
			}
		}

		return prop_order;
	}

	/// @brief Run the fixed point algorithm
	/// @param infos The BlockInfos updated by the algorithm
	/// @param prop_order The order in which BlockInfos should be updated each iteration
	/// @param boundary The value of the OUT set of the boundary block ENTRY/EXIT depending on direction.
	static void run_fixed_point(DenseMap<const BasicBlock*, BlockInfo>& infos,
	                            const std::vector<BlockInfo*>& prop_order, const BitVector& boundary) {
		// Keep track of which blocks have had their predecessor's OUT sets updated and potentially need to re-culculate
		// their IN set and transfer function. To begin with, all blocks must be updated.
		BitVector updated(prop_order.size(), true);
		BitVector updated_next(prop_order.size(), false);

		// As long as a block potentially needs to be updated, we haven't converged
		while (updated.any()) {
			// Loop through prop_order
			for (unsigned i = 0; i < prop_order.size(); i++) {
				// Only update a block if needed
				if (updated[i]) {
					std::vector<BitVector> in;

					BlockInfo& curr_block = *prop_order[i];

					// If a block is an initial block, then it has the special edge block ENTRY/EXIT as a predecessor.
					// This block's OUT set is never updated, and therefore still has the value init each iteration.
					if (curr_block.initial_block) { in.push_back(boundary); }

					// The predecessors of the block, depending on the direction
					auto pred = [&]() {
						if constexpr (T::DIRECTION == FORWARD) {
							return predecessors(curr_block.block);
						} else {
							return successors(curr_block.block);
						}
					}();

					for (auto block : pred) { in.push_back(infos[block].out); }

					curr_block.in     = T::meet(std::cref(in));
					BitVector new_out = T::transfer(std::cref(curr_block));

					// If our OUT has changed, then we'll need to reculate the IN for all of our succesors on the next
					// iteration
					if (new_out != curr_block.out) {
						auto succ = [&]() {
							if constexpr (T::DIRECTION == FORWARD) {
								return successors(curr_block.block);
							} else {
								return predecessors(curr_block.block);
							}
						}();

						for (auto block : succ) {
							updated_next.set(std::find(prop_order.begin(), prop_order.end(), &infos[block]) -
							                 prop_order.begin());
						}
					}

					curr_block.out = new_out;
				}
			}

			updated = updated_next;
			updated_next.reset();
		}
	}
};

//-------------------- UnifiedPass Namespace --------------------
namespace UnifiedPass {

class DominatorAnalysis : public Dataflow<DominatorAnalysis>, public MeetIntersect {
public:
	static AnalysisKey Key;

	struct Block {
		const BasicBlock* block;

		auto operator<=>(const Block&) const = default;
	};
	using Domain = Block;

	struct Scratch {
		// The index of this block in the `universe` vector
		size_t block_idx;
	};

	static constexpr DataflowDirection DIRECTION = FORWARD;
	static constexpr InitialCondition BOUNDARY   = EMPTY;

	static std::vector<Domain> getUniverse(const Function& F) {
		std::vector<Domain> re;
		for (auto& BB : F) { re.push_back(Block{.block = &BB}); }
		return re;
	}

	static Scratch calc_scratch(const BasicBlock& block, const std::vector<Domain>& universe) {
		return Scratch{.block_idx =
		                   (size_t) (std::lower_bound(universe.begin(), universe.end(), Block{.block = &block}) -
		                             universe.begin())};
	}

	static void print_scratch(llvm::raw_ostream& out, const Scratch& scratch, const std::vector<Domain>& universe,
	                          const std::vector<size_t>& sorted_indices) {}

	static BitVector transfer(const BlockInfo& block) {
		BitVector re = block.in;
		re.set(block.scratch.block_idx);

		return re;
	}

	struct Edge {
		const BasicBlock* head;
		const BasicBlock* tail;
	};

	static std::vector<Edge> get_back_edges(const Result& dom_res) {
		std::vector<Edge> re;

		for (auto& tail_block : dom_res.func) {
			BitVector dominators = dom_res.infos.at(&tail_block).out;
			for (auto head_block : successors(&tail_block)) {
				size_t head_idx = (size_t) (std::lower_bound(dom_res.universe.begin(), dom_res.universe.end(),
				                                             Block{.block = head_block}) -
				                            dom_res.universe.begin());
				if (dominators.test(head_idx)) {
					re.push_back(Edge{
					    .head = head_block,
					    .tail = &tail_block,
					});
				}
			}
		}

		return re;
	}

	static DenseSet<const BasicBlock*> get_loop_blocks(const Edge& back_edge) {
		DenseSet<const BasicBlock*> blocks;
		blocks.insert(back_edge.head);
		blocks.insert(back_edge.tail);

		std::vector<const BasicBlock*> stack;
		stack.push_back(back_edge.tail);

		while (!stack.empty()) {
			const BasicBlock* next_block = stack.back();
			stack.pop_back();

			for (auto pred : predecessors(next_block)) {
				if (!blocks.contains(pred)) {
					blocks.insert(pred);

					stack.push_back(pred);
				}
			}
		}

		return blocks;
	}
};
AnalysisKey DominatorAnalysis::Key;
llvm::raw_ostream& operator<<(llvm::raw_ostream& out, const DominatorAnalysis::Domain& rhs) {
	rhs.block->printAsOperand(out, false);
	return out;
}

class LoopInvariantCode : public PassInfoMixin<LoopInvariantCode> {
public:
	PreservedAnalyses run(Loop& loop, LoopAnalysisManager& AM, LoopStandardAnalysisResults& res, LPMUpdater& updater) {
		assert(loop.isLoopSimplifyForm() && "Loop should always be in loop simplify form");

		bool use_profile_data = false;
		ProfileData prof_data;
		if (const char* env_fname = std::getenv("PROF_DATA")) {
			std::ifstream prof_file(env_fname);
			prof_data        = profile_data_from(prof_file);
			use_profile_data = true;
		}

		BasicBlock* header = loop.getHeader();

		unsigned num_succ      = header->getTerminator()->getNumSuccessors();
		unsigned num_loop_succ = 0;

		for (auto succ : successors(header)) {
			if (loop.contains(succ)) { num_loop_succ++; }
		}

		BasicBlock* preheader = loop.getLoopPreheader();

		BasicBlock* code_motion_block;
		bool code_motion_block_after_header = false;

		if (num_succ == num_loop_succ) {
			code_motion_block = SplitEdge(preheader, header, nullptr, nullptr, nullptr, "code_motion_header");
		} else if (num_succ == 2 && num_loop_succ == 1) {
			BasicBlock* loop_succ;
			for (auto succ : successors(header)) {
				if (loop.contains(succ)) {
					loop_succ = succ;
					break;
				}
			}

			// Gotta clone the stupid condition block
			ValueToValueMapTy value_map;
			BasicBlock* new_cond = CloneBasicBlock(header, value_map, "_licm", header->getParent());
			loop.addBasicBlockToLoop(new_cond, res.LI);

			SmallVector<BasicBlock*> latches;
			loop.getLoopLatches(latches);

			for (auto latch : latches) {
				latch->getTerminator()->replaceSuccessorWith(header, new_cond);
				for (auto& phi : header->phis()) { phi.removeIncomingValue(latch); }
			}
			for (auto& phi : new_cond->phis()) { phi.removeIncomingValue(preheader); }

			for (auto succ : successors(new_cond)) {
				for (auto& phi : succ->phis()) {
					phi.addIncoming(value_map.lookup(phi.getIncomingValueForBlock(header)), new_cond);
				}
			}

			for (auto& inst : *new_cond) {
				if (isa<PHINode>(&inst)) continue;

				for (unsigned i = 0; i < inst.getNumOperands(); i++) {
					Value* op = inst.getOperand(i);
					if (auto op_inst = dyn_cast<Instruction>(op)) {
						if (op_inst->getParent() == header) { inst.setOperand(i, value_map.lookup(op_inst)); }
					}
				}
			}

			code_motion_block    = SplitEdge(header, loop_succ, nullptr, nullptr, nullptr, "code_motion_header");
			BasicBlock* phi_glue = SplitEdge(code_motion_block, loop_succ, nullptr, nullptr, nullptr, "phi_glue");
			loop.addBasicBlockToLoop(phi_glue, res.LI);
			loop.moveToHeader(phi_glue);
			loop.removeBlockFromLoop(header);

			new_cond->getTerminator()->replaceSuccessorWith(loop_succ, phi_glue);

			for (auto& inst : *header) {
				if (inst.getType()->isVoidTy()) continue;

				PHINode* new_phi = PHINode::Create(inst.getType(), 2, inst.getName() + "_licm");

				std::vector<Use*> modify_uses;
				for (auto& use : inst.uses()) {
					if (auto use_inst = dyn_cast<Instruction>(use.getUser())) {
						if (loop.contains(use_inst->getParent())) { modify_uses.push_back(&use); }
					}
				}
				for (auto use : modify_uses) { use->set(new_phi); }

				new_phi->insertBefore(phi_glue->getTerminator());
				new_phi->addIncoming(&inst, code_motion_block);
				new_phi->addIncoming(value_map.lookup(&inst), new_cond);
			}

			code_motion_block_after_header = true;
		} else {
			llvm_unreachable("Only works on normal-looking loops");
		}

		DenseSet<Instruction*> invariant_instructions;
		bool changed;
		int pass = 1;
		do {
			changed = false;

			for (auto block : loop.blocks()) {
				for (auto& inst : *block) {
					if (inst.getType()->isVoidTy()) continue;

					if (isSafeToSpeculativelyExecute(&inst) && !inst.mayReadFromMemory() &&
					    !isa<LandingPadInst>(&inst) && !invariant_instructions.contains(&inst)) {
						bool is_invariant = true;
						for (auto op : inst.operand_values()) {
							if (auto op_inst = dyn_cast<Instruction>(op)) {
								if (!invariant_instructions.contains(op_inst) && loop.contains(op_inst->getParent())) {
									is_invariant = false;
									break;
								}
							} else if (!isa<Constant>(op) && !isa<Argument>(op)) {
								is_invariant = false;
								break;
							}
						}

						if (is_invariant) {
							invariant_instructions.insert(&inst);
							changed = true;
						}
					}
				}
			}
			pass++;
		} while (changed);

		// Make sure to add new code motion block to parent loops, otherwise this sub loop will be unreachable
		if (auto parent = loop.getParentLoop()) { parent->addBasicBlockToLoop(code_motion_block, res.LI); }

		DominatorAnalysis dom_analysis;
		DominatorAnalysis::Result dom_results = dom_analysis.run_inner(*loop.getLoopPreheader()->getParent());

		BitVector exit_dominators(dom_results.universe.size(), true);
		SmallVector<BasicBlock*> exiting_blocks;
		loop.getExitingBlocks(exiting_blocks);

		// Record the list of all basic blocks that dominate every exit
		for (auto exit_block : exiting_blocks) {
			if (exit_block == header && code_motion_block_after_header) { continue; }
			exit_dominators &= dom_results.infos.at(exit_block).out;
		}

		outs() << "=== Loop Invariant Instructions ===\n";
		DenseSet<Instruction*> movable;
		for (auto inst : invariant_instructions) {
			inst->printAsOperand(outs(), false);

			size_t block_idx = (size_t) (std::lower_bound(dom_results.universe.begin(), dom_results.universe.end(),
			                                              DominatorAnalysis::Block{.block = inst->getParent()}) -
			                             dom_results.universe.begin());

			if (!exit_dominators.test(block_idx)) {
				if (!use_profile_data) {
					outs() << " (doesn't dominate all exits)\n";
					continue;
				} else {
					auto block_record = prof_data.block_records.find(get_block_name(*inst->getParent()));
					if (block_record == prof_data.block_records.end()) {
						outs() << " Couldn't find profiling data for block " << get_block_name(*inst->getParent())
						       << '\n';
						continue;
					}
					auto func_record = prof_data.function_records.find(get_func_name(*inst->getParent()->getParent()));
					if (func_record == prof_data.function_records.end()) {
						outs() << " Couldn't find profiling data for block "
						       << get_func_name(*inst->getParent()->getParent()) << '\n';
						continue;
					}

					if (block_record->second.num_entries > 3 * func_record->second.num_entries) {
						outs() << " (doesn't dominate all exits, but profiling data says to move anyway)";
					} else {
						outs() << " (doesn't dominate all exits, and isn't commonly executed)\n";
						continue;
					}
				}
			}
			outs() << '\n';

			movable.insert(inst);
		}

		while (!movable.empty()) {
			DenseSet<Instruction*> moved;
			for (auto inst : movable) {
				bool movable_now = true;
				for (auto op : inst->operand_values()) {
					if (auto op_inst = dyn_cast<Instruction>(op)) {
						if (op_inst->getParent() != code_motion_block && loop.contains(op_inst->getParent())) {
							movable_now = false;
							break;
						}
					}
				}

				if (movable_now) {
					inst->moveBefore(code_motion_block->getTerminator());
					moved.insert(inst);
					outs() << "Moved ";
					inst->printAsOperand(outs(), false);
					outs() << " to code motion block\n";
				}
			}

			for (auto inst : moved) { movable.erase(inst); }
		}

		outs().flush();

		return PreservedAnalyses::all();
	}
};
// char LoopInvariantCode::ID = 0;
}  // namespace UnifiedPass

//-------------------- Plugin Entry Point --------------------
extern "C" LLVM_ATTRIBUTE_WEAK __attribute__((visibility("default"))) PassPluginLibraryInfo llvmGetPassPluginInfo() {
	return {LLVM_PLUGIN_API_VERSION, "UnifiedPass", "v0.1", [](PassBuilder& PB) {
		        PB.registerPipelineParsingCallback(
		            [](StringRef Name, LoopPassManager& LPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
			            if (Name == "loop-invariant-code-motion") {
				            LPM.addPass(UnifiedPass::LoopInvariantCode());
				            return true;
			            }
			            return false;
		            });

		        // Dunno why but this doesn't actually register the analysis
		        PB.registerAnalysisRegistrationCallback([](FunctionAnalysisManager& FAM) {
			        FAM.registerPass([]() { return UnifiedPass::DominatorAnalysis(); });
		        });
	        }};
}
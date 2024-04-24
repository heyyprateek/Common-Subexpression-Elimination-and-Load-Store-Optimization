
#include <fstream>
#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "llvm-c/Core.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// Define a macro to enable/disable debugging output
#define DEBUG_PRINT_EN false

#if DEBUG_PRINT_EN
    # define DEBUG_PRINT(msg) llvm::errs() << msg;
#else
    # define DEBUG_PRINT(msg)
#endif

void debugPrintLLVMInstr(Instruction &I) {
    // convert to string
    std::string InstStr;
    raw_string_ostream OS(InstStr);
    // Use the debug print macro
    DEBUG_PRINT("Instruction:" << InstStr);
}

static void DeadCodeElimination(Module *);
static void SimplifyInstructions(Module *);
static void CommonSubexpressionElimination(Module *);
static void EliminateRedundantLoads(Module *);
static void EliminateRedundantStores(Module *);

static void summarize(Module *M);
static void print_csv_file(std::string outputfile);

static cl::opt<std::string>
        InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::Required, cl::init("-"));

static cl::opt<std::string>
        OutputFilename(cl::Positional, cl::desc("<output bitcode>"), cl::Required, cl::init("out.bc"));

static cl::opt<bool>
        Mem2Reg("mem2reg",
                cl::desc("Perform memory to register promotion before CSE."),
                cl::init(false));

static cl::opt<bool>
        NoCSE("no-cse",
              cl::desc("Do not perform CSE Optimization."),
              cl::init(false));

static cl::opt<bool>
        Verbose("verbose",
                    cl::desc("Verbose stats."),
                    cl::init(false));

static cl::opt<bool>
        NoCheck("no",
                cl::desc("Do not check for valid IR."),
                cl::init(false));


/**
 * @brief Prints the contents of the given LLVM module for debugging purposes.
 * 
 * This function iterates over all functions, basic blocks, and instructions
 * in the given LLVM module and prints their details to the standard output.
 * 
 * @param M Pointer to the LLVM module to be printed.
 */
void debugPrintModule(Module *M) {
    // Iterate over all functions in the module
    for (Function &F : *M) {
        // Print the name of the function
        llvm::outs() << "Function: " << F.getName() << "\n";
        
        // Iterate over all basic blocks in the function
        for (BasicBlock &BB : F) {
            // Print the name of the basic block
            llvm::outs() << "  Basic Block: " << BB.getName() << "\n";
            
            // Iterate over all instructions in the basic block
            for (Instruction &I : BB) {
                // Print the instruction
                llvm::outs() << "    Instruction: " << I << "\n";
            }
        }
    }
}


int main(int argc, char **argv) {
    // Parse command line arguments
    cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

    // Handle creating output files and shutting down properly
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.
    LLVMContext Context;

    // LLVM idiom for constructing output file.
    std::unique_ptr<ToolOutputFile> Out;
    std::string ErrorInfo;
    std::error_code EC;
    Out.reset(new ToolOutputFile(OutputFilename.c_str(), EC,
                                 sys::fs::OF_None));

    EnableStatistics();

    // Read in module
    SMDiagnostic Err;
    std::unique_ptr<Module> M;
    M = parseIRFile(InputFilename, Err, Context);

    // If errors, fail
    if (M.get() == 0)
    {
        Err.print(argv[0], errs());
        return 1;
    }

    // If requested, do some early optimizations
    if (Mem2Reg)
    {
        legacy::PassManager Passes;
        Passes.add(createPromoteMemoryToRegisterPass());
        Passes.run(*M.get());
    }

    if (!NoCSE) {
        CommonSubexpressionElimination(M.get());
    }

    // Collect statistics on Module
    summarize(M.get());
    print_csv_file(OutputFilename);

    if (Verbose)
        PrintStatistics(errs());

    // Verify integrity of Module, do this by default
    if (!NoCheck)
    {
        legacy::PassManager Passes;
        Passes.add(createVerifierPass());
        Passes.run(*M.get());
    }

    // Write final bitcode
    WriteBitcodeToFile(*M.get(), Out->os());
    Out->keep();
    if (DEBUG_PRINT_EN) debugPrintModule(M.get());

    return 0;
}


static llvm::Statistic nFunctions = {"", "Functions", "number of functions"};
static llvm::Statistic nInstructions = {"", "Instructions", "number of instructions"};
static llvm::Statistic nLoads = {"", "Loads", "number of loads"};
static llvm::Statistic nStores = {"", "Stores", "number of stores"};

static void summarize(Module *M) {
    for (auto i = M->begin(); i != M->end(); i++) {
        if (i->begin() != i->end()) {
            nFunctions++;
        }

        for (auto j = i->begin(); j != i->end(); j++) {
            for (auto k = j->begin(); k != j->end(); k++) {
                Instruction &I = *k;
                nInstructions++;
                if (isa<LoadInst>(&I)) {
                    nLoads++;
                } else if (isa<StoreInst>(&I)) {
                    nStores++;
                }
            }
        }
    }
}

static void print_csv_file(std::string outputfile)
{
    std::ofstream stats(outputfile + ".stats");
    auto a = GetStatistics();
    for (auto p : a) {
        stats << p.first.str() << "," << p.second << std::endl;
    }
    stats.close();
}

static llvm::Statistic CSEDead = {"", "CSEDead", "CSE found dead instructions"};
static llvm::Statistic CSEElim = {"", "CSEElim", "CSE redundant instructions"};
static llvm::Statistic CSESimplify = {"", "CSESimplify", "CSE simplified instructions"};
static llvm::Statistic CSELdElim = {"", "CSELdElim", "CSE redundant loads"};
static llvm::Statistic CSEStore2Load = {"", "CSEStore2Load", "CSE forwarded store to load"};
static llvm::Statistic CSEStElim = {"", "CSEStElim", "CSE redundant stores"};

// --------------------------------------------------------------------------------
//                      Optimization 0: Dead Code Elimination
// --------------------------------------------------------------------------------
/**
 * @brief Checks if the given LLVM instruction is dead, i.e., has no uses and can be safely removed.
 * 
 * This function examines the opcode of the instruction to determine if it falls into one of the categories
 * of instructions that can be considered dead. Instructions such as arithmetic, bitwise, conversion, and
 * memory-related instructions are checked to ensure they have no uses. If an instruction is a load, it is
 * further checked to ensure it is not volatile.
 * 
 * @param I Reference to the LLVM instruction to be checked.
 * @return True if the instruction is dead, false otherwise.
 */
bool isDead(Instruction &I) { 
    int opcode = I.getOpcode();

    switch(opcode) {
        // Instructions that do not produce a value
        case Instruction::Add:
        case Instruction::FNeg:
        case Instruction::FAdd: 	
        case Instruction::Sub:
        case Instruction::FSub: 	
        case Instruction::Mul:
        case Instruction::FMul: 	
        case Instruction::UDiv:	
        case Instruction::SDiv:	
        case Instruction::FDiv:	
        case Instruction::URem: 	
        case Instruction::SRem: 	
        case Instruction::FRem:
        case Instruction::Shl: 	
        case Instruction::LShr: 	
        case Instruction::AShr: 	
        case Instruction::And: 	
        case Instruction::Or: 	
        case Instruction::Xor:
        case Instruction::GetElementPtr: 	
        case Instruction::Trunc: 	
        case Instruction::ZExt: 	
        case Instruction::SExt: 	
        case Instruction::FPToUI: 	
        case Instruction::FPToSI: 	
        case Instruction::UIToFP: 	
        case Instruction::SIToFP: 	
        case Instruction::FPTrunc: 	
        case Instruction::FPExt: 	
        case Instruction::PtrToInt: 	
        case Instruction::IntToPtr: 	
        case Instruction::BitCast: 	
        case Instruction::AddrSpaceCast: 	
        case Instruction::ICmp: 	
        case Instruction::FCmp: 	
        case Instruction::ExtractElement: 	
        case Instruction::InsertElement: 	
        case Instruction::ShuffleVector: 	
        case Instruction::ExtractValue: 	
        case Instruction::InsertValue:
        case Instruction::Alloca:
        case Instruction::PHI: 
        case Instruction::Select: 
            // Check if the instruction has no uses
            if (I.use_begin() == I.use_end()) 
                return true;
            break;

        // Load instruction
        case Instruction::Load:
        {
            LoadInst *LI = dyn_cast<LoadInst>(&I);
            // Check if the load instruction is volatile
            if (LI && LI->isVolatile())
                return false;
            // Check if the load instruction has no uses
            if (I.use_begin() == I.use_end())
                return true;
            break;
        }
          
        default: 
            // Any other opcode fails the test
            return false;
    }
    return false;
}


/**
 * @brief Performs dead code elimination (DCE) on the given LLVM module.
 * 
 * This function iterates over all functions and basic blocks in the module,
 * identifies dead instructions within each basic block, and removes them.
 * Dead instructions are those that have no uses or are determined to be
 * dead based on specific opcode rules.
 * 
 * @param M Pointer to the LLVM module to perform DCE on.
 */
static void DeadCodeElimination(Module *M) {
    DEBUG_PRINT("DCE start\n");

    // Iterate over all functions in the module
    for (Function &F : *M) {
        // Iterate over all basic blocks in the function
        for (BasicBlock &BB : F) {
            std::vector<Instruction*> deadInstList;

            // Iterate over all instructions in the basic block
            for (Instruction &I : BB) {
                // Check if the instruction is dead
                std::vector<Instruction*> deadInstList;
                if (isDead(I)) {
                    deadInstList.push_back(&I);
                }
            }

            // Remove dead instructions from the basic block
            if (deadInstList.size() > 0) {
                for (Instruction *deadInst : deadInstList) {
                    DEBUG_PRINT("erasing dead instruction: \n\t");
                    debugPrintLLVMInstr(*deadInst);
                    DEBUG_PRINT("\n");
                    deadInst->eraseFromParent();
                    CSEDead++;
                }
            }
        }
    }

    DEBUG_PRINT("DCE end\n");
}


// --------------------------------------------------------------------------------
//                      Optimization 1: Simplify Instructions
// --------------------------------------------------------------------------------
/**
 * @brief Simplifies instructions within the given LLVM module.
 * 
 * This function iterates over all functions and basic blocks in the module,
 * simplifies instructions, and replaces them with simplified values if possible.
 * Simplified instructions are those that can be simplified using LLVM's
 * built-in simplification rules.
 * 
 * @param M Pointer to the LLVM module to simplify instructions in.
 */
static void SimplifyInstructions(Module *M) {
    DEBUG_PRINT("Simplify instruction start\n");

    // Iterate over all functions in the module
    for (Function &F : *M) {
        // Iterate over all basic blocks in the function
        for (BasicBlock &BB : F) {
            std::vector<Instruction*> toEraseSimplify;

            // Iterate over all instructions in the basic block
            for (Instruction &I : BB) {
                // Simplify the instruction
                Value *val = simplifyInstruction(&I, M->getDataLayout());

                // If the instruction was simplified, replace it with the simplified value
                if (val != nullptr) {
                    I.replaceAllUsesWith(val);
                    toEraseSimplify.push_back(&I);
                }
            }

            // Remove simplified instructions from the basic block
            if (toEraseSimplify.size() > 0) {
                // Erase the instructions marked for elimination (simplification
                for (Instruction *I : toEraseSimplify) {
                    DEBUG_PRINT("erasing simplified instruction:\n\t");
                    debugPrintLLVMInstr(*I);
                    DEBUG_PRINT("\n");
                    I->eraseFromParent();
                    CSESimplify++;
                }
            }
        }
    }

    DEBUG_PRINT("Simplify instruction end\n");
}


// --------------------------------------------------------------------------------
//                      Optimization 2: Common Subexpression Elimination
// --------------------------------------------------------------------------------
/**
 * @brief Checks if the given LLVM instruction has side effects.
 * 
 * This function determines whether the given instruction has side effects
 * based on its opcode. Instructions with certain opcodes are considered
 * to have side effects, such as calls, stores, allocations, loads, etc.
 * 
 * @param I Reference to the LLVM instruction to be checked.
 * @return true if the instruction has side effects, false otherwise.
 */
static bool isSideEffectInstruction(Instruction &I) {
    // Check if the opcode of the instruction indicates a side effect
    return (
        (I.getOpcode() == Instruction::Call)        ||
        (I.getOpcode() == Instruction::Store)       ||
        (I.getOpcode() == Instruction::Alloca)      ||
        (I.getOpcode() == Instruction::Load)        ||
        (I.getOpcode() == Instruction::Fence)       ||
        (I.getOpcode() == Instruction::Br)          ||
        (I.getOpcode() == Instruction::Invoke)      ||
        (I.getOpcode() == Instruction::Resume)      ||
        (I.getOpcode() == Instruction::Unreachable)
    );
}


/**
 * @brief Checks if the given LLVM instructions match each other as literals.
 * 
 * This function compares two LLVM instructions to determine if they match
 * each other as literals. Matching requires the instructions to have the
 * same opcode, type, number of operands, and order of operands. Additionally,
 * for compare instructions (FCmp and ICmp), their predicates must match.
 * 
 * @param I Reference to the first LLVM instruction to be compared.
 * @param J Reference to the second LLVM instruction to be compared.
 * @return true if the instructions match as literals, false otherwise.
 */
static bool isLiteralMatch(Instruction &I, Instruction &J) {
    // Check if the instructions are compare instructions (FCmp or ICmp)
    if (I.getOpcode() == Instruction::FCmp) {
        FCmpInst *FCI = dyn_cast<FCmpInst>(&I);
        FCmpInst *FCJ = dyn_cast<FCmpInst>(&J);
        // If either instruction is not an FCmpInst, they don't match
        if (!FCI || !FCJ) {
            return false;
        }
        // If the predicates of the FCmpInsts don't match, they don't match as literals
        if (FCI->getPredicate() != FCJ->getPredicate()) {
            return false;
        }
    }
    else if (I.getOpcode() == Instruction::ICmp) {
        ICmpInst *ICI = dyn_cast<ICmpInst>(&I);
        ICmpInst *ICJ = dyn_cast<ICmpInst>(&J);
        // If either instruction is not an ICmpInst, they don't match
        if (!ICI || !ICJ) {
            return false;
        }
        // If the predicates of the ICmpInsts don't match, they don't match as literals
        if (ICI->getPredicate() != ICJ->getPredicate()) {
            return false;
        }
    }

    // Check additional conditions for literal matching
    return (
        (!isSideEffectInstruction(I)) &&                     // Not a side effect instruction I
        (!isSideEffectInstruction(J)) &&                     // Not a side effect instruction J
        (I.isIdenticalTo(&J))         &&
        (I.getOpcode() == J.getOpcode()) &&                  // Same opcode
        (I.getType() == J.getType()) &&                      // Same type
        (I.getNumOperands() == J.getNumOperands()) &&        // Same number of operands
        (std::equal(I.op_begin(), I.op_end(), J.op_begin())) // Same order of operands
    );
}


/**
 * @brief Performs common subexpression elimination (CSE) on the given LLVM module.
 * 
 * This function iterates over all functions in the module and performs CSE
 * by identifying and eliminating common subexpressions within each function.
 * Common subexpressions are instructions that compute the same value and can
 * be replaced with a single instruction.
 * 
 * @param M Pointer to the LLVM module to perform CSE on.
 */
void performCSE(Module *M) {
    DEBUG_PRINT("CSE start\n");

    // Iterate over all functions in the module
    for (Function &F : *M) {
        if (!F.empty()) {
            // Construct a dominator tree for the function
            DominatorTree DT(F);
            DT.recalculate(F);
            std::vector<Instruction*> toEraseCSE;

            // Iterate over all basic blocks in the function
            for (BasicBlock &BB : F) {
                // Iterate over all nodes in the dominator tree
                for (DomTreeNodeBase<BasicBlock> *BBDomTreeNode : depth_first(DT.getRootNode())) {
                    if (BBDomTreeNode) {
                        BasicBlock *BBDomTree = BBDomTreeNode->getBlock();
                        if (BBDomTree == &BB) { // same block
                            // Iterate over all instructions in the basic block
                            for (Instruction &I : BB) {
                                // Iterate over all instructions in the same basic block of the dominator tree node
                                for (Instruction &J : *BBDomTree) {
                                    // Check if the instructions are different, I dominates J, and they literally match
                                    if ((&I != &J) && DT.dominates(&I, &J) && isLiteralMatch(I, J)) {
                                        DEBUG_PRINT("found CSE in the same block\n");
                                        debugPrintLLVMInstr(J);
                                        DEBUG_PRINT("\n");
                                        // Replace J with I and add J to the list of instructions to erase
                                        J.replaceAllUsesWith(&I);
                                        toEraseCSE.push_back(&J);
                                    }
                                }
                            }
                        }
                        // Check if the current basic block dominates the dominator tree node
                        else if (DT.dominates(&BB, BBDomTree) && (&BB != BBDomTree)){ // different block and is dominated
                            DEBUG_PRINT("BB dominates BBDomTree" << BBDomTree->getName() << "\n");
                            // Iterate over all instructions in the basic block
                            for (Instruction &I : BB) {
                                // Iterate over all instructions in the dominated basic block of the dominator tree node
                                for (Instruction &J : *BBDomTree) {
                                    // Check if the instructions match as literals
                                    if (isLiteralMatch(I, J)) {
                                    // if (I.isIdenticalTo(&J)) {
                                        DEBUG_PRINT("found CSE in the dominated block " << BBDomTree->getName() <<"\n");
                                        debugPrintLLVMInstr(J);
                                        DEBUG_PRINT("\n");
                                        // Replace J with I and add J to the list of instructions to erase
                                        J.replaceAllUsesWith(&I);
                                        toEraseCSE.push_back(&J);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Erase instructions marked for elimination
            if (toEraseCSE.size() > 0) {
                for (Instruction *I : toEraseCSE) {
                    // Check if the instruction's parent is not null before erasing it
                    if (I->getParent()) {
                        DEBUG_PRINT("erasing CSE instruction: \n\t");
                        debugPrintLLVMInstr(*I);
                        DEBUG_PRINT("\n");
                        I->eraseFromParent();
                        CSEElim++;
                    }
                }
            }
        }
    }

    DEBUG_PRINT("CSE end\n");
}

// --------------------------------------------------------------------------------
//                      Optimization 3: Eliminate Redundant Loads
// --------------------------------------------------------------------------------
/**
 * @brief Checks if there are no intervening store or call instructions between two load instructions.
 * 
 * This function checks if there are no store or call instructions between the currentLoad and nextLoad
 * within the same basic block.
 * 
 * @param currentLoad Pointer to the current load instruction.
 * @param nextLoad Pointer to the next load instruction.
 * @return true if there are no intervening store or call instructions, false otherwise.
 */
static bool noInterveningStoresOrCalls(LoadInst *currentLoad, LoadInst *nextLoad) {
    BasicBlock *PBB = currentLoad->getParent();
    bool retVal = true;

    // Iterate over instructions starting from the instruction after currentLoad
    for (BasicBlock::iterator I = std::next(currentLoad->getIterator()); 
         (I != PBB->end()) && (&*I != nextLoad); 
         I++) {
        // Check if the instruction is a store or call
        if (I->getOpcode() == Instruction::Store || 
            I->getOpcode() == Instruction::Call) {
            retVal = false;
            break;
        }
    }
    return retVal;
}


/**
 * @brief Eliminates redundant load instructions within each function in the LLVM module.
 * 
 * This function iterates over all functions in the module and within each function, it
 * iterates over all basic blocks to identify and eliminate redundant load instructions.
 * A load instruction is considered redundant if there is another load instruction later
 * in the same basic block that loads the same address, has the same type of operand, and
 * has no intervening store or call instructions between them.
 * 
 * @param M Pointer to the LLVM module to eliminate redundant loads from.
 */
static void EliminateRedundantLoads(Module *M) {
    DEBUG_PRINT("Eliminate redundant loads start\n");

    // Iterate over all functions in the module
    for (Function &F : *M) {
        // Iterate over all basic blocks in the function
        for (BasicBlock &BB : F) {
            // Vector to collect redundant loads within the basic block
            std::vector<Instruction*> toEraseRedundantLoads;
            bool moveToNextLoad = false;

            // Iterate over all instructions in the basic block
            for (Instruction &I : BB) {
                // Check if the instruction is a load
                if (I.getOpcode() == Instruction::Load) {
                    LoadInst *LI = dyn_cast<LoadInst>(&I);
                    
                    // Iterate over all instructions after I in the basic block
                    for (Instruction &J : llvm::make_range(std::next(I.getIterator()), BB.end())) {
                        // Check if J is a store instruction
                        if (J.getOpcode() == Instruction::Store) {
                            moveToNextLoad = true;
                            break;
                        }
                        // Check if J is a load instruction
                        if (J.getOpcode() == Instruction::Load) {
                            LoadInst *LJ = dyn_cast<LoadInst>(&J);
                            // Check if LI and LJ are identical and there are no intervening stores or calls
                            // if (
                            if ( 
                                LJ != nullptr &&
                                (!LJ->isVolatile()) &&
                                (LJ->getPointerOperand() == LI->getPointerOperand()) &&
                                (LJ->getType() == LI->getType()) &&
                                (noInterveningStoresOrCalls(LI, LJ))
                               ) {
                                    DEBUG_PRINT("redundant load found\n");
                                    debugPrintLLVMInstr(*LJ);
                                    // Replace uses of LJ with LI and mark LJ for erasing
                                    LJ->replaceAllUsesWith(LI);
                                    toEraseRedundantLoads.push_back(LJ);
                            }
                        }
                    }
                    if (moveToNextLoad) {
                        continue;
                    }
                }
            }
            
            // Eliminate collected redundant loads and update the counter
            if (toEraseRedundantLoads.size() > 0) {
                for (Instruction *redload : toEraseRedundantLoads) {
                    if (redload->getParent()) {
                        DEBUG_PRINT("erasing redundant load: \n\t");
                        debugPrintLLVMInstr(*redload);
                        redload->eraseFromParent();
                        CSELdElim++;
                    }
                }
            }
        }
    }

    DEBUG_PRINT("Eliminate redundant loads end\n");
}

// --------------------------------------------------------------------------------
//                      Optimization 4: Eliminate Redundant Stores
// --------------------------------------------------------------------------------
/**
 * @brief Eliminates redundant store instructions from the given LLVM module.
 * 
 * This function iterates over all functions in the module and within each function's
 * basic blocks, it identifies and eliminates redundant store instructions.
 * Redundant store instructions are those that store the same value to the same memory
 * address as another store instruction earlier in the same basic block.
 * 
 * @param M Pointer to the LLVM module to eliminate redundant stores from.
 */
static void EliminateRedundantStores(Module *M) {
    DEBUG_PRINT("Eliminate redundant stores start\n");

    // Iterate over all functions in the module
    for (Function &F : *M) {
        // Iterate over all basic blocks in the function
        for (BasicBlock &BB : F) {
            std::vector<Instruction*> toEraseRedundantLoads;
            std::vector<Instruction*> toEraseRedundantStores;
            bool moveToNextStore = false;
            bool redInstrFound = false;

            // Iterate over all instructions in the basic block
            for (Instruction &I : BB) {
                // Check if the instruction is a store
                if (I.getOpcode() == Instruction::Store) {
                    StoreInst *SI = dyn_cast<StoreInst>(&I);

                    // Iterate over instructions after the current store in the basic block
                    for (Instruction &R : llvm::make_range(std::next(I.getIterator()), BB.end())) {
                        // Check if the next instruction is a load
                        if (R.getOpcode() == Instruction::Load) {
                            LoadInst *LIR = dyn_cast<LoadInst>(&R);

                            // Check if the load matches the current store
                            if ((!LIR->isVolatile()) &&                                    // load is not volatile
                                (LIR->getPointerOperand() == SI->getPointerOperand()) &&   // loads the same address
                                (LIR->getType() == SI->getValueOperand()->getType())) {    // loads the same type of operand
                                DEBUG_PRINT("redundant load found\n");
                                debugPrintLLVMInstr(*LIR);
                                LIR->replaceAllUsesWith(SI->getValueOperand());
                                toEraseRedundantLoads.push_back(LIR);
                                redInstrFound = true;
                            }
                        }
                        // Check if the next instruction is a store
                        else if (R.getOpcode() == Instruction::Store) {
                            StoreInst *SIR = dyn_cast<StoreInst>(&R);

                            // Check if the stores are redundant
                            if ((!SI->isVolatile()) &&                                                      // current store is not volatile
                                (SIR->getPointerOperand() == SI->getPointerOperand()) &&                    // stores the same address
                                (SIR->getValueOperand()->getType() == SI->getValueOperand()->getType())) {  // stores the same type of operand
                                DEBUG_PRINT("redundant store found\n");
                                debugPrintLLVMInstr(*SIR);
                                toEraseRedundantStores.push_back(SI);
                                redInstrFound = true;
                                moveToNextStore = true;
                                break;
                            }
                        }
                        if (!redInstrFound) {
                            // Check if the next instruction is a call
                            if (isSideEffectInstruction(R)) {
                                moveToNextStore = true;
                                break;
                            }
                        }
                    }

                    // Move to the next store if needed
                    if (moveToNextStore) {
                        continue;
                    }
                }
            }

            // Erase redundant loads and stores
            if (toEraseRedundantLoads.size() > 0) {
                for (Instruction *redload : toEraseRedundantLoads) {
                    DEBUG_PRINT("erasing redundant load: \n\t");
                    debugPrintLLVMInstr(*redload);
                    DEBUG_PRINT("\n");
                    redload->eraseFromParent();
                    CSEStore2Load++;
                }
            }
            if (toEraseRedundantStores.size() > 0) {
                for (Instruction *redstore : toEraseRedundantStores) {
                    DEBUG_PRINT("erasing redundant store: \n\t");
                    debugPrintLLVMInstr(*redstore);
                    DEBUG_PRINT("\n");
                    redstore->eraseFromParent();
                    CSEStElim++;
                }
            }
        }
    }
}

// --------------------------------------------------------------------------------
//                      Call all optimizations here
// --------------------------------------------------------------------------------
static void CommonSubexpressionElimination(Module *M) {
    int i = 3;
    while (i > 0) {
        DEBUG_PRINT(" ----- iteration: " << (4 - i) << "------" << "\n");
        DeadCodeElimination(M);
        SimplifyInstructions(M);
        performCSE(M);
        EliminateRedundantLoads(M);
        EliminateRedundantStores(M);
        i--;
    }
}
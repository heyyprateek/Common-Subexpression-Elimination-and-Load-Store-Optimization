# Common-Subexpression-Elimination-and-Load-Store-Optimization
The objectives include implementing code for Common Subexpression Elimination, leveraging LLVM for instruction simplification and dead code elimination, and performing a simple load and store optimization during the CSE traversal to identify additional redundancy while maintaining the order of memory operations.
In this project, I executed Common Subexpression Elimination along with a few other basic optimizations. Each optimization is detailed below.

**Optimization 0 - Dead Code Elimination:** In this pass I eliminate dead instructions while visiting each instruction for CSE. If an instruction is found to be dead, it will be removed, and then the flow will proceed to the next one. A counter named CSEDead will be created to tally all eliminated instructions. It's crucial to note that this optimization does not involve buffering instructions in a worklist for removal later; instead, dead instructions are removed on the go without actively searching for all dead instructions.

**Optimization 1a - Instruction Simplification:** Instructions will be simplified during the CSE traversal by checking if they can be simplified through simple constant folding. A counter named `CSESimplify` will be incremented for all instructions simplified.

**Optimization 1b - Common Subexpression Elimination:** For each instruction, all other instructions with identical characteristics gets eliminated. These characteristics include the same opcode, same type, same number of operands, and same operands in the same order (without commutativity). The implementer will decide which opcodes can be eliminated by CSE. To implement CSE, a nested loop or recursion on the dominator tree will be utilized. The counter `CSEElim` will be created to count all instructions eliminated by CSE.

**Optimization 2 - Redundant Load Elimination:** Redundant loads within the same basic block will be eliminated. If a load is encountered, the algorithm will search for redundant loads within the same basic block and replace them accordingly. A counter named `CSERLoad` will be incremented for each redundant load eliminated.

**Optimization 3 - Redundant Store Elimination:** Similarly, redundant stores to the same address with no intervening loads will be eliminated. If two stores to the same address are found and the earlier one is not volatile, it will be removed. Additionally, if there is a non-volatile load to the same address after the store within the same basic block, all uses of the load will be replaced with the store's data operand. Counters named `CSEStore2Load` will track the relevant eliminations.

The code will also include functionality to print a total count of all instructions removed, as well as a breakdown across each optimization category.

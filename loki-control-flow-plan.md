# LOKI Control Flow Extension — Implementation Plan

Note: use the `devcontainer` cli to work on the project.

## Context

LOKI is a VM-based code obfuscation system built on LLVM (C++) and a Rust code transformation component. It protects straight-line code using four interlocking techniques: key-merged multi-semantics handlers, factorization-based and synthesized partial point function key encodings, recursively synthesized MBA expressions, and chained superoperators. Its prototype does not support control flow (branches, loops, function calls). LLVM's `-O3` passes (loop unrolling, inlining, etc.) are used to linearize code before loki processes it.

This plan describes how to extend loki with control flow support in three tiers of increasing strength. Each tier is self-contained and shippable. An implementer should complete Tier 1 before moving to Tier 2, and so on — each tier's output is the foundation for the next.

The codebase has two main components:

- **LLVM component** (~3,100 LOC C++): compiler passes that lift target functions to LLVM IR, optimize/unroll, and lift to loki's custom IR. Also handles final compilation of obfuscated LLVM IR back to native code.
- **Rust component** (~8,700 LOC Rust): parses the custom IR, builds superoperators, instantiates VM handlers, applies MBAs, generates bytecode, and translates handlers back to LLVM IR.

The VM has 510 arithmetic/logic handlers, 1 memory handler, and 1 VM exit handler. Each handler implements `f(x, y, c, k)` with 3–5 merged core semantics selected by key `k`. The dispatcher uses direct threaded code (each handler inlines the dispatch to the next).


---


## Tier 1 — Block-Level Virtualization with Native Stitching

**Goal:** Support arbitrary control flow by virtualizing individual basic blocks (or extended basic blocks) independently, connected by native branch/call instructions.

**Security posture:** Each block has full loki protection. Control-flow graph is visible to the attacker in native code. Equivalent to calling loki as a per-block black box.


### Phase 1.1 — Analysis and Block Selection

**Objective:** Given an LLVM IR function with arbitrary control flow, identify which blocks to virtualize and which to leave as native glue.

Steps:

1. **Add a CFG partitioning pass** (new LLVM `FunctionPass`, C++).
   - Walk the function's basic blocks. For each block, compute an "obfuscation value" score based on:
     - Number of arithmetic/logic instructions (higher = more valuable to protect).
     - Number of instructions that loki can already handle (its 510-handler ISA covers: binary arithmetic, bitwise ops, comparisons, sign/zero extension, truncation, loads/stores via the memory handler).
     - Absence of unsupported constructs (e.g., floating point, vector ops, inline asm, exception handling intrinsics).
   - Mark blocks scoring above a configurable threshold as *virtualize candidates*.
   - Mark blocks containing only branches, phis, or trivial glue as *native*.

2. **Merge adjacent virtualizable blocks** into *extended regions*.
   - Walk the CFG. If a virtualize-candidate block has exactly one successor, and that successor has exactly one predecessor, merge them into a single region. Repeat until no more merges are possible. This maximizes the straight-line code available to loki's superoperator construction.
   - Record the region boundaries: for each region, note the set of live-in SSA values (inputs) and live-out SSA values (outputs), plus the region's terminator type (unconditional branch, conditional branch, switch, return).

3. **Emit region metadata** as a JSON or custom IR annotation that the Rust component can consume.
   - For each region: region ID, list of LLVM IR instructions (in loki's existing custom IR format), input variables, output variables, terminator info.

**Key design decision:** Regions should be as large as possible. If LLVM can prove a branch condition is loop-invariant or a comparison of values already being computed, pull the branch condition computation *into* the preceding region. Only split at branches whose condition depends on values computed by different regions or external inputs. This directly impacts superoperator depth — see the note on block size below.

**Testing:**
- Unit test the partitioning pass on the 5 existing benchmark targets (AES, DES, MD5, RC4, SHA-1) compiled *without* loop unrolling. Verify that regions are non-empty and cover all virtualizable instructions.
- Verify that live-in/live-out sets are correct by comparing against LLVM's own liveness analysis.


### Phase 1.2 — Per-Region Virtualization

**Objective:** Pipe each region through loki's existing Rust transformation pipeline independently.

Steps:

1. **Adapt the Rust component's entry point** to accept multiple independent regions per function, rather than a single linearized function.
   - Currently the Rust component expects one contiguous sequence of custom IR instructions. Modify it to accept a list of regions, each with its own instruction sequence and input/output variable bindings.
   - For each region, the existing pipeline runs unchanged: parse → build superoperators (recursion bound 3–12) → instantiate handlers → apply MBAs (recursion bound 20–30) → verify → generate bytecode → emit LLVM IR.

2. **Handle region inputs/outputs.**
   - Each region's input variables map to the handler's `(x, y, c)` slots (or virtual registers for regions needing more inputs). If a region needs more than 2 input variables + 1 constant, extend the handler's register file. This may already be handled by loki's existing multi-handler regions — verify.
   - Each region's output variables must be written to locations the native glue code can read. The simplest approach: use loki's existing memory handler to write outputs to a known stack-allocated struct before VM exit.

3. **Emit per-region VM entry/exit code** as LLVM IR.
   - VM entry: allocate the VM context (virtual registers, virtual stack, bytecode pointer), copy live-in values from LLVM SSA registers into the VM context, jump to the first handler.
   - VM exit: copy live-out values from the VM context back to LLVM SSA registers.
   - This replaces the original region's instructions in the LLVM IR function.

**Testing:**
- For each benchmark target, verify that the obfuscated function produces correct output for 1,000 random inputs (matching loki's existing correctness test methodology).
- Verify that the number of unique MBAs per region is comparable to the full-function case (should be, since the MBA pipeline is unchanged).


### Phase 1.3 — Native Control-Flow Stitching

**Objective:** Reconnect the virtualized regions with native LLVM IR branch instructions.

Steps:

1. **Replace original basic blocks** with the VM entry/exit sequences from Phase 1.2.
   - For unconditional branches: the block ends with VM exit, followed by a native `br` to the successor's VM entry (or native block if the successor is not virtualized).
   - For conditional branches: the branch condition is one of the region's live-out values. After VM exit, emit a native `br i1 %cond, label %true_target, label %false_target`.
   - For switches: same pattern, with a native `switch` on the live-out discriminant.
   - For returns: VM exit writes the return value, followed by a native `ret`.

2. **Handle PHI nodes** at region boundaries.
   - LLVM's PHI nodes select values based on predecessor block. Since virtualized regions may have been merged, the predecessor labels need updating. Walk all PHI nodes in the function and remap incoming block labels to the new VM-exit blocks.

3. **Run LLVM verification and optimization.**
   - Run `verifyFunction()` to catch IR invariant violations.
   - Run a *light* optimization pass (`-O1` or custom pipeline) on the glue code only. Do NOT re-optimize the virtualized regions' LLVM IR — it was already emitted by the Rust component in its final form. Use LLVM's `optnone` attribute or a custom pass to protect virtualized code from being optimized away.

**Testing:**
- End-to-end correctness: compile the fully stitched function, run on all benchmark inputs, compare outputs.
- CFG integrity: dump the CFG of the final IR and verify it is isomorphic to the original (modulo block splitting from VM entry/exit insertion).
- Performance: measure overhead vs. the current loop-unrolled approach. Expect somewhat lower overhead (less code duplication from unrolling) but slightly weaker per-block superoperators (smaller regions).

**Deliverable:** A working loki that can obfuscate functions with branches, loops, and function calls, with full per-block MBA/superoperator/key-encoding protection and a native-code CFG.


---


## Tier 2 — Internalized VM Control Flow

**Goal:** Move control flow inside the VM interpreter so the CFG is hidden behind the bytecode dispatcher. The attacker must reverse the VM to recover the program's branch structure.

**Security posture:** Per-block protection unchanged. CFG is now protected by the VM's opaque dispatch mechanism. Comparable to Themida/VMProtect's basic virtualization mode.


### Phase 2.1 — VM ISA Extension

**Objective:** Add control-flow instructions to loki's bytecode ISA.

New handler types to implement (in the Rust component):

1. **`vm_branch_uncond(target_offset)`** — Unconditional jump. Updates the virtual PC by `target_offset` (relative offset into the bytecode stream).

2. **`vm_branch_cond(cond_reg, true_offset, false_offset)`** — Conditional branch. Reads a virtual register, jumps to `true_offset` if nonzero, `false_offset` otherwise.

3. **`vm_switch(discriminant_reg, default_offset, table[])`** — Switch dispatch. Reads a virtual register, looks up the target offset in a jump table, falls through to `default_offset` if not found.

4. **`vm_call(callee_id, arg_regs[], ret_reg)`** — Internal VM call. Pushes a return address onto a virtual call stack, transfers to a different bytecode region. Used for intra-function calls to virtualized helper blocks.

5. **`vm_return(ret_reg)`** — VM exit. Writes the return value and terminates the VM interpreter loop.

Design notes:
- All offsets should be encoded as indices into the bytecode array, not absolute addresses, to avoid leaking layout information.
- The `target_offset` values should be encrypted or obfuscated (see Phase 2.3).
- The dispatcher already uses direct threaded code (each handler jumps to the next). For branch handlers, the "next handler" is determined by the branch outcome rather than a sequential increment of the bytecode pointer.

**Implementation:**
- Add the new handler types to the Rust component's handler registry. Each new handler needs a code template that emits the appropriate LLVM IR (a conditional or unconditional branch within the dispatcher's threaded code structure).
- Update the bytecode encoder to emit the new opcodes with their operand encodings.
- Update the bytecode decoder (in the dispatcher) to recognize the new opcodes.

**Testing:**
- Unit test each new handler in isolation: create a minimal bytecode program that exercises the handler, run it, verify correct control transfer.
- Test nested branches, back-edges (loops), and multi-level switches.


### Phase 2.2 — Unified Bytecode Emission

**Objective:** Instead of emitting one independent bytecode stream per region (Tier 1), emit a single bytecode stream for the entire function with internal branch instructions connecting regions.

Steps:

1. **Modify the Rust component's code generator** to accept the full set of regions (from Tier 1's Phase 1.1) and lay them out sequentially in a single bytecode buffer.
   - Assign each region a bytecode offset.
   - At region boundaries where the original code had a branch, emit the appropriate `vm_branch_*` instruction with offsets pointing to the target region's start.

2. **Resolve forward references.**
   - Use a two-pass approach: first pass emits all handlers and records placeholder offsets for branches; second pass patches the offsets once all regions are laid out.

3. **Eliminate per-region VM entry/exit overhead.**
   - In Tier 1, each region had its own VM entry (context setup) and VM exit (context teardown). Now there is a single VM entry at the function's start and a single VM exit at the function's return points.
   - Virtual registers persist across regions within the same VM invocation. This means live-out values from one region are directly available as live-in values for successor regions — no copying to/from native SSA registers.

4. **Handle PHI nodes inside the VM.**
   - LLVM's PHI nodes don't have a direct analogue in the VM's register-machine model. At each join point, insert `vm_move` instructions at the end of each predecessor region to write the correct value to the PHI's destination virtual register. This is the standard "PHI elimination" transformation — LLVM has a pass for this (`-reg2mem` or manual insertion of copies).

**Testing:**
- Correctness: same methodology as Tier 1, but now the entire function runs inside a single VM invocation.
- Verify that the bytecode stream is a valid encoding by adding a bytecode validator (checks that all branch targets point to valid handler starts, no out-of-bounds offsets, etc.).


### Phase 2.3 — Control-Flow Obfuscation Within the VM

**Objective:** Make the VM-level control flow harder to analyze.

Techniques to implement:

1. **Opaque predicates on branch conditions.**
   - Before each `vm_branch_cond`, insert a handler that computes an opaque predicate (an expression that always evaluates to true or false, but is hard for an attacker to determine statically). Use loki's existing MBA machinery to synthesize these.
   - Combine the opaque predicate with the real branch condition using a formula like: `real_cond XOR (opaque AND false_flag)`, where `false_flag` is a constant that makes the opaque predicate irrelevant. The MBA encoding hides this structure.

2. **Bytecode offset encryption.**
   - Don't store branch target offsets in the clear. Encrypt them with a key derived from the current handler's context (e.g., XOR with a hash of the current virtual PC). The dispatcher decrypts at runtime.
   - This prevents static disassembly of the bytecode from recovering the CFG.

3. **Bogus branch insertion.**
   - At random points in the bytecode, insert `vm_branch_cond` instructions whose condition is an opaque predicate that always takes one path. The dead path points to garbage bytecode or to a different region's middle (causing nonsensical control flow if an attacker tries to follow it statically).

4. **Dispatcher diversification.**
   - Instead of a single dispatcher implementation, generate N variants (e.g., 4–8) with different opcode-to-handler mappings and different threading mechanisms (direct threading, indirect threading via encrypted table, call threading). Switch between variants at branch points. This forces an attacker to reverse multiple dispatchers rather than one.

**Testing:**
- Correctness: unchanged methodology. Opaque predicates and offset encryption must not alter observable behavior.
- Security: for a sample of 100 obfuscated functions, run the MIASM-based CFG recovery tool from loki's evaluation suite. Measure how many bogus edges appear in the recovered CFG and how much time CFG recovery takes (should increase substantially vs. Tier 1).

**Deliverable:** A loki variant where the entire function (including all control flow) runs inside a single VM invocation, with an obfuscated bytecode-level CFG.


---


## Tier 3 — Integrated Semantics Across Control Flow

**Goal:** Extend loki's core security properties (key-merged multi-semantics, superoperators) across basic block boundaries, so that control-flow transitions are entangled with data computation and cannot be analyzed in isolation.

**Security posture:** This is the research-grade tier. Block boundaries cease to be clean seams. Taint analysis and synthesis attacks must consider cross-block data/control dependencies.


### Phase 3.1 — Cross-Block Superoperators

**Objective:** Build superoperators whose use-def chains span multiple basic blocks, so that a single handler computes values that feed into both the current block's output and a successor block's computation.

Steps:

1. **Extend the SSA-based superoperator construction** (Section 3.5 of the paper) to operate on the function's full SSA graph rather than a single region.
   - Currently, the algorithm picks a variable use, replaces it with its definition, and recurses. This works within a basic block because definitions dominate uses along a single path. Across blocks, you need to respect dominance: a definition in block A can only be inlined into a use in block B if A dominates B.
   - Use LLVM's dominator tree (available from the `DominatorTree` analysis pass) to determine which cross-block inlining is valid.

2. **Handle merge points (PHI nodes) in superoperators.**
   - A PHI node has multiple definitions (one per predecessor). You cannot inline a PHI's "definition" because it has no single definition. Options:
     - **Conservative:** never inline across PHI nodes. Superoperators are bounded by dominance regions between PHI nodes. This is safe but limits depth.
     - **Aggressive:** specialize the superoperator per incoming edge. For each predecessor, create a variant of the superoperator with that predecessor's PHI input inlined. This increases code size but produces deeper superoperators on each path.
   - Recommend starting conservative, measuring the impact on semantic depth, and adding the aggressive mode if depths are too shallow.

3. **Adjust the recursion bounds.**
   - Cross-block superoperators naturally reach higher depths because they draw from more instructions. But very deep expressions increase compilation time (MBA synthesis, SMT verification) and runtime overhead.
   - Add a configurable depth cap (separate from the existing 3–12 recursion bound) for cross-block inlining. Start with 3–5 cross-block steps.

**Testing:**
- Measure the distribution of semantic depths with cross-block superoperators vs. Tier 2 (intra-block only). Target: median depth ≥ 9 (matching the paper's Figure 4) even for functions with many small blocks.
- Correctness verification via the existing fuzzing approach (100 random inputs per binary, compare outputs).


### Phase 3.2 — Branch Predicate Integration

**Objective:** Fold branch condition computation into the preceding block's key-merged handler, so the branch decision is entangled with the data computation and protected by the same MBAs/key encodings.

Steps:

1. **Model the branch condition as an additional "core semantics" of the handler.**
   - Currently, a handler's output is a single data value selected by key `k`. Extend the handler to produce a *pair*: `(data_output, control_output)`, where `control_output` encodes the branch direction.
   - The key selection mechanism (`e_i(k)`) selects both the data semantics and the branch semantics simultaneously. An attacker cannot learn the branch condition without also solving the key encoding — which the paper shows is hard.

2. **Encode the branch target as a data value.**
   - Instead of a separate `vm_branch_cond` handler, the preceding handler writes the encrypted target offset to a designated virtual register. The dispatcher reads this register and jumps.
   - The target offset is computed as part of the MBA-obfuscated expression, making it indistinguishable from the data computation.

3. **Handle the case where the branch condition depends on values from multiple handlers.**
   - If the condition is a comparison of two values computed by different handlers (e.g., `handler_A_output < handler_B_output`), you cannot fold it into either handler alone.
   - Solution: introduce a "merge handler" that takes both values as inputs and computes the branch decision. Apply loki's full protection to this merge handler. It becomes a thin handler, but its MBA/key protection still holds.

**Testing:**
- Security: repeat the MIASM-based symbolic execution experiment from the paper (Experiment 7). For handlers with integrated branch predicates, measure whether a dynamic attacker can extract the branch condition as a standalone expression. Target: the branch condition should be inseparable from the data computation in the symbolic output.
- Correctness: critical. The branch encoding must be deterministic and produce the correct target for all inputs. Use loki's existing CEGAR-based verification to prove equivalence between the original branch condition and the obfuscated version.


### Phase 3.3 — Loop Hardening

**Objective:** Special handling for loops to prevent attackers from exploiting the repetitive structure.

Steps:

1. **Loop detection and classification.**
   - Use LLVM's `LoopInfo` analysis to identify all loops in the target function.
   - Classify each loop:
     - **Bounded, small trip count:** can be fully unrolled. Use LLVM's unroller (existing behavior). Loki processes the unrolled straight-line code with full strength.
     - **Bounded, large trip count:** partial unrolling. Unroll N iterations (e.g., 4–8), virtualize the unrolled body as a single region, and keep a native loop around it. The unrolled body gives superoperators more material.
     - **Unbounded / data-dependent:** cannot be unrolled. Virtualize the loop body as a single region. The back-edge is a `vm_branch_cond` (from Tier 2) with the loop condition integrated into the body's handler (from Phase 3.2 above).

2. **Loop body diversification.**
   - For loops that execute many iterations, an attacker can observe the same handler sequence repeating and correlate iterations. Mitigate by generating K variants of the loop body (e.g., K = 3–5), each with different MBA instantiations, key encodings, and superoperator structures, but identical semantics.
   - At each iteration, the dispatcher rotates through variants. The rotation can be deterministic (round-robin keyed by iteration count) or pseudorandom (keyed by a hash of the virtual PC and iteration counter).
   - This forces an attacker to analyze K×(handler count) handlers instead of just (handler count).

3. **Iteration-dependent key rotation.**
   - Extend the key `k` mechanism: instead of a static key per handler invocation, derive `k` as a function of the loop iteration counter. This means the same handler implements different core semantics on different iterations, unless the attacker knows the exact iteration.
   - Implement by adding the iteration counter as an additional input to the key encoding `e_i(k, iter)`.

**Testing:**
- Correctness: test with loops of various trip counts (0, 1, small, large, data-dependent). Compare outputs against non-obfuscated version.
- Performance: measure overhead of loop body diversification. K variants multiply the code size of the loop body; verify this stays within acceptable bounds (< 10x original loop body size).
- Security: run SYNTIA against the loop body handlers across different iterations. Verify that the synthesis success rate does not improve compared to the non-loop case (it shouldn't if diversification is working).


### Phase 3.4 — Whole-Function Verification

**Objective:** Ensure the combined Tier 3 transformations preserve correctness.

Steps:

1. **Extend the existing fuzzing-based verification.**
   - The paper uses 100 random inputs per binary. For control-flow-rich functions, 100 inputs may not exercise all paths. Increase to 10,000 inputs and use coverage-guided fuzzing (e.g., AFL or libFuzzer in LLVM) to maximize path coverage.
   - Compare the obfuscated function's output against the original for every input. Any divergence is a correctness bug.

2. **Per-handler formal verification.**
   - Loki already uses SMT-based equivalence checking for individual MBA expressions. Extend this to the new handler types:
     - Verify that branch predicate integration (Phase 3.2) preserves the original branch condition.
     - Verify that cross-block superoperators (Phase 3.1) produce the same outputs as the original instruction sequence they replace.
   - This is tractable because verification is per-handler, not whole-program.

3. **Regression test suite.**
   - Maintain a suite of test functions covering: nested loops, switches, early returns, recursive calls (if supporting inter-function virtualization), exception-like patterns (setjmp/longjmp), and the 5 existing cryptographic benchmarks.
   - Run the full correctness + resilience evaluation pipeline on every commit to the Rust component.

**Deliverable:** A research-grade loki extension where control flow is deeply integrated with data obfuscation, branch conditions are protected by the same mechanisms as data handlers, and loops are diversified across iterations.


---


## Cross-Cutting Concerns

### Block Size vs. Superoperator Depth Tradeoff

This is the single most important parameter to get right. The paper shows synthesis resistance drops sharply below semantic depth 7 (Figure 5). Superoperator depth depends on the length of use-def chains available, which depends on region size.

Recommendations:
- Instrument the superoperator builder to log the achieved semantic depth distribution for every obfuscated function.
- Set a minimum region size of 15–20 IR instructions. If a region is smaller, try to merge it with a dominating predecessor or dominated successor.
- If merging is not possible (e.g., the region is a loop header with multiple predecessors), flag it as "low-depth warning" in the build output. The user can decide to apply extra MBA recursion (bound 40–55 instead of 20–30) to compensate with syntactic complexity.


### Handler Register File Extension

Loki's current handler signature is `f(x, y, c, k)` — two inputs, one constant, one key. Real basic blocks often have more than two live-in values. The existing design handles this via multiple handlers per block (each consuming two inputs from virtual registers). Verify that this mechanism works correctly when blocks are larger (Tier 1 merging) and when cross-block superoperators (Tier 3) pull in additional variables. If not, extend the handler signature to `f(x0, x1, ..., xN, c, k)` with N configurable (default 2 for backward compatibility, up to 4–6 for complex blocks).


### Performance Budget

The paper reports 300–480x runtime overhead for the current straight-line design. Adding control flow should not significantly increase per-handler overhead (handlers are unchanged). The overhead growth comes from:
- Additional handlers for branch/jump instructions (small: these are fast dispatch operations).
- Loop body diversification (Tier 3): K variants multiply code size.
- Reduced unrolling efficiency: data-dependent loops can't be unrolled, so the VM dispatch overhead is paid per iteration instead of being amortized over an unrolled body.

Target: stay within 500–1000x for Tier 2, 800–1500x for Tier 3. These are consistent with commercial obfuscators (Themida Dolphin Black is 2,400–11,700x per Table 2 in the paper).


### Evaluation Checklist (Per Tier)

For each completed tier, run the full evaluation from the paper:

1. **Correctness:** 1,000 random inputs × 5 benchmark targets × 1,000 obfuscated instances. All outputs must match.
2. **Overhead:** measure runtime factor and binary size factor vs. unobfuscated baseline. Report per-benchmark and averaged.
3. **Dead code elimination:** measure assembly instructions per handler before and after DCE. Target: < 5% reduction (matching loki's current 1.14%).
4. **Forward taint analysis:** byte-level and bit-level. Measure percentage of instructions unmarked. Target: < 20%.
5. **Backward slicing:** measure percentage of instructions not sliced. Target: < 10%.
6. **Symbolic execution:** static and dynamic attacker. Measure percentage of handlers simplified. Target: 0% static, < 20% dynamic.
7. **Program synthesis (SYNTIA):** 10,000 randomly sampled core semantics. Measure synthesis success rate. Target: < 25%.
8. **MBA diversity:** count unique MBAs per 7,000 handlers. Target: > 75% unique.
9. **(New, Tier 2+) CFG recovery:** measure time and accuracy of MIASM-based CFG reconstruction. Report number of spurious edges from bogus branches.
10. **(New, Tier 3) Branch predicate extraction:** measure whether symbolic execution can isolate the branch condition from the data computation.




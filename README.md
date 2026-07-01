# Loki: Hardening Code Obfuscation Against Automated Attacks
<a href="https://www.usenix.org/conference/usenixsecurity22/presentation/schloegel"> <img align="right" width="200" src="pictures/first_page.png"> </a>

Loki is an academic obfuscator prototype developed to showcase VM handler hardening techniques.

It is based on our [USENIX Security '22 paper](https://www.usenix.org/conference/usenixsecurity22/presentation/schloegel):

```
@inproceedings{schloegel2022loki,
    author = {Moritz Schloegel and Tim Blazytko and Moritz Contag and Cornelius Aschermann and Julius Basler and Thorsten Holz and Ali Abbasi},
    title =  {{Loki}: Hardening Code Obfuscation Against Automated Attacks},
    year = {2022},
    booktitle = {USENIX Security Symposium}
}
```

This repository contains the original research prototype and evaluation tooling, plus branch-local build and virtualization updates for reproducible Linux development and CFG-heavy C/C++ samples. The project remains research code and is not production-ready. Unless otherwise specified, code and data are licensed under AGPL3. Loki received the *Artifact Available*, *Artifact Functional*, and *Artifact Reproduced* badges from the USENIX Artifact Evaluation Committee.

## Further resources

We released an additional [artifact on Zenodo](https://zenodo.org/record/6686932) containing binaries and data produced during the evaluation.

A [technical report](https://arxiv.org/abs/2106.08913) with more details is available on arXiv.

# Structure

1. [loki](./loki): obfuscator prototype, translator, Rust obfuscator, testcases, and scripts to generate obfuscated targets
2. [lokiattack](./lokiattack): attack tooling for binaries obfuscated by Loki
3. [experiments](./experiments): documented evaluation experiments and reproduction scripts
4. [vmsamples](./vmsamples): Linux VM sample suite and Loki-virtualized shared-library validation scripts

# Installation

The easiest supported workflow is the devcontainer image. The Dockerfile can also build the environment locally.

## Devcontainer

The devcontainer uses:

```text
ghcr.io/llvmparty/loki:20230720-18.04-llvm9
```

Open the repository in a devcontainer-capable editor, or use the Dev Containers CLI:

```bash
devcontainer up --workspace-folder .
devcontainer exec --workspace-folder . bash
```

The devcontainer mounts the repository at `/home/user/loki` and runs `./setup.sh` after creation.

## Build Docker image locally

Run:

```bash
./docker_build.sh
```

The build uses BuildKit and compiles LLVM with all available cores while serializing LLVM link jobs by default:

```bash
PARALLEL_JOBS=$(nproc)
LLVM_PARALLEL_LINK_JOBS=1
```

Override either variable when needed:

```bash
PARALLEL_JOBS=16 LLVM_PARALLEL_LINK_JOBS=1 ./docker_build.sh
```

Ubuntu 18.04 requires ESM packages for reproducible 2026 builds. If you have an Ubuntu Pro attach config, provide it as a BuildKit secret without committing it:

```bash
PRO_ATTACH_CONFIG=pro-attach-config.yaml ./docker_build.sh
```

`pro-attach-config*.yaml` is ignored by git.

The image keeps LLVM installed under `/llvm` and avoids carrying full LLVM source/build trees in the final image. LLVM remains pinned to Loki's original snapshot rather than an LLVM release tag.

## Run Docker container

After pulling or building an image, run:

```bash
./docker_run.sh
```

Run the same command again to attach to the existing container. Stop and remove the container with:

```bash
./docker_stop.sh
```

## Setup inside Docker

Inside the container:

```bash
./setup.sh
```

The script builds Loki, installs LokiAttack Python dependencies, installs Triton when missing, and installs Intel Pin for Experiment 2. It uses `PARALLEL_JOBS` from the environment.

# Loki virtualization driver

This branch adds a reusable shared-library virtualization driver:

```bash
loki/translator/loki-virtualize.sh \
  --source sample.cpp \
  --output libsample_loki.so \
  --no-annotation \
  --exported-functions
```

For LLVM bitcode input:

```bash
loki/translator/loki-virtualize.sh \
  --input sample.bc \
  --output libsample_loki.so \
  --function target_function
```

Target selection options:

```text
--annotation NAME       virtualize annotated functions; default: loki_virtualize
--function NAME         virtualize a named function; repeatable
--exported-functions    virtualize defined externally visible functions except main
--all-functions         virtualize every defined non-intrinsic function
```

Supported operations currently include scalar integer and pointer-compatible operations up to 64 bits:

```text
add, sub, xor, and, or, mul
shl, lshr, ashr
udiv, sdiv, urem, srem
integer icmp
integer/pointer select
```

The driver uses a generated Loki uop dispatcher:

```text
loki/translator/generate_uop_dispatch.py
loki/translator/src/uop_dispatch.map
```

`uop_dispatch.map` contains stable randomized opcodes and XOR encoding keys. Keep it when repeated protections should use the same uop ABI. Regenerate it when you want a new protected ABI:

```bash
loki/translator/loki-virtualize.sh \
  --source sample.cpp \
  --output libsample_loki.so \
  --regenerate-uop-map \
  --uop-map-seed project-seed
```

## Expression batching

`virtualize-uops` batches dependent straight-line expression trees inside basic blocks. The default cap is 12 supported instructions per batch, matching the paper's superoperator depth range.

Relevant options:

```text
--no-batch              disable target-specific expression batching
--max-batch-nodes N     maximum supported instructions per batch; default: 12
```

The batching flow is:

1. Transform selected target functions to encoded Loki VM calls.
2. Emit target-specific batch semantics into `uop_batches.cpp.inc`.
3. Generate a target-specific `uop_dispatch.cpp` containing the base uops and batch semantics.
4. Obfuscate that dispatcher with Loki.
5. Link the transformed target bitcode with the generated Loki VM bitcode.

CFG is preserved by native stitching. Branches, loops, switches, PHIs, memory operations, and external calls remain native. Straight-line arithmetic/predicate expressions inside basic blocks execute through Loki-generated VM code.

`--inline-vm` exists as an experimental option. The default path links VM bitcode directly because `llvm-link + always-inline` is not stable for all generated Loki VM instances.

Loki symbols remain visible by default so release packaging can strip them separately. Use `--hide-loki-symbols` if you want the driver to localize Loki helper symbols during linking.

# vmsamples validation

Build and test the Linux sample suite:

```bash
cd vmsamples
./test_loki_vm.sh
```

The script builds:

```text
vmsamples/build/libvm_test_suite.so
vmsamples/build/libvm_test_suite_loki.so
vmsamples/build/check_vm_test_suite
```

The latest validated Loki path produces:

```text
formed 27 batched VM entries; 94 total VM entries
verified 94 direct Loki VM entries across 23 functions
summary: 892 passed, 0 failed, 0 skipped
```

The main virtualized shared library is:

```text
vmsamples/build/libvm_test_suite_loki.so
```

Generated artifacts under `vmsamples/build/` are ignored by git.

# Original Loki workflow

For the original prototype workflow, use [loki/obfuscate.py](./loki/obfuscate.py):

```bash
cd loki
python3 obfuscate.py --allow se_analysis /tmp/loki-eval
```

The original pipeline assumes a selected `extern "C" target_function` and has limited control-flow support. The branch-local `loki-virtualize.sh` path adds native-CFG-compatible shared-library virtualization for broader C/C++ targets.

# Manual dependency installation

Inside the container, the manual equivalent of `setup.sh` is:

```bash
cd loki
./build.sh

cd ../lokiattack
python3 -m pip install --user -r requirements.txt
./install_triton.sh

cd ../experiments/experiment_02_coverage/tracer
./install_pin.sh
```

# Contact

For more information about the original Loki project, contact [m_u00d8](https://github.com/mu00d8) ([@m_u00d8](https://twitter.com/mu00d8)) or [mrphrazer](https://github.com/mrphrazer) ([@mr_phrazer](https://twitter.com/mr_phrazer)).

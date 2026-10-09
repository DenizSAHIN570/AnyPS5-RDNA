# Native RDNA4 experiments

Tools for the two proposals of this fork: a native gfx12 (RDNA4) GPU path ([#1](https://github.com/DenizSAHIN570/AnyPS5-RDNA/issues/1)) and guest memory placed in CPU-visible VRAM ([#2](https://github.com/DenizSAHIN570/AnyPS5-RDNA/issues/2)). The target is a Radeon RX 9070 XT (gfx1201) on Linux with RADV. Nothing here changes how a title runs unless one of the environment switches below is set.

What is done and what to do next: [NATIVE_RDNA_NEXT.md](NATIVE_RDNA_NEXT.md).

## Environment switches

| Switch | Library | Effect |
|---|---|---|
| `APS5_DUMP_REGISTERED_SHADERS=<dir>` | libSceAgcDriver | Writes every shader the title registers to `<dir>/shader_<address>_t<type>_<hash>.ash`, before the recompiler prepares it, so shaders the recompiler rejects are kept too. |
| `APS5_MEMORY_CENSUS=<file>` | libkernel | Writes one CSV row per direct memory allocation, release, direct or flexible mapping, protection change, type change and unmap: `seconds,op,result,requested,address,length,physical,memory_type,prot,flags`. `memory_type` is `-2` when it is unknown for the row's operation. |

A switch that is set but cannot be honoured (empty value, directory or file that cannot be created or written) throws.

### Shader dump format (`.ash`, version 1)

Little-endian. A 48-byte header: magic `0x44485341` (`ASHD`), version, shader type, header byte count, code address (u64), header address (u64), code word count, then the shader, context and user config register counts. Then the code words, the shader, context and user config registers as (offset, value) u32 pairs, and the raw shader header bytes. The writer and the reader share [`ShaderDumpFormat.hpp`](../../core/libs/prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderDumpFormat.hpp).

## Tools

| Path | Purpose | Needs |
|---|---|---|
| [`tools/native-rdna/dev/Dockerfile`](../../tools/native-rdna/dev/Dockerfile) | Ubuntu 24.04 with GCC, CMake, Ninja, LLVM 20 (AMDGPU), libdrm, Vulkan and lavapipe, HSA headers | Docker |
| [`tools/native-rdna/dev/check.sh`](../../tools/native-rdna/dev/check.sh) | Builds and tests every tool below | the dev image or an equivalent host |
| [`tools/native-rdna/host_check.py`](../../tools/native-rdna/host_check.py) | Kernel, GPU, GC version, Resizable BAR (`mem_info_vis_vram_total` against `mem_info_vram_total`), render node, udmabuf, ttm, kfd, HSA, LLVM gfx1201 support; with `--vram-bench`, the Vulkan heap and `VK_EXT_map_memory_placed` | Linux |
| [`tools/native-rdna/baseline.py`](../../tools/native-rdna/baseline.py) | `build` (frame timing on), `prepare` (relink, copy `libs`, link `app0` resources), `run` (`APS5_PROFILE_DRAW=1`, optional shader dump and memory census, logs and `metadata.json`), `summarize` (fps, 1% low, per-field statistics of `frame-timing.log`) | Linux, a title |
| [`tools/native-rdna/census`](../../tools/native-rdna/census) | `shader_census`: decodes `.ash` dumps with upstream's RDNA decoder and counts opcodes, encodings, SDWA/DPP, literals, export targets, PC and wait instructions | C++20 |
| [`tools/native-rdna/census_report.py`](../../tools/native-rdna/census_report.py) | Joins a census with the opcode table: share of instructions by gfx12 status, the opcodes with no direct gfx12 form and their replacements | Python 3 |
| [`tools/native-rdna/opcode_map.py`](../../tools/native-rdna/opcode_map.py) | Generates [`data/gfx1013-to-gfx12.csv`](../../tools/native-rdna/data/gfx1013-to-gfx12.csv) from AMD's machine-readable ISA (RDNA1, RDNA2, RDNA4 XML) and LLVM's AMDGPU tablegen | the ISA zip, an LLVM checkout |
| [`tools/native-rdna/vram-bench`](../../tools/native-rdna/vram-bench) | `vram_bench info`, `placed` (CPU write/read/latency and GPU read bandwidth of placed VRAM against system memory and imports), `access` (atomics, `read()`, `write()`, `O_DIRECT`, futexes, `process_vm_readv`, `mprotect` on the mapping), `submits` (submit cost with ordinary allocations against imports) | Linux, a Vulkan driver (the loader is opened at run time) |
| [`tools/native-rdna/drm-dispatch`](../../tools/native-rdna/drm-dispatch) | `drm_dispatch`: a gfx1201 compute kernel submitted as raw PM4 through the amdgpu kernel interface (DRM ioctls, no libdrm) on the compute or gfx ring, with its buffers at fixed GPU addresses (`--va-base`, default 0x200000000) and every output value checked | Linux, gfx12 |
| [`tools/hw-oracle`](../../tools/hw-oracle) | Now selects `template_gfx12.s` for gfx12 targets; `--assemble-only` assembles without a GPU (`HW_ORACLE_TARGET`) | see [HW_ORACLE.md](HW_ORACLE.md) |

`--unplaced` runs `vram_bench placed` and `access` without `VK_EXT_map_memory_placed`, which lavapipe lacks, so their code paths can be exercised without an AMD GPU. The numbers are only meaningful with placed VRAM on the RX 9070 XT.

### Opcode table

`data/gfx1013-to-gfx12.csv` has one row per instruction of [`tools/rdna_isa.txt`](../../tools/rdna_isa.txt), with the source encoding and opcode (RDNA1, or RDNA2 for instructions RDNA1 lacks), the gfx12 name, encoding and opcode, and a status:

| Status | Meaning |
|---|---|
| `identical` | same name, encoding and opcode |
| `renumbered` | same name, other opcode or encoding |
| `renamed` | gfx12 name from the RDNA4 XML's `AliasedInstructionNames` (`renameSource` says where the name came from) |
| `removed` | no gfx12 form; `replacement` describes the expansion |
| `unresolved` | LLVM still encodes it for gfx12, the RDNA4 XML has no such instruction (`S_WAITCNT`) |

Regenerate it after a change to `tools/rdna_isa.txt`:

```sh
python3 tools/native-rdna/opcode_map.py --isa-xml <unzipped ISA dir> --llvm-amdgpu <llvm-project>/llvm/lib/Target/AMDGPU --csv tools/native-rdna/data/gfx1013-to-gfx12.csv
```

The ISA zip is https://gpuopen.com/download/machine-readable-isa/latest/; the table was generated from `AMD_GPU_MR_ISA_XML_2026_08_06.zip` (SHA-256 `82404f1126761b7877595b622afa7e1f311f2f41e89a3abe9aaf8ad045c082e2`). The XML files are MIT-licensed; the sidecar `gfx1013-to-gfx12.sources.json` records their hashes and the attribution.

## On the RX 9070 XT host

```sh
docker build -t anyps5-rdna-dev tools/native-rdna/dev
docker run --rm -v "$PWD:/src" anyps5-rdna-dev bash /src/tools/native-rdna/dev/check.sh
cmake -S tools/native-rdna/vram-bench -B build-vram-bench -G Ninja && cmake --build build-vram-bench
python3 tools/native-rdna/host_check.py --vram-bench build-vram-bench/vram_bench --json host.json
build-vram-bench/vram_bench all --json vram.json
python3 tools/native-rdna/baseline.py build
python3 tools/native-rdna/baseline.py prepare --relinker build/core/relinker/relinker --input <title>/eboot.elf --app0 <title> --out runs/dreaming-sarah
python3 tools/native-rdna/baseline.py run --out runs/dreaming-sarah --seconds 180 --label m0 --dump-shaders --memory-census --host-check host.json
cmake -S tools/native-rdna/census -B build-census -G Ninja && cmake --build build-census
build-census/shader_census -o census.json runs/dreaming-sarah/runs/m0/shaders
python3 tools/native-rdna/census_report.py census.json --markdown census.md
cmake -S tools/native-rdna/drm-dispatch -B build-drm -G Ninja && cmake --build build-drm && build-drm/drm_dispatch
```

`drm_dispatch` submits raw PM4. A wrong packet can hang the GPU until the kernel resets it; run it with no unsaved work open.

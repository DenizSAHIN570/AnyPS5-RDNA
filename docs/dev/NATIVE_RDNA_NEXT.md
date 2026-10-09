# Native RDNA4: what is done and what is next

Status of [#1](https://github.com/DenizSAHIN570/AnyPS5-RDNA/issues/1) (native gfx12 path) and [#2](https://github.com/DenizSAHIN570/AnyPS5-RDNA/issues/2) (VRAM placement). Commands are in [NATIVE_RDNA.md](NATIVE_RDNA.md).

## Done
- Source research for both issues, posted as issue comments: ISA changes, descriptors, tiling, compression, PM4, registers, RADV placed maps.
- Every tool in [NATIVE_RDNA.md](NATIVE_RDNA.md). They build and pass their tests in the dev container, and none of them has run on the RX 9070 XT yet.

## Next, on the RX 9070 XT (in this order)
1. **Set up the host.** Pull `groundwork`, run `git submodule update --init --recursive`, build the dev image and run `dev/check.sh`.
2. **Check the host.** Run `host_check.py --vram-bench ...` and post the output to #2 (#2 M0). Resizable BAR must be "full" and `VK_EXT_map_memory_placed` present.
3. **Run `vram_bench all`.** Post the results to #2 (#2 M2). The `access` lines decide what VRAM placement must exclude: atomics, `read()`, O_DIRECT, futexes.
4. **Run `drm_dispatch` four ways:**
   - on the compute ring;
   - on the gfx ring;
   - with `--vram-destination`;
   - with a high `--va-base`.

   Post the results to #1 (#1 M4, first goal). A failure here means the gfx12 preamble or register values need fixing before anything else.
5. **Run the hardware oracle on gfx1201.** It needs the HSA runtime and `/dev/kfd`. Start with a small body such as `v_add_f32 v10, v4, v5`, with `--ieee 0 --dx10-clamp 0`.
6. **Record the M0 baseline with Dreaming Sarah.** Run `baseline.py build`, `prepare`, then `run --dump-shaders --memory-census`. Post `summary.json` to both issues.
7. **Census.** Run `shader_census` on the dumped shaders, then `census_report.py`. Post the report to #1 (#1 M1): it sets the exact translator scope for the title. Look at the memory census for #2 Step 1.

## After that (not started)
- **#2 Step 3: placement.** Implement it once steps 3 and 7 show which memory types are safe in VRAM. One Vulkan allocation per guest mapping is needed, because RADV has no range placement.
- **#1 Step 2: translator library.**
  - a gfx12 encoder, driven by `data/gfx1013-to-gfx12.csv`;
  - an m0/null register remap;
  - SDWA and VINTRP expansion;
  - waits and hazards regenerated from data flow;
  - `s_getpc` replaced with a constant.

  Start with the opcodes the census lists for Dreaming Sarah.
- **#1 Step 2 tests.** These tools are not built yet:
  - an HSA harness that runs upstream's `tests/execution` programs through the translator on gfx1201;
  - a generator for the gfx10 → gfx12 register remap table from Mesa's `registers/gfx10.json` and `gfx12.json`.
- **#1 Step 3: descriptors and surfaces.** Write the V#/T#/S# conversion and the "guest surface at A, gfx12 shadow at A + offset" design in the issue before any graphics work.
- **#1 Steps 4–6:** grow `drm_dispatch` into the backend: graphics state, attribute ring, CLEAR_STATE stack, labels, presentation. Then one Dreaming Sarah draw, then the whole title.

## Gaps in what was built
- The memory census logs mappings but does not yet measure how often the CPU reads GPU-oriented memory (the `PROT_NONE` fault-sampling run in #2 Step 1).
- `drm_dispatch` waits on the kernel fence only and writes no RELEASE_MEM label.
- Nothing here targets Windows.

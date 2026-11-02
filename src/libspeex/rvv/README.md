# RVV 内核开发约定(src/libspeex/rvv/)

这个目录存放 RVV(RISC-V Vector Extension)优化的内核实现。所有内核用
C intrinsics(`<riscv_vector.h>`,RVV 1.0)编写,编译期通过宏选择,与
上游已有的 per-arch 机制保持一致。

## 文件与接入方式

上游 libspeex 的做法是每个计算模块带一个 per-arch 覆盖头
(`filters_sse.h`、`filters_bfin.h`、`ltp_arm4.h` …),由模块 `.c` 里的
`#ifdef` 链选择。RVV 内核平行地放在本目录,命名 `<module>_rvv.h`:

```c
/* filters.c 内,仿照 _USE_SSE 分支 */
#ifdef _USE_SSE
#include "filters_sse.h"
#elif defined(ARM4_ASM) || defined(ARM5E_ASM)
#include "filters_arm4.h"
#elif defined(BFIN_ASM)
#include "filters_bfin.h"
#elif defined(_USE_RVV)
#include "rvv/filters_rvv.h"     /* 新增 */
#endif
```

`_USE_RVV` 由根 CMakeLists 的 `USE_RVV` 选项写入 config.h,并自动追加
`-march=$(RVV_MARCH)`。

## 覆盖宏(内核 ↔ 上游钩子)

上游通过 `OVERRIDE_*` 宏把 C 实现替换为 arch 版本,RVV 内核同样定义它们:

| 内核                       | 位置              | 覆盖宏 |
|----------------------------|-------------------|--------|
| 码本距离 / VQ              | vq.c              | `OVERRIDE_VQ_NBEST` `OVERRIDE_VQ_NBEST_SIGN` ✅ |
| 内积(分块移位语义)       | ltp.c inner_prod  | `OVERRIDE_INNER_PROD` ✅ |
| 基音相关(4-lag 批量)     | ltp.c pitch_xcorr | `OVERRIDE_PITCH_XCORR` ✅ |
| 码本响应/能量              | cb_search.c       | `OVERRIDE_COMPUTE_WEIGHTED_CODEBOOK` ✅ |
| 目标更新(逐元素)         | cb_search.c       | `OVERRIDE_TARGET_UPDATE` ✅ |
| 开环基音增益搜索           | ltp.c             | `OVERRIDE_OPEN_LOOP_NBEST_PITCH`(经 inner_prod/pitch_xcorr 间接加速)|
| 3-tap 基音增益搜索         | ltp.c             | `OVERRIDE_PITCH_GAIN_SEARCH_3TAP_VQ`(经 inner_prod 间接加速)|
| iir/fir 滤波               | filters.c         | `OVERRIDE_FILTER_MEM16` `OVERRIDE_FIR_MEM16` `OVERRIDE_COMPUTE_IMPULSE_RESPONSE`(顺序反馈环,难点)|
| FFT                        | kiss_fft/kiss_fftr(经 fftwrap.c 调用)| — |

## 硬性规则

1. **Bit-exact**:`test/run_ab.sh`(native/qemu)与 `scripts/device-test.sh ab`
   (真机)必须 ALL PASS。定点路径的饱和、舍入、移位语义(arch.h 的
   SATURATE16/SHR16/MAC16_16 等)必须逐步复现,不能用 RVV 的饱和指令
   直接替换定点宏(舍入方向可能不同)。
2. **长度无关(VLEN-agnostic)**:一律 `vsetvl` 动态取 VL 且**必须放在
   使用 vl 的语句之前**;禁止对 VLEN 做任何假设;不得用
   `-mrvv-vector-bits=zvl*` 之类的定长编译选项产码。
3. **先测 C 参考**:每个内核落地前,先用未改动的 C 路径在目标机上拿到
   bench_e2e 基线,再逐个替换、逐个对比。
4. 头文件顶部 `#include "rvv/rvv_arch.h"`,并在 `#ifdef __riscv` 保护下
   使用 intrinsics。
5. **内核形状**:逐 entry"点积+redsum"只能打平 gcc auto-vec;要赢就得
   entry 并行(一 lane 一 entry,broadcast 标量乘累加,免 redsum)。
   每 entry 的 O(1) 修正(E 修正、符号翻转)放标量侧,只向量化数据并行
   主体——同时绕开 gcc-13 mask/merge intrinsics 的语义坑。

## 状态(X60 真机,详见 docs/perf-log.md)

| 内核 | 状态 | 真机 bit-exact | 收益(wb q8 e2e) |
|------|------|----------------|------------------|
| vq_nbest/_sign(entry 并行) | ✅ | PASS | 与 cwcb 合计显著 |
| inner_prod(分块移位)      | ✅ | PASS | 与 pitch_xcorr 合计 |
| pitch_xcorr(4-lag 批量)   | ✅ | PASS | ~+1% |
| compute_weighted_codebook  | ✅ | PASS | wb q8 单轮 -22.6% |
| target_update              | ✅ | PASS | ~+1% |
| filters / kiss_fft         | ⬜ | —    | 待做 |

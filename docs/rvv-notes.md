# RVV 1.0 编码要点(本项目语境)

## 工具链与编译选项

- 工具链:gcc ≥ 12(或 clang ≥ 15)的 riscv64 目标,`-march=rv64gcv`。
  定长优化(实验性、牺牲可移植性)才考虑 `zvl*` / `-mrvv-vector-bits`,
  本项目默认禁用——内核必须 VLEN 无关。
- QEMU 用户态可以跑 `qemu-riscv64 -L <sysroot>`,行为正确但性能数据
  没有参考意义;真实加速比只认开发板(RVV 1.0 硬件)。
- T-Head C910/C920 系(如部分国产开发板)是 **RVV 0.7.1 草案版**,
  intrinsics 面不同(`<riscv_vector.h>` 的 v0.7 变体或内置函数),
  适配层集中在 `src/libspeex/rvv/rvv_arch.h`,内核代码不感知差异。

## 定点语义是第一位

libspeex 定点路径的每一步都走 `arch.h` 的宏(Q12/Q14/Q15 移位、
`SATURATE16`、`MAC16_16`、`SHR32`…),含具体的舍入与饱和规则。
RVV intrinsics 里很多"等价"指令(vsmul、vnclip、vssra 的舍入模式、
饱和行为)与这些宏**并不逐位一致**。规则:

1. 内核先用 C 写出逐步语义,再映射为向量操作;拿不准的舍入就手动实现
   (移位 + 取整 + `vmin/vmax` 饱和)。
2. 每个内核落地后立刻跑 `test/run_ab.sh`;它对 nb/wb/uwb × 多质量档做
   码流逐字节比对,任何位级漂移都会暴露。
3. 定点溢出属于未定义行为,auto-vectorization(即使不开 _USE_RVV,
   `-march=rv64gcv` 也会触发)理论上可改变 UB 表现——因此 A/B 双方
   必须使用**完全相同的编译选项**构建,run_ab.sh 已保证这一点。

## 内核写法约定

- 每个内核一个 `<module>_rvv.h`,放在 `src/libspeex/rvv/`,由模块 `.c`
  的 `#elif defined(_USE_RVV)` 分支引入;替换函数用上游的 `OVERRIDE_*`
  宏声明(见 rvv/README.md 的表)。
- 向量类型统一用 `vint16m*_t`/`vint32m*_t` 等**带类型**别名,LMUL 选择
  写明理由(定点累加常用 m2 起步,避免 16×16→32 展开时寄存器不足)。
- 循环一律:
  ```c
  for (size_t vl; n > 0; n -= vl, p += vl) {
      vl = __riscv_vsetvl_e16m1(n);
      ...
  }
  ```
- 累加顺序改变会改变结果:点积类内核若用多 lane 累加,必须最后按与
  标量版一致的顺序归约——这正是 bit-exact 最常翻车的地方。

## 参考资料

仓库 `docs/ref/` 目录已收集核心资料:

- `docs/ref/riscv-v-spec-v1.0.pdf|.html` — RVV 1.0 ISA 规范( ratified)
- `docs/ref/rvv-intrinsic-spec.html`、`docs/ref/rvv-intrinsic-examples.html` — intrinsics 规范与示例
- `docs/ref/gcc-riscv-options.html` — gcc 的 RISC-V/-march 选项
- `docs/ref/xuantie-性能优化实战-rvv向量化.html`、`docs/ref/riscv-xuantie-vectorization-libraries.html` — T-Head(0.7.1 草案版)向量化实战
- `docs/ref/rvv-autovectorization-performance-pdp25.pdf`、`docs/ref/llvm-riscv-summit-rvv-autovectorizer.pdf` — 自动向量化能力与局限
- `docs/ref/sifive-xnnpack-rvv-blog.html` — SiFive 用 RVV 优化 xnnpack 的工程经验(内核替换 + bit-exact 议题同类)
- `docs/ref/backporting-riscv-vector-assembly-arxiv.pdf` — RVV 汇编移植研究

上游 per-arch 实现也可参考:baseline/libspeex/filters_sse.h、ltp_bfin.h
等——它们展示了"如何替换 C 参考"(注意 SSE 是浮点路径,定点语义请
对齐 C 代码而不是 SSE 代码)。

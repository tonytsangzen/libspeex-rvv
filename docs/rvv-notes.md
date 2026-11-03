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

## gcc-13 intrinsic 补充(第 14 轮)

- **vmerge_vvm(op1, op2, m) = m ? op2 : op1**:首参是 FALSE 分支,
  与规范 `vd = v0 ? vs1 : vs2` 相反——gcc 按汇编操作数序
  (vmerge.vvm vd, vs2, vs1, v0)排参。用已知值的双分支单元测试验证。
- **掩码类型 = vbool(SEW/LMUL)**:i32m2 → vbool16_t、i16m2 →
  vbool8_t、i32m1 → vbool32_t。不是"每元素 1 bit 的 vboolSEW"。
- 比较内建必须带 vl:vmslt_vv_i32m2_b16(a, b, vl) 三个参数缺一不可。
- VARDECL 与指针别名:`VARDECL(spx_word32_t *energy)` 会把 `*` 重复
  展开成二级指针;指向另一数组别名时须裸声明 `spx_word32_t *energy`。
- vredmax/vredmin 的 (vs2 向量, vs1 载体) 顺序与 vmerge 相反,是
  (vector, carrier),载体的 C 类型是 LMUL1(m1)。
- 内核级二分守卫:rvv/ltp_rvv.h、rvv/filters_rvv.h、rvv/vq_rvv.h、
  ltp.c 的第 14 轮内核均有 R14_NO_*(OPENLOOP/NORM16/VQTAIL/GSHELP)
  编译开关,`SPEEX_OPT_FLAGS="-DR14_NO_X"` 可单独禁用任一内核做
  真机 A/B 二分。守卫必须同时包住 OVERRIDE define 与函数体。

## 验证纪律(第 14 轮事故驱动)

- 每个新内核配**直接差分测试**(test/*_diff.c,qemu + 真机双跑),
  e2e A/B 不能替代:第 14 轮的基址 bug 只被 gshelp_diff 抓到。
- 真机基准一律 **SPEEX_PROF=OFF**:-finstrument-functions 对调用
  密集路径虚高 40-70%,prof-on 的 A/B 比值失真。
- ab 前**必须确认 build 成功**(device-test.sh 的 docker 退出码会被
  管道掩盖;build 失败后 ab 会部署陈旧二进制并给出误导性 FAIL)。

## 参照与算术契约(审查 R11/R12,2026-11-03)

**baseline 出处**:baseline/ 与 Xiph 官方 1.2.1 发布包
(speex-1.2.1.tar.gz, sha256 4b44d4f2…28feea)**不是字面等同**:
nb_celp.c 带宽扩展用 QCONST16(发布包为浮点常量)、sb_celp.c 的
filter_ratio 有 EPSILON 下限、质量范围/初始化存在修正(独立审查比对)。
baseline 按"当前行为参照"冻结——它是 A/B 的语义基准;是否另建"官方
发布版参照"属项目决策,不得用发布包直接覆盖 baseline。

**定点算术契约**:fixed_generic.h 的 ADD32/SUB32/MAC16_16 依赖有符号
int32 的二补码回绕(C 标准中为 UB;上游 libspeex 同样依赖)。RVV 内核
的位精确论证以此为前提。要点:
- inner_prod 语义 = 每 4 项的 int32 和**先回绕、再算术右移 6、再累加**,
  不是宽积分后统一移位;
- QMF 系数"取负再窄化"的边界(含 -32768)是参考行为,不得按"更精确"
  改写;
- 左移与 INT32_MIN 取负需逐项按选定参考定义。
随机差分 PASS 不构成对 UB 的数学证明;两侧编译器选项若引入不同
回绕/移位行为,位精确前提即失效(候选:-fwrapv 双侧固定,未启用)。

**RVV 尾部策略纪律(审查 R1/R2/R3 后新增)**:短 VL 写操作的目的地
尾部是 agnostic(indeterminate),跨操作读取属源码正确性问题——
- 持久向量累加器的更新一律 `_tu`(vadd/vmerge);
- 状态向量(滤波器 mem、候选值/索引)的短 VL 写入同上;
- 归约/reduction 只在活跃 VL 上进行,或保证源全宽有定义;
- 验收门禁:qemu `-cpu rv64,v=true,vlen={128,256,512},vext_spec=v1.0`
  × {normal, ta_all_1s, ma_all_1s, ta+ma} 四策略矩阵,全差分 + e2e
  (build/review/run-tail-matrix.sh)。

## CPU 能力修正表(probe v3,2026-11-03;取代第 15 轮 v2 数字)

probe v2 的三处缺陷:部分累加流被死代码消除、"unit load 被提升出
计时环"、rdtime 计时形态在 X60 上引发病态代码生成。v3 全消费 +
clock_gettime 计时后:

| 项目 | v3 实测 | 对 v2 结论的修正 |
|------|---------|------------------|
| vmacc 延迟 | 3.1 ns/vec-op(≈5 cyc @1.6GHz) | 维持 |
| vmacc 吞吐(e32m2 16 lane) | 2.4-3.0 ns/vec-op ≈ **0.33-0.42 vec-op/cyc**;"2 流已近峰值"(2/4/8 流 3.2/2.9/3.0ns) | "≥8 流达 1 vec-op/cyc、14-16 lane-MAC/cyc"撤回 |
| vwmacc e16m2(32 lane)+load | 5.2-5.3 ns/vec-op | — |
| vlse16(2KB span) vs vle16(64B span) | 21.3 vs 5.7 ns = **3.7×** | "8×"撤回 |
| RVV memcpy(8MB,全消费) | ≈5.0 GB/s(libc 4.4) | v2 5.3GB/s 为死代码消除伪影 |
| 向量/标量(全消费) | ≈**32×** | "40×"撤回 |

工程含义:respT 转置布局的收益依据从"跨步 8×"修正为"跨步 3.7×",
实测内核收益 2~3× 与之自洽;累加链分链的收益上限低于 v2 推断,
第 15 轮实测(e2e -1~3%)与之自洽。

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

## 工具链升级记录:gcc-14(第 24 轮,2026-11-04)

- 镜像升级 gcc-13.3 → gcc-14.2(scripts/Dockerfile.buildenv):
  intrinsics API 从 v0.11(11000)升到 v1.0(12000),
  **段元组类型可用**:`vint16m1x4_t` + `__riscv_vlseg4e16_v_i16m1x4` +
  `__riscv_vget_v_i16m1x4_i16m1`(vget 索引必须是编译期常量)。
- 坑 1:Ubuntu 的 gcc-14 交叉包不装无后缀 `riscv64-linux-gnu-gcc`
  symlink(CMake 探测需要),Dockerfile 里手动 `ln -sf`。
- 坑 2:gcc-14 默认 `-std=c23`,隐式函数声明是硬错误——C99 风格
  老测试代码必须补 `<stdio.h>` 等标准头(bench_e2e 中招)。
- 坑 3:vmerge 操作数序复验:gcc-14 与 gcc-13 一致(实测汇编
  m=true 取 op2,与规范 vs1-true 相反),既有调用点无需改动。
- 特性宏纪律:依赖新 API 的代码一律挂在 `rvv_arch.h` 的
  `SPEEX_RVV_SEGV4`(API 版本轴)下,内核其它部分保持双 API 兼容;
  gcc-13 回退路径 = 第 23 轮代码逐字等价,每次改动都要走一遍
  "关宏全树编译 + CTest" 的回退验证。
- vlseg4 的位精确依据:field k 与第 k 路 stride-8B vlse16 取到
  逐元素相同的值——只改加载指令形态,不改任何算术;短 VL 语义
  (尾部 agnostic)与 _tu 累加器纪律不受影响。
- qemu 相对信号(TCG,仅供方向参考):segdiff len=160,
  vlseg4 vs 4×vlse16 rdtime −24%(VLEN128)/−13%(VLEN256);
  绝对数字只认 X60 真机。

## 第 24 轮真机复盘:陈旧对象伪造性能信号(必读)

- **事故**:内核门控宏(SEG 双路径)翻转后用增量构建出真机基准
  二进制,配对测量显示编码 −2~−27% 全面回退;逐内核消融出现非单调
  矛盾(PX-only 反而最快 +8%),一度误判为"vlseg4 组合布局敏感"。
- **定位**:目标文件级 diff——ltp/cb_search/vq 已是新配置,但
  nb_celp.o 时间戳早于头文件修改,make 依赖跟踪漏更 → 混合新旧
  代码的弗兰肯二进制。同码对照实验(两份 truly-identical 配置互测
  = 0.0%)证明配对测量方法本身可靠;污染全在构建侧。
- **修复与纪律**:`device-test.sh do_build` 先 `rm -rf` 构建目录;
  改任何影响预处理器路径的宏后,不允许信任增量构建的测速结果;
  可疑信号先用 objdump 对比关键内核的反汇编(vlseg4/vlse16 计数)
  再下结论。
- **顺带修正的认知**:vlseg4e16 在 X60 上**对齐不敏感**(len=160
  微基准:对齐 168 vs 2 字节不对齐 166 ns/call),1.9~2.5× 于
  4×vlse16 稳定成立——第 22 轮 K3 的 1.9× 不是对齐红利。
- **gcc-14 代码生成中性**:同源码(全跨步)gcc-14 vs 第 23 轮
  gcc-13 等价物配对 = +0.0/+0.2%。本轮全部净收益来自段加载。

## 第 25 轮:LTO/PGO 与剖析门(2026-11-05)

- **LTO 不可用**:gcc-14.2 交叉工具链 `-flto` 在 RVV intrinsics 跨
  TU 内联(compute_rms16 → vmv_v_x_i32m2)触发 ICE
  (riscv-vector-builtins.cc generate_insn);`-flto-partition=none`
  同样。链接行需要带 `-march=rv64gcv` 才能过第一关(内建函数
  lowered),但 ICE 在其后。升级 gcc 后重试。
- **PGO 配方**(generate → 真机训练 → use,真机 A/B bit-exact
  ALL PASS):见 perf-log 第 25 轮。结果 enc +1.8~3.7% / dec 混杂,
  未采纳为默认——训练数据代表性与两段式构建的复杂度不抵边际收益。
- **剖析门教训**:SPEEX_PROF 定义在构建目录的 config.h 里,是 speex
  目标的私有包含——benchmark TU 看不见时 PROF_GATE 静默变空操作,
  dec 相归因被 warmup 编码污染 37-46%。这类"宏可见性"问题没有
  编译告警,只能靠泄漏率检查(剖析 dec 相时 enc 函数占比应为 0)。
  修法:benchmark/CMakeLists.txt 显式 target_compile_definitions。
- **剖析口径**:rdtime 在欠账状态下读数整体偏慢 ~1000×,但同一次
  运行内份额(百分比)仍有效——跨轮比较只看占比,不看绝对值。

## 第 26 轮:X100 目标机笔记(2026-11-12)

- **X100 ≠ X60 的关键差异**:同 VLEN=256 / RVV 1.0,但标量 ISA 富
  zba/zbb/zbc/zbs + zicond,向量富 zvbb/zvbc/zvkb/zvfh。MAC 型内核
  用不上 zvbb;zicond/zbb 让标量胶水代码受益(实测 enc +4.1~5.3%)。
- **16 硬件线程**:policy0(cpu0-7)@2.15GHz + policy8(cpu8-15)
  @1.8GHz——为 P2 批并行重估容量(8 大核 + 8 小核)。nproc 显示 8
  是 shell 亲和性,不是硬件上限。基准必须 taskset 锁大核。
- **microarch 调优**:gcc-14 `-mtune=sifive-p600-series` 对 X100
  解码一致性回退(−1.5~−4.2%)——SpacemiT 自研微架构与 SiFine P600
  调度模型不合,不要照搬厂商近似;只采纳语义无风险的 march 扩展。
- **gcc-14 mtune 名**:合法值是 `sifive-p400-series`/`sifive-p600-series`
  (不是 p400/p450/p670 裸名,那些只在 `-mcpu=` 列表)。
  `--param=riscv-autovec-*` 在 Ubuntu gcc-14 不存在。
- **cpu_probe 在 X100 复测**:4 独立流吞吐 ~0.9 vec-op/cyc(X60
  ~0.42),vlse16 仍 4.7× 慢于 vle16——形状优先方法论跨代成立;
  L1 翻倍到 64KB,16KB 内核工作集全驻留。

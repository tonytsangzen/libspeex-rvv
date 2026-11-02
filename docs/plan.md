# 移植与优化计划

目标:libspeex 1.2.1(定点)在 64 位 RISC-V 上正确运行,并用 RVV 1.0
逐内核加速,**码流与参考实现 bit-exact**。

## 里程碑

- **M0 骨架**(已完成):项目结构、构建体系、A/B 回归工具。
- **M1 目标机可运行**:交叉编译通过(docker 工具链或板端工具链),
  qemu-user / 开发板上跑通 `testenc` 三件套与 `run_ab.sh`。
- **M2 基线性能**:目标机上 `bench_e2e` 记录 nb/wb/uwb 各质量档的
  us/frame,作为所有优化的对照基线;用 perf/内核级计时确认热点排序。
- **M3 内核级 RVV 优化**:按下表逐个替换,每落一个内核跑一次
  `run_ab.sh`(必须 ALL PASS)+ `bench_e2e` 记录提升。
- **M4 收尾**:全模式全质量档回归,更新 `docs/progress.md`,整理数据。

## 热点内核优先级(按预期收益排序)

上游 OPTIMIZE 文件与社区经验一致:窄带编码时间大头在码本搜索与基音
搜索,宽带再叠加子带 FFT/滤波。定点模式下逐样本循环(DSP 宏风格)
占比极高,RVV 向量化空间大。

| 优先 | 内核 | 文件 | 钩子 / 覆盖宏 | 说明 |
|------|------|------|---------------|------|
| 1 | 内积/码本搜索 | cb_search.c, vq.c | `OVERRIDE_VQ_NBEST`, `OVERRIDE_VQ_NBEST_SIGN` | 大量定长点积,最典型的 RVV 收益点 |
| 2 | 开环基音相关 | ltp.c | `OVERRIDE_OPEN_LOOP_NBEST_PITCH` | 一串 160 点点积(约 80–360 的滞后范围) |
| 3 | FIR/感知加权滤波 | filters.c | `OVERRIDE_FILTER_MEM16`, `OVERRIDE_FIR_MEM16`, `OVERRIDE_COMPUTE_IMPULSE_RESPONSE` | 逐样本 IIR 反馈环需分段技巧,FIR 部分直接向量化 |
| 4 | LSP/Chebyshev 求值 | lsp.c, quant_lsp.c | `OVERRIDE_CHEB_POLY_EVA`, `OVERRIDE_LSP_QUANT`, `OVERRIDE_LSP_WEIGHT_QUANT` | 10 阶多项式求值与量化距离计算 |
| 5 | FFT | kiss_fft.c, kiss_fftr.c | 无 OVERRIDE 宏,改 fftwrap 调用的内部内核 | 宽带子带分解使用 |
| 6 | 窗口/逐元素运算 | window.c 等 | 直接改调用点 | 简单收益,顺手做 |

> 上表是假设清单,M2 的剖析数据说了算;每完成一个内核在
> `docs/progress.md` 打勾并记录数字。

## 约束与验收标准

1. **bit-exact 是硬门槛**:`test/run_ab.sh` 任何时刻必须 ALL PASS
   (码流逐字节一致 + 交叉解码一致)。
2. 优化只在 `src/` 内进行;`baseline/` 永远保持上游原样,作为参照构建。
3. 内核代码长度无关(不假设 VLEN),目标机为 RVV 1.0(如 C920、
   CV1800B 后续型号、QEMU rv64gcv)。
4. 性能指标以目标机 `bench_e2e` 为准;qemu 数据只作回归参考,
   不作为加速比结论。

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

## 第 15 轮方案:CPU 能力探测驱动的点积家族重构(2026-11-03)

### X60 能力探测(test/cpu_probe.c,真机实测)

| 项目 | 实测 |
|------|------|
| 拓扑 | 8× SpacemiT X60 @1.6GHz(performance governor),每核 L1I/L1D 32KB 4-way 64B 行,L2 512KB 16-way 每 4 核簇 ×2,8GB DDR |
| ISA | rv64imafdcv + zicbom/zicboz/zba/bb/bc/bs/zkt/zicntr/zicond;用户态仅 rdtime 可读(**rdcycle 非法指令**) |
| VLEN | 256 bit 确认(qemu 默认 128——差分测试天然覆盖不同 VL 尾路径) |
| 向量 MAC | 延迟 5 cyc/vec-op(e32m2 16 lane);吞吐上限 ≈1 vec-op/cyc(14~16 lane-MAC/cyc),需 ≥8 独立累加流;单链仅 1/5 利用率 |
| 跨步加载 | **vlse16(stride 64B)= 38.7 cyc/vec-op,比 vle16(5 cyc)慢 ~8×** |
| load+compute | 重叠有限(4 流 load+MAC ≈0.7 vec-load/cyc) |
| DDR 拷贝 | ~5.3 GB/s(3.3 B/cyc),稳态;首触页 15ms/16MB |
| 标量基线 | 4 链 mul-add ≈0.34 lane-MAC/cyc → 向量满利用率收益 ~40× |

### 假设验证(probe 内 A/B,位精确对照)

raw_dists 形状(e16m2 batch,len=20):
- A 现状(vlse16 + 单累加链):**732 cyc/batch**
- B 转置布局(vle16)+ 4 路分累加链:**213.6 cyc/batch = 3.4×**,两版
  累加结果逐位相同(mod-2^32 加法结合性)。

### 优化项(优先级)

| 项 | 内容 | 预期 |
|----|------|------|
| P0-A | compute_weighted_codebook 输出列主序 resp2T(写 1 次换 8×nb_subvect 次读),vq_nbest/_sign raw_dists 改 vle16 unit load | 点积内核 ~2-3×;编码 −5~10%,解码 −8~15%(解码 split_cb/pitch 子树占比更高) |
| P0-B | raw_dists/inner_prod 累加器 4 路分链(顺序交错,合并按 mod-2^32;bit-exact 已验证) | 与 A 叠加趋近 3.4× |
| P1-C | P0 落地后重剖解码(multicomb 占 nb 解码 ~30%、interp_pitch ~19%),决定是否还有 ≥5% 块 | 视数据 |
| P1-D | qemu 差分测试显式 `-cpu rv64,v=true,vlen=256`,与真机同 VL 尾路径 | 覆盖性 |
| P2-E | 8 核流级并行(批转码场景,库外 API) | ~线性 ×8 |
| P2-F | zicboz 大缓冲清零(exc 仅 40 样本,边际) | 暂缓 |

收敛判据沿用:P0 落地后重剖,无 ≥5% 可向量化块且单轮增益 <2% 即收敛。

### 第 19/20 轮执行结果(2026-11-03,审查 M2)

- K1 滤波器状态驻留寄存器:落地,enc -6.6~9.8% / dec -4.9~8.7%
  (远超保守预期;真实占比被插桩稀释),累计 enc 最高 -63.6% /
  dec 最高 -56.0%。
- K2 QMF 固定形态:落地,dec wb -25~28% / uwb -40~42%。
- K3 停止:vlseg4 无工具链支持;lane=lag 操作数 ×4 权衡反转。
- K5 消融:6 个 override 逐一验证均有正贡献(QMF 固定形态贡献
  WB 解码 31% / UWB 56%),无一回退;quant_lsp/lpc_to_lsp 低于
  投入阈值。
- 消融后无剩余 ≥3% 的可行目标,审查停止条件满足。
- 第 22 轮(M3 补充):gcc-14.2 全量对照通过(0 错误、CTest 8/8、
  毒化 84 组全零、e2e ALL PASS);vmerge 操作数序跨版本稳定。
  vlseg4e16 在 gcc-14 下解禁,qemu 相对计时 -23%,设备实测待补,
  列为工具链升级时的首选优化项。
- 第 23 轮(M2-K3 追加):gain_search 的 9 点积批合并
  (rvv_gain_corr9,加载 72→16/块 + 省 8 次调用);真机 A/B ALL
  PASS,同会话实测编码 nb -1.8~2.0% / wb·uwb -0.6~1.0%(与归因
  预期一致),累计 enc 最高 -64.1% / dec 最高 -56.2%。

### 第 15 轮执行结果(2026-11-03)

P0-A/P0-B 落地,真机 A/B ALL PASS。**实际收益低于预期**:编码仅
wb q8 -2.6% / uwb q8 -1.4%,解码持平。复盘三点(详见 perf-log 第 15 轮):
raw_dists 只占 vq_nbest ~43%(乘法错在全函数);探针形状(32 lane、
stride 64B)高估了旧内核成本(生产 16 lane、stride 10~40B),真实
加速 ~2-3×;解码不经过 vq——第 14 轮"解码 split_cb/pitch"归因是
剖析阶段污染(BENCH_PHASE=dec 仍含编码阶段)。

内核级:vq_nbest_sign -60%、vq_nbest -20%,剩余 ~82% 在已向量化的
argmin 尾部(继续压榨 <1.5%,不划算)。

**P1-C 修订**:解码内部成分剖析需要 bench_e2e + prof.c 增加**运行时
剖析门**(PROF_ENABLE 全局,编码阶段关门),这是下一步的前置;
之后据真实解码成分决定是否继续。P2-E(多线程)与 P2-F(zicboz)
维持原判。

## 第 22 轮方案:进一步优化(2026-11-04,配对基准完整矩阵后)

### 最新基准(第 21 轮配对基准,B/A,5 对交替配对,完整 6 档)

| 模式/质量 | 编码 B/A | 解码 B/A |
|------|------|------|
| wb q8 | 2.72× | 1.77× |
| uwb q8 | 2.57× | 2.21× |
| uwb q2 | 1.81× | 2.27× |
| wb q2 | 1.77× | 1.83× |
| nb q8 | 1.84× | 1.35× |
| nb q2 | 1.83× | 1.37× |

### 覆盖度反推(Amdahl,f=(1-1/S)/(1-1/s);s=热点 kernel 平均加速)

| 档位 | 编码 f(s=4) | 编码 f(s=8) | 解码 f(s=4) | 解码 f(s=8) |
|------|------|------|------|------|
| wb q8 | 0.84 | 0.72 | 0.58 | 0.50 |
| uwb q8 | 0.81 | 0.70 | 0.73 | 0.63 |
| uwb q2 | 0.60 | 0.50 | 0.75 | 0.64 |
| wb q2 | 0.58 | 0.50 | 0.60 | 0.52 |
| nb q8 | 0.61 | 0.52 | 0.35 | 0.30 |
| nb q2 | 0.60 | 0.52 | 0.36 | 0.31 |

**解读(以档位间相对排序为准,单点绝对值受 s 假设影响)**:
- 编码 q8 档(wb/uwb)f≈0.81-0.84,覆盖已近 Amdahl 上限;q2 档
  f≈0.58-0.60 明显偏低——q2 低复杂度路径(hexc 子模式)覆盖不足,
  是编码端唯一剩余洼地。
- 解码 nb f≈0.35,全域最低;wb/uwb 解码 f≈0.58-0.75 中等。
- 已论证无解的路径(lsp 根查找、kiss_fft)不再重复投入。

### 优化项(优先级)

| 优先 | 项 | 内容 | 预期 | 验收 |
|------|----|------|------|------|
| P0 | q2 档编码覆盖剖析 | 用 prof.c 运行时门(prof-off 口径)对 nb/wb/uwb q2 编码做真实成分剖析,定位 ≥3% 的未向量化块;候选:hexc 子模式逐样本循环、激励重建、perceptual 加权残部 | 编码 q2 各档 −3~8% | 差分为零 + 真机 A/B ALL PASS + 配对基准 q2 档提升 ≥3% |
| P1 | nb 解码残余确认 | prof-off + 运行时门重测 nb 解码真实成分;若 LSP 插值/增益解码等 ≥3% 则做跨系数 lane 并行(此前 K5 判低于阈值,以新数据复核) | nb 解码 −2~5% | 同上 |
| P2 | 库外帧级并行 API | 新增帧级并行入口(pthread,帧间无共享态),批转码场景多帧并行;库内单帧 bit-exact 不动 | 吞吐 ×4-8(8 核) | 逐帧与单线程输出逐字节一致 |
| P3 | 净环境重测基线 | 停 gdm3/拔显示器,同代码重测,产出无桌面污染的最终累计矩阵(跨天对比的前提) | 数据完整性 | 与第 19 轮累计矩阵同口径可比 |
| P4 | VLEN=512 设备 LMUL 重调 | 若获得 ≥512 设备,重跑配对基准并按设备调 LMUL(m2→m4 候选) | 视设备 | 真机 A/B ALL PASS |

### 收敛判据

P0/P1 落地后重剖:无 ≥3% 可行目标且单轮增益 <2% 即收敛(沿用
第 15 轮判据)。P2 为库外能力,不影响收敛判定;P3 为数据收尾;
P4 依赖设备到位。

# libspeex-rvv 最终测试报告

日期:2026-11-03 · 分支 main(63bdd4e 之后) · 优化循环:16 轮

## 1. 项目与验收口径

将 libspeex 1.2.1 定点编解码器移植到 RISC-V RVV 1.0 并逐内核优化。

- **硬性验收**:优化后的编码码流与原始 C 参考(baseline/,gcc 自动向
  量化)**逐字节 bit-exact**,含交叉解码(A 编 B 解 / B 编 A 解)。
- **目标机**:BPMIK102080640200 —— SpacemiT X60 ×8 @1.6GHz(performance
  governor),RVV 1.0 VLEN=256,L1I/L1D 32KB/核,L2 512KB×2 簇,8GB DDR,
  Bianbu Star 2.1(非 Android,adb 部署到 /tmp/speex-rvv)。
- **工具链**:gcc-13(riscv64-linux-gnu)静态交叉编译,`-march=rv64gcv`;
  qemu-user(VLEN=128)+ 真机(VLEN=256)双环境验证,天然覆盖不同
  VL 尾路径。
- **测速口径**:同会话 A(src)vs B(baseline)对照,`SPEEX_PROF=OFF`
  (剖析插桩会使调用密集路径虚高 40-70%,剖析与测速分离构建)。

## 2. 最终认证结果(本报告当日实测)

### 2.1 位精确门禁(真机)

| 项目 | 结果 |
|------|------|
| A/B 码流逐字节比对(nb/wb/uwb × q2/q8,含交叉解码) | **6/6 ALL PASS** |
| batch5_diff(5 点积批合并) | 0/27,000 失配 |
| gshelp_diff(gain_search 尾部) | 0/8,400 失配 |
| open_loop_diff(开环基音选择/能量递推) | 0/14,000 失配 |
| vqtail_diff(vq N=1 向量尾部,列主序) | 0/34,560 失配 |
| inner_prod_diff(分块移位点积) | 0/80,000 失配 |
| gainvq_diff(gain-VQ 二次型,全 int16 域) | 0/2,000 失配 |
| **差分合计** | **0 / 165,960 失配** |

### 2.2 端到端性能(同会话 A vs baseline,µs/帧)

| 模式/质量 | 编码基线 | 编码优化后 | 编码提升 | 解码基线 | 解码优化后 | 解码提升 |
|---|---|---|---|---|---|---|
| nb q2 | 334.0 | 202.4 | −39.4% | 52.8 | 41.4 | −21.6% |
| nb q8 | 476.1 | 280.7 | −41.1% | 54.3 | 42.7 | −21.3% |
| wb q2 | 449.5 | 279.0 | −37.9% | 109.7 | 64.8 | **−40.9%** |
| wb q8 | 1207.0 | 477.6 | **−60.4%** | 114.4 | 69.2 | **−39.6%** |
| uwb q2 | 640.9 | 389.7 | −39.2% | 213.3 | 102.4 | **−51.9%** |
| uwb q8 | 1397.7 | 591.0 | −57.7% | 218.6 | 107.4 | **−50.9%** |

(第 18 轮后的最终数字;第 2.1 节认证表为此轮同日实测。)

**实时率(X60 单核 1.6GHz)**:nb q8 编码 72×、解码 473×实时;
最重的 uwb q8 编码 34×、解码 114×实时。

### 2.3 逐轮演进(编码 wb q8 / 解码 nb,同会话或同环境口径)

| 轮 | 内容 | 阶段性累计(编码 wb q8 / 解码 nb) |
|----|------|-----------------------------------|
| 1 | vq entry-并行 + inner_prod 分块 | ≈0(教训:naive 点积仅平 autovec) |
| 2 | vq raw dists + 标量 argmin/符号修复 | ≈−22% / — |
| 3 | compute_weighted_codebook entry 并行 | ≈−30% / — |
| 4 | pitch_xcorr 4-lag 批 + target_update | ≈−33% / — |
| 5 | iir/fir/filter_mem16(转置 DF vrgather) | ≈−40% / −8~11% |
| 6 | interp_pitch | ≈−40% / −10~15% |
| 7 | 真机剖析(无内核) | — |
| 8 | multicomb 尾部 + compute_rms16 | ≈−40% / −14~19% |
| 9 | lsp_to_lpc 级联展开 | ≈−42% / −15~19% |
| 10 | qmf_decomp(+qmf_synth 回退) | ≈−47% / — |
| 11 | qmf_synth 调度驱动重写 | ≈−50% / −16~19% |
| 12 | 编译旗标矩阵(阴性:无免费午餐) | — |
| 13 | pitch_gain_search_3tap_vq | −51.3% / — |
| 14 | open_loop argmax + normalize16 + 增益尾部 + vq 向量尾部 | −58.8% / −19% |
| 15 | CPU 探测驱动:respT 转置 + 分链累加 | −60.5% / −17~19% |
| 16 | 剖析运行时门 + 点积批合并 | **−60.8% / −21.7%** |
| 17 | 审查响应:M0 正确性修复(_tu 尾部/哨兵种子/ABS16/guard)+ 毒化验收体系 | −60.8% / −21.7%(修复零代价) |
| 18 | 审查 M2-K2:QMF synth 固定形态替代调度解释器 | −60.4% / **−40~52%(wb/uwb)** |

## 3. 目标 CPU 能力实测(test/cpu_probe.c,probe v3,第 17 轮审查后修正)

| 项目 | 实测(probe v3) | 对内核设计的含义 |
|------|------|------------------|
| VLEN | 256 bit(e8m8 VLMAX=256) | 内核全部运行时 vsetvl,VLA 感知 |
| 向量 MAC | 延迟 ≈5 cyc(e32m2 16 lane);吞吐 ≈0.33-0.42 vec-op/cyc(≈5-7 lane-MAC/cyc),2 条独立流已近峰值 | 分链累加适度即可 |
| 跨步加载 vlse16(2KB span) | ≈21 ns/vec-op,比 vle16(≈5.7 ns)慢 **3.7×** | respT 列主序布局的依据 |
| load+compute 重叠 | ~0.7 vec-load/cyc | — |
| DDR 拷贝 | ≈5.0 GB/s(libc 4.4 GB/s) | 工作集均可驻留缓存 |
| 标量基线 | ≈0.1 lane-MAC/cyc → 向量收益 ≈32× | — |
| 用户态计数器 | 仅 rdtime(24MHz);rdcycle 非法 | 剖析器与时基校准 |

> v2 探针的"1 vec-op/cyc 需 ≥8 流 / 跨步 8× / 收益 40×"已被独立审查
> (R5:累加流死代码消除、rdt 计时形态病态)推翻并按 v3 修正;
> respT 转置的实测内核收益(2~3×)不受影响。

## 4. 工程资产

- **20 个 RVV override 点**:vq_nbest/_sign(+N=1 向量尾部)、inner_prod
  (+batch5)、pitch_xcorr、pitch_gain_search_3tap_vq、
  compute_weighted_codebook(+respT 双写)、target_update、iir/fir/
  filter_mem16、interp_pitch、multicomb 尾部、compute_rms16、lsp_to_lpc、
  qmf_decomp/qmf_synth(调度驱动)、open_loop_nbest_pitch、normalize16、
  gain_search 尾部助手。
- **7 个差分测试**(qemu+真机双跑)、A/B 门禁脚本、剖析器(带运行时门)、
  CPU 能力探针、内核级二分开关(R14_NO_*)。
- **完整文档**:docs/perf-log.md(16 轮含事故复盘)、plan.md、
  progress.md、rvv-notes.md(gcc-13 内建陷阱清单)、
  skills/rvv-optimization-loop(方法论与踩坑)。

## 5. 已知事故与修复(择要)

1. 多返回值调试脚手架 printf 风暴(第 13 轮,780× 减速)→ 调试代码
   即用即删纪律。
2. rvv_gain_build_exc 第二段基址重复偏移(第 14 轮)→ 专用差分测试
   在真机抓出。
3. gain-VQ `(int16_t)(-C16[k])` 在 −32768 负溢出(第 13 轮遗留,
   编解码域不可达)→ 全 int16 域差分测试抓出并修复。
4. 剖析阶段污染:BENCH_PHASE=dec 混入编码阶段(第 14 轮误读)→
   运行时剖析门根治。
5. gcc-13 工具链坑:vmerge_vvm 首参为 FALSE 分支、掩码类型
   vbool(SEW/LMUL)、VARDECL 指针别名展开、vredsum 参数序——均已
   记录于 docs/rvv-notes.md。

## 6. 剩余边界与后续路径

- 编码剩余:lsp 根查找(自适应顺序游走,bit-exact 向量化无解)、
  lsp 量化标量搜索(<3%)、调用开销。
- 解码剩余:全部为已优化内核残部(qmf_synth/multicomb/lsp_to_lpc/
  iir_mem16),单轮预期 <2%。
- 后续两条路(均需项目决策):
  1. **多线程批处理**:8 核流级并行(库外 API),批转码 ~×8;
  2. **净环境重测基线**:停板子桌面(gdm3)消除 DDR 争抢,绝对数字
     更好看但需中断桌面会话。
- 收敛判据已达成:连续三轮单轮 e2e 增益编码 ≤2.6%、解码 ≤4%,
  无剩余 ≥5% 可行目标。

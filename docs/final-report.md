# libspeex-rvv 最终测试报告(全流程)

日期:2026-11-04 · 分支 main · 任务周期 2026-11-02 ~ 11-04 ·
优化 19 轮 + 独立审查响应 4 轮(验证/归因/测量),共 21 轮

## 0. 任务全流程时间线

| 阶段(2026-11-02) | 内容 |
|------|------|
| 初始化 | 项目骨架:baseline/ 只读参照、src/ 工作副本、CMake(FIXED_POINT/_USE_RVV/RVV_KERNELS/SPEEX_PROF + 工具链文件)、speex_ab 确定性 A/B 工具、bench_e2e、presets |
| M1 | 交叉编译打通(gcc-13 rv64gcv,Docker buildenv 缓存镜像);qemu 全模式 A/B PASS;真机 BPMIK102080640200(SpacemiT X60)连通,adb 部署循环建立 |
| 第 1 轮 | 首个教训:naive 逐 entry 点积仅平 gcc 自动向量化 → 确立"形状优先"方法论(entry-并行、免归约) |
| 第 2-6 轮 | vq raw dists/argmin、compute_weighted_codebook、pitch_xcorr 4-lag 批、target_update、iir/fir/filter_mem16(转置 DF vrgather)、interp_pitch |
| 第 7 轮 | 真机剖析基建(-finstrument-functions + rdtime),确认后续优先级 |

| 阶段(2026-11-03) | 内容 |
|------|------|
| 第 8-11 轮 | multicomb 尾部 + compute_rms16、lsp_to_lpc 级联展开、qmf_decomp、qmf_synth(调度驱动版,后被第 18 轮取代) |
| 第 12 轮 | 编译旗标矩阵(阴性:无免费午餐,手写内核主导) |
| 第 13 轮 | pitch_gain_search_3tap_vq(增益码本 entry-并行) |
| 第 14 轮 | 编码尾部清扫:open_loop argmax、normalize16、增益尾部、vq 向量尾部;printf 风暴事故复盘 |
| 第 15 轮 | CPU 能力探针 v1 + respT 转置布局 + 分链累加 |
| 第 16 轮 | 剖析运行时门(prof gate);interp_pitch/multicomb 点积批合并 |
| 独立审查 | code-review-2026-11-03.md(对象 0f103b6):R1-R12 正确性/测试/归因/边界发现 + M0-M3 分期方案 |

| 阶段(2026-11-03 ~ 11-04,审查响应) | 内容 |
|------|------|
| 第 17 轮(M0/M1) | 尾部策略 `_tu` 纪律(R1/R2/R3)、哨兵等值种子(R6)、ABS16 INT16_MIN(R7)、自指 guard 的 target_update(R9)、构建矩阵(R8)、gshelp 测试修复(R4)、契约文档(R11/R12);毒化矩阵 72/72 |
| 第 18 轮(M2-K2) | QMF synth 固定形态替代调度解释器 → 解码 wb -25~28% / uwb -40~42% |
| 第 19 轮(M2-K1) | 滤波器状态驻留寄存器(vslide1down/vmv.x.s/VL=ord)→ 编码 -6.6~9.8% / 解码 -4.9~8.7% |
| 第 20 轮(M2-K3/K5) | K3 停止判定(vlseg4 工具链阻断、lane=lag 权衡反转);K5 逐内核消融归因(全部 override 正贡献) |
| 第 21 轮(M3) | 配对基准(benchp,中位数±0.3% 区间)、帧时延百分位(p50/p95/p99)、资源记录、rvv_arch 版本声明修正 |
| 第 22 轮 | CPU 能力探针 v3(MAC 延迟/吞吐、vlse16 3.7× 慢、rdtime 计时纪律);vlseg4 设备实验 1.9×(K3 采纳前提) |
| 第 23 轮 | gain_search 9 点积批合并 rvv_gain_corr9;真机 A/B ALL PASS;累计编码 -64.1%(wb q8)/解码 -56.2%(uwb q2) |
| 第 24 轮(2026-11-04) | 工具链升级 gcc-14.2(v1.0 API);4 个点积族内核 vlseg4e16 段加载默认采纳(8/12/4/20 → 2/3/1/5 次加载,gcc-13 跨步回退保留);真机配对基准 enc +4.3~8.9% / dec +4.3~10.0%(vs 第 23 轮等价物);事故复盘:增量构建的陈旧对象伪造 −27% 回退,do_build 改为清理式重建;毒化矩阵扩到 9 测试 = 108/108 |

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
| gcorr_diff(gain_search 9 点积批合并,第 23 轮) | 0/12,000 失配 |
| qmf_diff(QMF 固定形态,tap-iso) | 0/240,000 失配 |
| filter_diff(滤波器状态驻留) | 0/577 失配 |
| **差分合计(9 测试)** | **0 / 414,537 失配** |
| 尾部策略毒化矩阵(qemu TA/MA 全 1 × VLEN128/256/512 × 9 测试,第 24 轮) | **108/108 通过 0 失配** |
| 毒化模式端到端 A/B(第 17 轮) | ALL PASS |
| CTest 门禁(9 测试,qemu 模拟器自动包装) | 9/9 |
| qmf_diff 固定形态复验(第 18 轮,normal+毒化 × VLEN128/256/512) | 0/240000 × 4 组 |
| filter_diff(第 19 轮,normal+毒化 × VLEN128/256/512) | 0/577 × 12 组 |
| 配对基准(第 21 轮,5 对 A/B 交替 × 6 模式) | 中位数区间 ±0.3% |

### 2.2 端到端性能(同会话 A vs baseline,µs/帧)

| 模式/质量 | 编码基线 | 编码优化后 | 编码提升 | 解码基线 | 解码优化后 | 解码提升 |
|---|---|---|---|---|---|---|
| wb q8 | 847.8 | 317.4 | **−62.6%** | 84.5 | 45.8 | −45.8% |
| uwb q8 | 975.1 | 378.7 | **−61.2%** | 178.6 | 73.0 | **−59.1%** |
| nb q8 | 326.3 | 176.2 | **−46.0%** | 31.3 | 26.0 | −17.1% |
| uwb q2 | 400.9 | 223.9 | −44.1% | 174.5 | 69.5 | **−60.2%** |
| wb q2 | 273.0 | 164.5 | −39.8% | 81.1 | 42.6 | −47.4% |
| nb q2 | 197.0 | 121.8 | −38.2% | 30.1 | 24.9 | −17.4% |

(第 24 轮最终数字,gcc-14 全新构建,同会话 A vs baseline 实测;
与第 23 轮的 gcc-13 表不可直接比——baseline 的自动向量化代码在
gcc-14 下同样变快。逐轮净收益见 2.3。)

**实时率(X60 单核 1.6GHz,performance governor,第 24 轮)**:
nb q2 编码 164×、解码 804× 实时;最重的 uwb q8 编码 53×、解码
274× 实时。

**配对基准与尾延迟(第 21 轮)**:5 对 A/B 交替的 B/A 时间比中位数
与区间(±0.3%)见 docs/perf-log.md 第 21 轮;wb q8 帧时延
编码 p50/p95/p99 = 444/460/466 µs、解码 64/67/77 µs,无离群尖峰。

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
| 19 | 审查 M2-K1:滤波器状态驻留寄存器(vslide1down/vmv.x.s,VL=ord) | **−63.6% / −44~56%** |
| 20 | 审查 M2-K3 停止判定 + K5 逐内核消融归因 | (归因轮,无性能变更) |
| 21 | 审查 M3:配对基准(中位数±0.3%)、帧时延百分位、资源记录 | (测量加固,无性能变更) |
| 22 | CPU 探针 v3 + vlseg4 设备实验(K3 采纳前提:1.9×) | (探测/实验轮) |
| 23 | gain_search 9 点积批合并 rvv_gain_corr9 | −64.1% / −26%(nb q8) |
| 24 | gcc-14 工具链 + 全点积族 vlseg4 段加载(净收益,vs r23 等价物:wb q8 enc +5.2% / nb dec +9.5%) | 累计 −62.6% / −17.1%(gcc-14 口径,与 gcc-13 轮次表不可直接比) |

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
- 第 24 轮已落地 vlseg4 段加载(审查 K3 项目):依赖 gcc-14 段元组
  类型,SPEEX_RVV_SEGV4 宏守卫,gcc-13 跨步回退保留。
- 后续两条路(均需项目决策):
  1. **多线程批处理**:8 核流级并行(库外 API),批转码 ~×8;
  2. **净环境重测基线**:停板子桌面(gdm3)消除 DDR 争抢,绝对数字
     更好看但需中断桌面会话。
- 收敛判据已达成:连续三轮单轮 e2e 增益编码 ≤2.6%、解码 ≤4%,
  无剩余 ≥5% 可行目标。

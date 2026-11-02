# 真机性能日志(BPMIK102080640200 / SpacemiT X60)

设备:SpacemiT X60(rv64imafdcv,RVV 1.0,VLEN=256,8 核),Bianbu Star 2.1,
内核 6.6.63。工具链:gcc 13.3(Ubuntu 24.04 交叉),`-O2 -march=rv64gcv`,
静态链接。测试:bench_e2e,4 秒信号,reps=5 取最优;定点构建。

> 注:A=src(工作副本),B=baseline(参照)。同轮 A/B 间存在 ~5% 级测量
> 噪声(A 恒略快于 B,疑似顺序/热效应);结论以 A 列跨轮对比为准,
> 小于 ~8% 的差异不作为结论依据。

## 基线(2026-11-02,RVV_KERNELS=OFF,编译器 auto-vec)

| 模式 | 质量 | 编码 us/frame | 实时倍率 | 解码 us/frame | 实时倍率 |
|------|------|---------------|----------|---------------|----------|
| nb | q2 | 325 | x61.5 | 53 | x379 |
| nb | q8 | 458 | x43.6 | 54 | x369 |
| wb | q2 | 441 | x45.4 | 109 | x183 |
| wb | q8 | 1076 | x18.6 | 114 | x176 |
| uwb | q2 | 631 | x31.7 | 212 | x94 |
| uwb | q8 | 1270 | x15.8 | 217 | x92 |

## 第 1 轮(2026-11-02,RVV_KERNELS=ON:vq_rvv 逐 entry 点积 + inner_prod 分块)

bit-exact:设备 A/B ALL PASS。**性能与基线打平**(≤±5%,噪声内)。

| 模式 | 质量 | 编码 us/frame | vs 基线 | 解码 us/frame | vs 基线 |
|------|------|---------------|---------|---------------|---------|
| nb | q2 | 309 | -5% | 53 | ~0 |
| nb | q8 | 461 | +1% | 54 | ~0 |
| wb | q8 | 1080 | 0% | 114 | 0% |
| uwb | q8 | 1272 | 0% | 217 | 0% |

教训:基线并非标量 C,而是 gcc auto-vec;逐 entry"算一个点积做一次
redsum"的内核形状与 auto-vec 等价。下一轮改为 entry 并行(免 redsum)。

## 第 2 轮(2026-11-02,RVV_KERNELS=ON)

内核:vq_rvv **entry 并行**重写(vq_nbest + vq_nbest_sign,标量侧做符号/E
修正)+ inner_prod 分块移位语义。bit-exact:设备 A/B ALL PASS(含真机)。

| 模式 | 质量 | 编码 us/frame | vs 同轮 baseline | 解码 us/frame |
|------|------|---------------|------------------|---------------|
| nb | q2 | 306 | **-8.3%** | 52 |
| nb | q8 | 434 | **-8.8%** | 54 |
| wb | q2 | 422 | -6.2% | 109 |
| wb | q8 | 941 | **-22.2%** | 113 |
| uwb | q2 | 612 | -4.4% | 212 |
| uwb | q8 | 1135 | **-19.1%** | 217 |

q2 档收益小(hexc/低复杂度子模式码本调用少);解码未动。
调试记录:sign 内核的向量侧符号处理(vmsgt/vneg/vmax/vmerge/vsm)在真机
与 C 语义不符,改为标量侧 O(1) 修正后 PASS——每 entry 只多 2 条标量指令。

## 第 3 轮(2026-11-02,+ compute_weighted_codebook entry 并行)

bit-exact ALL PASS。

| 模式 | 质量 | 编码 us/frame | 较上轮 | vs 同轮 baseline |
|------|------|---------------|--------|------------------|
| nb | q8 | 408 | -6.0% | -14.3% |
| wb | q8 | 728 | **-22.6%** | **-39.9%** |
| uwb | q8 | 921 | -18.9% | -34.4% |

## 第 4 轮(2026-11-02,+ pitch_xcorr 4-lag 批量化 + target_update 逐元素)

bit-exact ALL PASS。

| 模式 | 质量 | 编码 us/frame | vs 同轮 baseline | vs 最初基线 |
|------|------|---------------|------------------|-------------|
| nb | q2 | 279 | -16.3% | -16.3% |
| nb | q8 | 405 | -14.9% | -14.9% |
| wb | q2 | 394 | -12.4% | -12.4% |
| wb | q8 | 724 | **-40.1%** | **-40.1%** |
| uwb | q2 | 585 | -8.6% | -8.6% |
| uwb | q8 | 915 | **-34.7%** | -34.7% |

解码全程 ~52/114/217 us/frame 不变(未动解码内核)。

**收敛判断**:第 4 轮相对第 3 轮仅 +1%——点积家族热点已吃干(剩余大头
是顺序 IIR 滤波反馈环,向量难以安全加速)。本轮循环到此收敛,后续候选:
filters 的 OVERRIDE_FILTER_MEM16(需滑窗/重排技巧)、kiss_fft 蝶形、
VLEN≥512 设备上的重调 LMUL。

### 调试记录(第 4 轮)

批量 pitch_xcorr 首版在设备端 100% CPU 死循环:内层循环忘了先
`vl = vsetvl_e16m1(n)`,首次使用的是未初始化的 `vl`,`n -= vl` 永不减到 0。
qemu 差分测试(inner_prod)之前抓不到这类问题——**死循环只在设备上
暴露,且会留下载满 CPU 的残留进程**,重跑前必须 pkill。

## 第 5 轮(2026-11-02,+ iir_mem16/fir_mem16/filter_mem16)

转置直接型滤波:寄存器内 vstate(lane ≥ ord 恒 0 不变式)+ `vrgather`
实现"mem 下滑一位"(越界索引返 0 是规范保证),全 VLMAX 无尾部歧义;
每样本标量侧复现 yi 的 SATURATE/PSHR。bit-exact ALL PASS。

| 模式 | 质量 | 编码 us/frame | vs 同轮 baseline | 解码 us/frame | vs 同轮 baseline |
|------|------|---------------|------------------|---------------|------------------|
| nb | q2 | 220 | -34.2% | 47.1 | -10.9% |
| nb | q8 | 338 | -28.8% | 48.6 | -10.4% |
| wb | q2 | 325 | -27.7% | 100.3 | -8.6% |
| wb | q8 | 638 | **-47.3%** | 105.2 | -8.2% |
| uwb | q2 | 490 | -23.4% | 196.1 | -8.2% |
| uwb | q8 | 801 | **-42.9%** | 201.1 | -8.2% |

filters 的编码端占比远超预期(第 4 轮"收敛"判断为时过早)——
分析滤波 filter_mem16/syn_percep 路径吃到了大块收益,解码首次提升。

## 第 6 轮(2026-11-02,+ interp_pitch 分数基音插值)

multicomb(解码增强器)的 interp_pitch 最终插值循环 entry 并行化
(lane=输出样本,7 个连续窗口加载 + vwmacc_vx + PSHR/narrow)。
bit-exact ALL PASS。

| 模式 | 质量 | 解码 us/frame | 较上轮 |
|------|------|---------------|--------|
| nb | q8 | 46.5 | -4.4% |
| wb | q8 | 102.9 | -2.1% |
| uwb | q8 | 201.3 | ~0% |
| 编码各档 | — | 无变化 | (multicomb 仅解码路径) |

### 收敛判断(第 6 轮后)

单轮增益回到 2-4%(仅解码),点积/滤波/插值三类热点已吃干。剩余候选
(qmf 多相、multicomb 逐元素循环、kiss_fft、lsp)预期均 ≤5% e2e,
留给下一阶段按需处理。

**累计(X60 真机,vs 原始 baseline):编码 wb q8 -47.3% / uwb q8 -42.9% /
nb q8 -28.8%;解码 nb -14% / wb -10%。**

## 第 7 轮(2026-11-03):真机剖析(无新内核)

板上无 perf 且 apt 源缺包,落地了 `-finstrument-functions` 剖析器:
`SPEEX_PROF=ON` 构建(测试/prof.c,rdtime 计时,退出时打印每函数
ticks),`nm` 符号化,`BENCH_PHASE=enc|dec` 分相位。修正符号化后
(rdtime 版,注意含插桩开销):

- **NB 编码**:pitch_search_3tap 子树 48%(内含 pitch_gain_search_3tap
  39%、其 _vq 19%),open_loop 39%,split_cb_search 11% —— 其余为
  已优化的 vq/inner_prod/pitch_xcorr。
- **解码**:LSP↔LPC 变换链(lpc_to_lsp/lsp_to_lpc)与 multicomb 尾部
  各占 ~30-44%(含嵌套),iir/fir/filter_mem16 已优化后占比很小,
  qmf_synth 次之。

**收敛结论(证据版)**:剩余热点由几十个 1-5K tick 的微小顺序/数值
循环构成(LSP 根查找的 bisect+acos、多项式级联展开、multicomb 逐元素
尾部),单点向量化预期 ≤5% e2e 且侵入最脆弱的数值代码。内核循环到此
收官;后续方向:①解码 LSP 变换链整体重写(跨系数 lane 并行,中风险);
②QMF 多相(跨输出样本 lane 并行,低风险小收益);③kiss_fft。

### 环境测量警告

板子当晚会话中重启过并进入 GNOME 桌面(initial-setup 向导),桌面渲染
抢占 DDR 带宽:同代码复测全体 +60~85%,且 RVV 优化版受带宽挤压更明显
(wb q8 A/B 比值 -47%→-27%)。跨天对比请先确认板上无桌面会话
(`systemctl stop gdm3` 或拔显示器),同会话 A/B 相对值仍然有效。

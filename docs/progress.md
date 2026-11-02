# 进度

| 阶段 | 内容 | 状态 |
|------|------|------|
| M0 | 项目骨架:目录、CMake 构建、presets、工具链文件 | ✅ |
| M0 | src/ 工作副本建立(与 baseline 内容一致) | ✅ |
| M0 | A/B bit-exact 工具(speex_ab + run_ab.sh),native 全部 PASS | ✅ |
| M0 | bench_e2e 基准工具,native 冒烟通过 | ✅ |
| M1 | 交叉编译通过(本机工具链或 scripts/docker-cross.sh) | ✅ gcc-13 rv64gcv,ubuntu 24.04 容器 |
| M1 | qemu-user 跑通 testenc + run_ab.sh | ✅ ALL PASS;testenc SNR 与宿主机逐位一致 |
| M1 | 开发板(RVV 1.0)上跑通 | ⬜ |
| M2 | 目标机 e2e 基线数据(nb/wb/uwb × 质量)入库 | ✅ docs/perf-log.md |
| M2 | 热点剖析,确认内核优先级 | ✅ 设备无 perf;按结构分析 + OPTIMIZE 文件 |
| M3 | RVV: vq/cb_search 点积(vq_nbest/_sign + compute_weighted_codebook + target_update) | ✅ 真机 bit-exact |
| M3 | RVV: ltp 开环基音(inner_prod + pitch_xcorr) | ✅ 真机 bit-exact |
| M3 | RVV: filters iir/fir/filter_mem16(转置 DF vrgather 滑窗) | ✅ 编码 -12~23%/轮,解码 -8~11% |
| M3 | RVV: interp_pitch(解码增强器) | ✅ 解码再 -2~4% |
| M3 | RVV: multicomb 尾部 + compute_rms16 | ✅ 解码再 -2.5~5.4%(解码累计 nb -19%) |
| M3 | RVV: qmf_decomp | ✅ 编码 wb -35% / uwb -37%(同会话口径) |
| M3 | RVV: qmf_synth(调度驱动重写) | ✅ 解码 wb/uwb -16~18%,nb -19% |
| M3 | RVV: pitch_gain_search_3tap_vq(增益码本) | ✅ 编码再 -7%(nb q8 340→316) |
| M3 | RVV: open_loop 选择循环+normalize16+gain 尾部+vq 向量尾部(第 14 轮) | ✅ 编码再 -3~16%(q8 模式 -12~16%) |
| M3 | RVV: lsp 根查找 | ⬜ 自适应顺序游走,bit-exact 向量化无解(已论证) |
| M3 | RVV: kiss_fft | ⬜ 编解码路径零调用,无价值(已论证) |
| M4 | 全量回归 + 数据汇总 | ✅ 第 14 轮:编码 wb q8 -58.8% / uwb q8 -56.4% / nb q8 -41.4%;解码 -16~19% |

> 完成一项更新本表;每个 M3 内核落地时在 PR/commit 说明里附
> run_ab.sh 结果与 bench_e2e 前后对比。

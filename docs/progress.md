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
| M3 | RVV: filters iir/fir(顺序反馈环,难点) | ⬜ |
| M3 | RVV: lsp/chebyshev | ⬜(点积家族已被 vq_nbest 覆盖,预期收益小) |
| M3 | RVV: kiss_fft | ⬜ |
| M4 | 全量回归 + 数据汇总 | ✅ 本轮收敛:wb q8 -40% / uwb q8 -35% / nb q8 -15% |

> 完成一项更新本表;每个 M3 内核落地时在 PR/commit 说明里附
> run_ab.sh 结果与 bench_e2e 前后对比。

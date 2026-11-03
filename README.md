# libspeex-rvv

把 [libspeex 1.2.1](https://gitlab.xiph.org/xiph/speex) 移植到 64 位
RISC-V,并用 RVV(RISC-V Vector Extension 1.0)逐内核定点优化。
硬性验收标准:优化后的编码码流与参考实现 **bit-exact**,同时给出
开发板上的端到端加速比。

## 目录结构

```
baseline/   上游 xiph speex 1.2.1 原始代码(只读参照,勿改动)
src/        移植工作副本;RVV 内核放 src/libspeex/rvv/
test/       speex_ab(确定性编解码工具)、run_ab.sh(bit-exact 回归门)
            以及上游 testenc/testenc_wb/testenc_uwb
benchmark/  bench_e2e 端到端编解码吞吐
cmake/      工具链文件 + config.h / speex_config_types.h 模板
scripts/    build.sh、docker-cross.sh、device-test.sh(X60 真机 adb 循环)、
            Dockerfile.buildenv(缓存交叉构建环境)
docs/       最终报告(final-report.md)、计划(plan.md)、RVV 编码要点(rvv-notes.md)、
            逐轮性能日志(perf-log.md)、进度(progress.md)、
            独立审查与响应(code-review-2026-11-03.md);
            docs/ref/ 为 RVV 参考资料库(V 规范、intrinsics、工具链选项、T-Head 实战等)
test/       另含 7 个内核级差分测试(*_diff.c,已注册 CTest,qemu+真机双跑)、
            CPU 能力探针 cpu_probe.c、尾部策略矩阵脚本 build/review/run-tail-matrix.sh
skills/     rvv-optimization-loop:RVV 优化循环方法论与踩坑 skill
```

## 快速开始(本机开发)

```bash
cmake --preset native && cmake --build build/native -j
./test/run_ab.sh                                   # src vs baseline 全模式 bit-exact 回归
./build/native/benchmark/bench_e2e wb 8 2          # 端到端基准
./build/native/test/testenc_wb                     # 上游自带 SANity 工具(读 stdin 参数见源码)
```

默认定点构建(`FIXED_POINT=ON`),即嵌入式 RISC-V 的目标配置。

## 交叉编译到 riscv64

本机没有 RISC-V 工具链时,一条命令在 Docker 里完成"交叉编译 +
qemu 运行 + A/B 回归":

```bash
./scripts/docker-cross.sh
```

本机有工具链时:

```bash
cmake --preset riscv64 && cmake --build build/riscv64 -j
RUNNER="qemu-riscv64 -L /usr/riscv64-unknown-linux-gnu" ./test/run_ab.sh
```

## 真机测试(SpacemiT X60 开发板)

目标机 BPMIK102080640200:SpacemiT X60(rv64imafdcv,RVV 1.0,VLEN=256,
8 核,Bianbu Linux 2.1),通过 adb 连接。`scripts/device-test.sh` 完成
静态交叉构建、推送、设备端 A/B bit-exact 与基准:

```bash
./scripts/device-test.sh build ab bench   # 或分步:build / deploy / ab / bench
```

最终结果(第 19 轮,同会话 A vs baseline,完整数据见
docs/final-report.md 与 docs/perf-log.md):

| 模式/质量 | 编码基线 | 编码优化后 | 编码提升 | 解码基线 | 解码优化后 | 解码提升 |
|---|---|---|---|---|---|---|
| wb q8 | 1209.2 us/帧 | 440.7 | **-63.6%** | 114.8 | 64.4 | **-43.9%** |
| uwb q8 | 1399.6 | 543.6 | **-61.2%** | 218.3 | 98.6 | **-54.9%** |
| nb q8 | 475.9 | 258.1 | **-45.8%** | 54.3 | 40.3 | -25.8% |
| wb q2 | 450.8 | 253.0 | -43.9% | 109.7 | 60.0 | **-45.3%** |
| uwb q2 | 662.6 | 352.6 | **-46.7%** | 212.8 | 93.7 | **-56.0%** |
| nb q2 | 334.2 | 181.9 | **-45.6%** | 53.0 | 38.8 | -26.8% |

单核实时率(X60 @1.6GHz):nb q2 编码 110×/解码 516× 实时;
最重的 uwb q8 也有编码 37×/解码 203×。

位精确认证:真机 A/B 6 模式(含 aa/ab/ba 交叉解码)ALL PASS;
7 个内核级差分测试共 **165,960 组随机向量 0 失配**;尾部策略毒化
矩阵(qemu TA/MA 全 1 × VLEN128/256/512)**72/72 通过**;毒化模式
端到端 A/B ALL PASS;CTest 7/7。

## 日常开发流程

1. 只改 `src/`;`baseline/` 永远原样,是 A/B 的参照系。
2. 每次改动后 `./test/run_ab.sh`(native)与
   `./scripts/device-test.sh ab`(真机)必须 **ALL PASS**(码流逐字节
   一致,含交叉解码)。它们用相同选项构建 src 与 baseline 两份库再对比。
3. 性能对照 `bench_e2e` / 真机 bench;内核级优化按 `docs/plan.md` 的
   优先级表推进,开发规范与踩坑见 `src/libspeex/rvv/README.md`、
   `docs/rvv-notes.md` 和 skill `skills/rvv-optimization-loop/`。
4. 性能数据记入 `docs/perf-log.md`,进度记录在 `docs/progress.md`。

## 当前状态

**19 轮优化 + 1 轮独立审查响应**:21 个 RVV override 点落地(vq 全家、
点积家族、滤波器组、QMF 固定形态、基音全套、lsp_to_lpc、normalize16、
open_loop argmax、滤波器状态驻留寄存器、CPU 探测驱动的转置布局/分链
累加等),X60 真机全部 bit-exact,编码最高 **-63.6%**、解码最高
**-56.0%**。

2026-11-03 独立审查(code-review-2026-11-03.md)指出的问题已全部
修复:尾部策略 `_tu` 纪律(R1/R2/R3)、哨兵等值种子(R6)、ABS16 的
INT16_MIN 漏检(R7)、自指 guard 静默失效 13 轮的 target_update(R9)、
构建矩阵(R8)、测试与产物可追溯性(M1);审查 M2 的 QMF 固定形态
(K2)已落地。能力推断按审查要求重测修正(R5:吞吐 ≈0.33-0.42
vec-op/cyc、跨步 3.7×、向量/标量 ≈32×)。

剩余可选路径:审查 K1(滤波器状态驻留寄存器)/K3(点积寄存器分配)
预算制实验、8 核流级并行的批处理(库外 API)、停桌面净环境基线重测。

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
            逐轮性能日志(perf-log.md)、进度(progress.md);
            docs/ref/ 为 RVV 参考资料库(V 规范、intrinsics、工具链选项、T-Head 实战等)
test/       另含 7 个内核级差分测试(*_diff.c,qemu+真机双跑)
            与 CPU 能力探针 cpu_probe.c
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

最终结果(第 16 轮收敛,同会话 A vs baseline,完整数据见
docs/final-report.md 与 docs/perf-log.md):

| 模式/质量 | 编码基线 | 编码优化后 | 编码提升 | 解码基线 | 解码优化后 | 解码提升 |
|---|---|---|---|---|---|---|
| wb q8 | 1209.9 us/帧 | 474.3 | **-60.8%** | 114.2 | 92.4 | -19.1% |
| uwb q8 | 1402.1 | 587.2 | **-58.1%** | 219.2 | 175.8 | -19.8% |
| nb q8 | 475.2 | 277.5 | **-41.6%** | 54.0 | 42.3 | -21.7% |
| wb q2 | 447.7 | 280.7 | -37.3% | 109.6 | 88.2 | -19.6% |
| uwb q2 | 638.9 | 393.7 | -38.4% | 213.5 | 174.6 | -18.2% |
| nb q2 | 333.1 | 205.7 | -38.3% | 52.6 | 41.2 | -21.7% |

单核实时率(X60 @1.6GHz):nb q8 编码 72×/解码 473× 实时;
最重的 uwb q8 也有编码 34×/解码 114×。

位精确认证:真机 A/B 6 模式(含交叉解码)ALL PASS;7 个内核级差分
测试共 **165,960 组随机向量 0 失配**(qemu VLEN=128 与真机 VLEN=256
双环境)。

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

**优化循环已收敛(16 轮)**:20 个 RVV override 点落地(vq 全家、点积
家族、滤波器组、qmf、基音全套、lsp_to_lpc、normalize16、open_loop
argmax、CPU 探测驱动的转置布局/分链累加等),X60 真机全部 bit-exact,
编码最高 **-60.8%**、解码 -18~22%。剩余热点(lsp 根查找自适应顺序
游走、kiss_fft 零调用、已优化内核残部)均不可向量化或无价值。

完整结论、逐轮数据与事故复盘见 **docs/final-report.md**;
方法论与踩坑见 `skills/rvv-optimization-loop/`、`docs/rvv-notes.md`。
后续可选路径(需项目决策):8 核流级并行的批处理(库外 API,~×8)、
停桌面后的净环境基线重测。

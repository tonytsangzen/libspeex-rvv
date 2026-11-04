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

构建镜像工具链:**gcc-14.2**(`scripts/Dockerfile.buildenv`,
RVV intrinsics v1.0 API;gcc-13 的 v0.11 API 路径仍受支持——段加载
内核自动回退跨步加载)。

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

最终结果(第 23 轮,同会话 A vs baseline,完整数据见
docs/final-report.md 与 docs/perf-log.md):

| 模式/质量 | 编码基线 | 编码优化后 | 编码提升 | 解码基线 | 解码优化后 | 解码提升 |
|---|---|---|---|---|---|---|
| wb q8 | 1210.9 us/帧 | 435.3 | **-64.1%** | 114.9 | 65.0 | **-43.4%** |
| uwb q8 | 1401.8 | 538.0 | **-61.6%** | 218.6 | 98.5 | **-54.9%** |
| nb q8 | 475.9 | 250.9 | **-47.3%** | 54.2 | 40.3 | -25.7% |
| wb q2 | 449.8 | 250.3 | -44.4% | 109.7 | 60.6 | **-44.8%** |
| uwb q2 | 640.9 | 349.5 | **-45.5%** | 214.0 | 93.8 | **-56.2%** |
| nb q2 | 334.1 | 178.9 | **-46.5%** | 53.0 | 39.1 | -26.1% |

单核实时率(X60 @1.6GHz):nb q2 编码 112×/解码 511× 实时;
最重的 uwb q8 也有编码 37×/解码 203×。

位精确认证:真机 A/B 6 模式(含 aa/ab/ba 交叉解码)ALL PASS;
9 个内核级差分测试共 **414,537 组比较 0 失配**;尾部策略毒化
矩阵(qemu TA/MA 全 1 × VLEN128/256/512,**9 测试 × 4 策略 × 3
VLEN = 108/108 通过**);毒化模式端到端 A/B ALL PASS;CTest 9/9。

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

**23 轮完成(20 轮优化 + 独立审查响应:验证/归因/测量)**:
21 个 RVV override 点落地(vq 全家、点积家族、滤波器组、QMF 固定
形态、基音全套、lsp_to_lpc、normalize16、open_loop argmax、滤波器
状态驻留寄存器、CPU 探测驱动的转置布局/分链累加等),X60 真机全部
bit-exact,编码最高 **-64.1%**(wb q8)、解码最高 **-56.2%**
(uwb q2)。

第 24 轮:构建镜像升级 gcc-14.2(intrinsics v1.0 API,段元组类型
可用),`SPEEX_RVV_SEGV4` 特性宏下 4 个点积族内核改用 vlseg4e16 段
加载(inner_prod 2、batch5 3、sumsq 1、pitch_xcorr 5 次段加载,分别
取代 8/12/4/20 次跨步加载;field k 与第 k 路跨步加载逐元素相同,
MAC 顺序不变 → 位精确),gcc-13 跨步回退路径经关宏全树编译 +
CTest 验证保留。毒化矩阵扩到 9 测试 × 4 尾部策略 × 3 VLEN 全绿。

独立审查(code-review-2026-11-03.md)的 M0(正确性)/M1(验证
体系:CTest、毒化矩阵、配对基准、产物新鲜度)/M2(K1、K2 落地,
K3 停止判定,K5 消融归因)已全部执行完毕;M3(配对基准、帧时延
百分位、资源记录、版本声明修正)完成。vlseg4 段加载实验已随工具链
升级落地(第 24 轮)。剩余可选路径:8 核流级并行的批处理(库外
API)、停桌面净环境基线重测。

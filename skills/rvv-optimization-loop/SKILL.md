---
name: rvv-optimization-loop
description: 在 RISC-V 真机(SpacemiT X60/Bianbu)上对 libspeex 做 RVV 内核循环优化的标准流程与踩坑清单。当要新增 RVV 内核、真机 A/B bit-exact 验证、adb 部署测试、或遇到 vsetvl/intrinsics/交叉编译相关问题时使用。
---

# libspeex-rvv 真机优化循环

目标设备:BPMIK102080640200(SpacemiT X60,rv64imafdcv RVV 1.0,VLEN=256,
Bianbu Linux 2.1,非 Android)。工具链:Ubuntu 24.04 容器内
gcc-riscv64-linux-gnu(gcc 13),`-march=rv64gcv` + 静态链接,adb 推送。

## 标准循环(每轮一个内核)

```bash
RVV_KERNELS=ON ./scripts/device-test.sh build ab bench
```

1. 读 C 参考与 `fixed_generic.h` 宏定义,**先推导 bit-exact 论证**再写码。
2. 内核放 `src/libspeex/rvv/<module>_rvv.h`,定义 `OVERRIDE_*` 宏;模块 `.c`
   加 `#elif defined(_USE_RVV)` 分支。**头文件里的函数定义必须用同一个
   `OVERRIDE_*` 宏包住**(否则做 bisect 注释宏时重复定义)。
3. `build ab` → 设备端 A/B 全 PASS → `bench` → 把数字记进
   `docs/perf-log.md`(对比"同轮 baseline",剔除顺序噪声)。
4. 基线(baseline)永远不改动;性能对照用 `RVV_KERNELS=OFF`(只开
   `-march` 的 auto-vec)——**对手是 gcc auto-vec,不是标量 C**。

## 内核形状(最重要的经验)

- **逐 entry"点积 + 每次redsum"= 平手**(第 1 轮教训)。redsum 延迟链
  吃掉全部收益。要赢必须 **entry 并行**:一 lane 一 entry、`in[j]` 用
  `vwmacc_vx` 广播、codebook 列用 `vlse16` 跨步加载、免 redsum
  (vq_rvv.h 是范本)。
- **每 entry 的 O(1) 修正放标量侧**(E 修正、符号翻转):每 entry 仅
  1-2 条标量指令,且绕开 gcc-13 的 mask/merge intrinsics 语义坑
  (vq_nbest_sign 的向量符号路径真机不 bit-exact,标量化后 PASS)。
- 分块移位语义(如 `inner_prod` 每 4 样本 `>>6` 后再累加)必须逐块
  复现:lane=块、跨步加载、`vsra` 后再归约。**先读宏再下结论**:
  `MAC16_16` 是纯 int32 环绕加(结合律 mod 2^32,顺序无关 ⇒ 向量化安全),
  但 `PSHR32` 有舍入项、`SUB16` 是截断减——一字之差全部重来。
- IIR 反馈环(filters.c)是顺序依赖,常规向量化无解,别硬啃。

## bit-exact 排障流程

1. A/B 失败 → 注释单个 `OVERRIDE_*` 宏二分(重新 build+deploy+ab!)。
2. 数值反例 → 写差分测试(qemu 里跑):参考 `test/inner_prod_diff.c`
   (C 参考副本 vs 链接进 lib 的 RVV 版,随机+极端值,8 万组)。
3. **真机 A/B 是最终裁判**:qemu(TCG)与 X60 硬件语义基本一致,但
   未初始化变量/UB 只在真机上暴露为死循环或错误输出。

## 踩坑清单(每条都真实付出过时间)

### 设备/部署
- **`ab` 子命令必须先 deploy**:改了源码但忘 deploy,会用板上旧二进制
  "验证"——二分结论全部作废(device-test.sh 已强制 ab 前 deploy)。
- 死循环进程会**残留板上打满 CPU**(load 3.0),重跑前
  `adb shell pkill -9 -f speex_ab`。
- macOS 无 `timeout` 命令;`adb shell` 的命令找不到时 `which` 先探。
- 后台跑 device-test 时,**不要并行编辑源文件**——容器挂载读到的可能是
  编辑中间态(报错位置完全对不上)。
- 交叉 libc:ubuntu 容器 `--no-install-recommends` 会漏掉
  `libc6-dev-riscv64-cross`(crt1.o/libc.a),静态链接报 cannot find
  crt1.o;动态链接也缺 Scrt1.o。显式安装。
- 静态 glibc 二进制在 Bianbu 上直接跑,免去 glibc 版本地狱;校验静态用
  `readelf -l | grep INTERP`(容器里没有 `file` 命令)。

### 内核形状补充(第 5/6 轮)
- 转置直接型滤波(mem[j]=mem[j+1]+coef[j]*x)用**寄存器滑窗**:维护
  vstate(全 VLMAX、lane ≥ ord 恒 0 不变式),`vrgather(vstate, vid+1)`
  做下滑——**vrgather 越界索引返 0 是规范保证的**,比 vslidedown 的
  尾部语义可靠;全部操作跑满 VLMAX,无尾部歧义。范本 filters_rvv.h。
- gcc-13 只有 `vid_v_u32m2`(无符号)且 vrgather 索引必须是
  `vuint32m2_t`。
- 每 entry 的标量修正(yi 的 SATURATE/PSHR)从 mem 数组读回即可:每样本
  一次 40B vse32,换整个正确性简单性。
- 覆盖 static 函数(如 interp_pitch)且依赖同文件的 static 表时,把
  `#include "rvv/xxx_rvv.h"` 放在**函数原位**(表定义之后),不要放文件头。
- 解码热点与编码不同:enhancer(multicomb/interp_pitch)只在解码路径,
  编码不动是正常现象。
- 板子 USB 瞬时掉线(adb device not found)偶发,`adb reconnect` 或重试
  即可,构建产物仍有效。

### RVV intrinsics(gcc 13 为准)
- `vredsum_vs_i32m2_i32m1(acc, zero, vl)` 参数序是 **(向量, 标量载体)**,
  与 RVV C-API 文档相反;clang 相反。写注释,换编译器时搜 `vredsum`。
- 泛型 `vmerge_vvm` 在 C 里分发失败(返回 int),用带后缀的
  `vmerge_vvm_i32m2`。
- **数组元素不能是 RVV 向量类型**(gcc-13 直接报错)——把向量列存进
  标量 VLA 缓冲,用 `vle16` 重载。
- 尽量避开 `vsm`(mask 存储)语义争议:转成 0/1 整数向量再 `vse32`。
- `vsetvl` **必须放在循环体第一句、先于任何用 vl 的语句**。忘写 =
  未初始化 vl → `n -= vl` 永不归零 → 设备 100% CPU 死循环,且差分测试
  和 qemu 都可能"看起来能跑"。
- `vwmacc_vx/vv`、`vsra`、`vncvt`、`vlse16`(stride 单位是**字节**)、
  `vsext_vf2` 都是实测可靠的基础件;EXTRACT16 类截断用
  `vsra + vncvt` 精确复现,别用 `vnclip`(受 vxrm 舍入模式影响)。

### 真机剖析(第 7 轮沉淀)
- 板上装不上 perf(源缺包)时,用 `-finstrument-functions`:CMake 开关
  `SPEEX_PROF=ON`(test/prof.c,rdtime 计时,退出打印
  `PROF <addr> <ticks> <calls>`),host 端 `nm <binary>` 符号化。
- **符号化必须用实际运行的二进制**(bench_e2e ≠ speex_ab,布局不同,
  错配会得到看似合理的错名,极难察觉)。
- 计时用 `rdtime`(用户态 CSR,几十周期);**clock_gettime 在板上走
  系统调用(~2µs),钩子开销直接淹没数据**(实测 2.6× 虚高)。
- `adb shell` 不转发远端 stderr——重定向必须写在远端命令里
  (`... 2>&1 >/dev/null`),写在 adb 外面只会得到空文件。
- 自定义 CMake 开关经 docker 传递时,务必 `-e VAR=...` 显式透传
  (容器内求值 `${VAR:-OFF}` 永远是 OFF)。
- bench 加 `BENCH_PHASE=enc|dec` 分相位;解码剖析的样本里混着
  prime encode,按符号区分。
- 板子重启/桌面会话(GNOME)会抢 DDR 带宽:全体变慢且向量内核比值
  被压缩,跨会话对比绝对值前先检查 `uptime`/负载/桌面进程。

### 剖析解读的边界(第 9 轮教训)
- `-finstrument-functions` 剖析器是 **inclusive 计时**:每函数结果含
  插桩钩子开销(~1K ticks/调用,rdtime 版)。单次耗时低于 ~2K ticks 的
  函数占比严重虚高、向量化后可能"毫无收益"——lsp_to_lpc"占解码 29%"
  实为 ~90% 钩子开销。热点只有在总耗时远超 `调用数 × 2K ticks` 时才可信。
- 宏展开务必逐字符读:`MULT16_32_Q14(a,b) = a·hi + ((a·lo)>>14)` 的
  移位只作用于低半段乘积,凭直觉写会错,A/B 门能抓住但浪费一轮。

### 性能事故(第 13 轮)
- **内核里的调试脚手架(SUMDIFF 逐 entry printf)未受 ref_debug 保护
  (dbg 数组未填充→条件恒真)→ 每次 _vq 调用 ~64 次控制台 printf →
  编码 780× 减速**。规则:调试代码验证完立即删除;bench 异常数量级
  放大时先查 printf/日志 I/O。
- 增益码本搜索(9 项二次型,码字为 lane)是 entry-并行的又一范例:
  int8 跨步加载(vlse8 stride 4)+ vwmul/vncvt 截断乘 + vwmacc_vx,
  argmin 标量复刻选择规则。

### 验证纪律事故(第 14 轮)
- **新内核必须有直接差分测试**:e2e A/B 不能替代。第 14 轮
  `rvv_gain_build_exc` 第二段调用把"j=from 处的绝对地址"传给了
  "从 from 起算"的 src 形参(基址重复偏移),第二子段全读错——qemu
  e2e 被跳过、真机 A/B 又因陈旧二进制假通过,最终被新写的
  gshelp_diff 在真机抓出。内核落地与差分测试同 commit。
- **build 失败会被管道掩盖 + ab 部署陈旧二进制**:device-test.sh 的
  docker 退出码可被 grep/tail 吞掉;build 失败后 ab 照样 deploy 旧
  二进制,连续 FAIL 全是假信号。ab 前先肉眼看 build 成功行
  ("== build done =="),或 ab 脚本内硬校验产物时间戳。
- **prof-on 污染基准**:-finstrument-functions 对调用密集路径虚高
  40-70%(解码 74→43µs 仅为去插桩)。基准一律 SPEEX_PROF=OFF;
  剖析(prof-on)与测速(prof-off)必须分开构建。
- **差分测试驱动自身也会错**:对比缓冲区要严格分开(ref 用 buf、
  被测用 buf2);曾把 RVV 调用写成与参考同缓冲,SW-MISMATCH 风暴
  全是测试 bug。测试报错时先怀疑测试,再怀疑内核。
- gcc-13 新坑:vmerge_vvm(op1,op2,m)=m?op2:op1(首参 FALSE 分支,
  与规范相反);掩码类型 = vbool(SEW/LMUL);VARDECL 指针类型展开成
  二级指针;内核级 R14_NO_* 编译开关可单内核真机二分(详见
  docs/rvv-notes.md)。

### 方案收益复盘事故(第 15 轮)
- **微基准形状必须与生产完全一致**:探针用 e16m2(32 lane)+ stride
  64B 测出 3.4×,生产旧内核是 e16m1(16 lane)+ stride 2*len(10~40B)
  ——lane 数与步长都影响 strided load 代价,真实加速 ~2-3×。探针
  结论外推前先对齐 lane 数/步长/数据规模。
- **收益乘法要按剖析占比分解**:raw_dists 只占 vq_nbest ~43%,3.4×
  只该乘在那一半上;把内核级加速乘满全函数导致方案预期 -5~10%、
  实际 -1~3%。预期收益 = 内核加速 × 该内核剖析占比 × 阶段权重。
- **剖析的阶段污染**:BENCH_PHASE=dec 的 bench 仍先编码,profiler
  按符号聚合无法区分阶段——第 14 轮把编码阶段的 split_cb/pitch
  行误读为解码成分。跨阶段剖析需要运行时剖析门(PROF_ENABLE)。
- **存量内核的测试范围缺口**:gain-VQ 的 `(int16_t)(-C16[k])` 在
  C16[k]=-32768 时负溢出翻转符号——e2e 永远触发不了(normalize16
  限幅 ±32767),全范围随机差分测试一跑就中(1/2000)。差分测试
  的输入范围应覆盖内核数学的完整域,而不是只覆盖"编解码会出现的
  值";反之若内核只承诺编解码域,测试应显式声明并限制域。
- 布局约定变更(vq 码本改列主序)必须同步:共享代码的读取点、
  内核内的重算路径(_sign 的符号重算曾漏改)、差分测试两侧。

### 剖析门与解码真成分(第 16 轮)
- prof.c 增加 `speex_prof_enabled` 运行时开关(弱符号 `extern
  __attribute__((weak))`,无 PROF 构建自动退化);bench 在 dec 相位的
  一次性编码前后翻转。**翻转只允许在顶层库调用之间**,否则进出栈
  记账失衡。
- 仪器化剖析对小函数的放大是数量级级的:multicomb 仪器化占比 46%,
  真实批合并收益仅折算 dec -3~4%——高频小函数的 enter/exit 开销
  (~30+ ticks/次)远超其真实计算。凡 <100 ticks/call 的符号,排名
  只可信相对序,占比不可信。
- 连续多轮单轮增益 <3% 且无 ≥5% 可行目标 = 收敛信号,继续挖的
  边际成本大于收益。

### 脚本/bash
- `${VAR:-default}` **不能对同一变量用两次**(第一次已赋值,交叉模式的
  第二默认永远不生效)——分支里各自给默认。
- 自写脚本的多子命令分发只取 `$1` 会静默吞掉后续参数(`build ab` 只跑
  build),用 `for cmd in "$@"` 循环。

## 收敛数据(X60,详见 docs/perf-log.md)

14 轮后编码 e2e(同会话 A vs baseline,prof-off):wb q8 **-59%**、
uwb q8 **-56%**、nb q8 **-41%**;解码 -16~19%。第 14 轮单轮增益
(同会话 r13→r14 对照):q8 模式编码再 -12~16%,q2 模式 -3~6%。

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

### 尾部策略与测量纪律(第 17 轮,独立审查驱动)
- **短 VL 写的尾部是 agnostic(indeterminate)**:持久向量累加器、
  状态向量、候选值/索引的短 VL 更新一律 `_tu`,否则跨操作读尾部是
  源码正确性问题。验收:qemu `-cpu rv64,v=true,vlen={128,256,512}
  ,rvv_ta_all_1s=true,rvv_ma_all_1s=true` 毒化矩阵——普通 QEMU 全过
  不能证明可移植正确性(全 1 尾下 inner_prod/batch5 失配、vqtail
  SIGSEGV、open_loop 数千失配,一次全暴露)。
- **哨兵初始化 + 严格小于的 argmin 永远捕不到等于哨兵的实值**:
  C 参考的"首条目强制接纳"要用标量种子显式实现,向量路径从
  [1, entries) 开始。
- **ABS16(-32768) 经整型提升得 32768**:窄位宽向量 abs 会漏检;
  对原始值做双向阈值(vredmax>hi || vredmin<lo)才精确。
- **微基准的全部累加流/加载流必须消费进 sink**,否则死代码消除会
  产生 1000× 的假数(v2 探针的"1 vec-op/cyc""跨步 8×""40× 收益"
  全部因此撤回);rdtime 计时形态在 X60 上还有病态代码生成,
  计时统一用 clock_gettime。
- **guard 自指**:`#if defined(X_OVERRIDE)` 内 `#define X_OVERRIDE`
  永不为真——override 从第 4 轮起静默失效了 13 轮;启用前先 grep
  预处理结果,不要把"写了 override"当作"启用了 override"。
- **测试与产物的新鲜度**:device-test 的 B 侧(baseline)链接差分
  测试必失败(无 RVV 符号),构建目标要按侧裁剪;ab/bench 强制先
  build;build 失败被管道掩盖时 ab 会用陈旧产物给出假结果。

### 脚本/bash
- `${VAR:-default}` **不能对同一变量用两次**(第一次已赋值,交叉模式的
  第二默认永远不生效)——分支里各自给默认。
- 自写脚本的多子命令分发只取 `$1` 会静默吞掉后续参数(`build ab` 只跑
  build),用 `for cmd in "$@"` 循环。

## 收敛数据(X60,详见 docs/perf-log.md)

14 轮后编码 e2e(同会话 A vs baseline,prof-off):wb q8 **-59%**、
uwb q8 **-56%**、nb q8 **-41%**;解码 -16~19%。第 14 轮单轮增益
(同会话 r13→r14 对照):q8 模式编码再 -12~16%,q2 模式 -3~6%。

### 调度解释器反模式(第 18 轮,审查 K2)
- **运行时调度表 + 解释器循环是自找的间接层**:每项 5 次数组读 +
  switch 的标量开销与向量 MAC 同量级;若项模式在编译期静态,直接
  展开成固定循环(寄存器复用参考代码的复用)。本例内核内省去
  5KB 调度表构建与 256 项解释,解码 wb/uwb e2e -25~42%。
- **逐 lane 窗口索引的 +base 项在两处出现**(块首与循环内滚动更新)
  时极易漏改一处——第一块 base=0 会掩盖块首正确性;差分测试的
  "SYNTH MISMATCH 从第 17 输出开始、仅 acc2/acc3 错"模式直接指向
  两处漏改,比读汇编快。
- **helper 的早退守卫不能静默无输出**:形状不支持时必须回退到逐字
  C 参考(与 filters 的 qmf_synth_c/iir_mem16_c 等同一模式)。

### 脚本/bash""")
open('skills/rvv-optimization-loop/SKILL.md','a').write('')

### 调度解释器反模式(第 18 轮,审查 K2)
- **运行时调度表 + 解释器循环是自找的间接层**:每项 5 次数组读 +
  switch 的标量开销与向量 MAC 同量级;项模式编译期静态时直接展开成
  固定循环(寄存器复用参考代码的复用)。本例省去 5KB 调度表构建与
  256 项解释,解码 wb/uwb e2e -25~42%。
- **逐 lane 窗口索引的 +base 出现在块首与循环内滚动更新两处**,漏改
  一处时第一块(base=0)会掩盖块首正确性;"失配从第 N 块起点开始、
  仅部分累加器错"的失配模式直接指向漏改位置,比读汇编快。
- **helper 的早退守卫不能静默无输出**:形状不支持时回退到逐字 C
  参考(qmf_synth_c/iir_mem16_c 等同一模式)。

### 逐内核消融归因(第 20 轮,审查 M1/K5)
- **归因的正确工具是消融,不是插桩剖析**:单开关重建(SPEEX_OPT_FLAGS
  传 -D) + 真机 prof-off 基准,每组 ~4 分钟。插桩占比与消融实测的
  偏差可到 2~4 倍(qmf_synth 仪器化 13.9% vs 消融实测独占 31%/56%)。
- 消融差值的噪声带约 ±2%(不同时刻运行);<2% 的结论不可靠,
  要么配对复测要么放弃结论。
- 停止判定模板:工具链阻断(记录版本与缺失符号)、权衡反转
  (算清操作数倍数 vs 加载收益)、占比低于噪声带——三者任一即停,
  不为用完预算改写。

### 设备时钟陷阱(第 22 轮)
- **真机微基准禁用 clock_gettime**:设备重启后 vDSO 时钟在紧循环下
  欠账(6ms 真实工作读出 6µs),亚毫秒间隔完全失真;rdtime(24MHz
  CSR,~30ns 开销)不受影响且已标定。多秒级区间(bench_e2e)实测
  正常。判别方法:同一循环同时打 wall 与 rdt 两种计时,比值失衡
  即时钟源异常。
- 微基准防提升检查单(审查 R5 后固化):①全部累加流/加载流消费进
  sink;②输入在环内变异(固定偏移轮转会被预计算——0.2ns/call 的
  "结果"就是表查询);③计时用 clock_gettime 或已标定 rdtime;
  ④换工具链后重测,不外推。
- **工具链升级检查单**(gcc-13 → gcc-14 实测):①vmerge 操作数序
  实测不变;②隐式声明变硬错误(C23 默认),补标准头;③gcc-14 不
  提供无后缀交叉 gcc 名,需符号链接;④段元组类型(vlseg4)解禁。

### 构建卫生与测量优先(第 24/27 轮,X100/K3 session)
- **改内核门控宏后禁信增量构建**:门控宏翻转后增量 make 漏编
  nb_celp.o,混合新旧的"弗兰肯二进制"测出 −27% 假回退,消融数据
  全部作废。定位靠目标文件级反汇编 diff(找 vlseg4/vlse16 计数与
  时间戳矛盾);修复是构建脚本先 rm -rf。同码对照实验(两份
  identical 配置互测应 = 0.0%)用于自证测量方法可靠。
- **编译器选型只认同机配对测量**:厂商自带 gcc(默认
  -mtune=spacemit-x100)全面慢于交叉 gcc-14(编码 −8~19%、解码
  −25~47%),strict-align 与调优模型均被单变量排除。"厂商出品"
  不是性能证据。
- **微基准结论不自动迁移到 e2e**:vlseg4 微基准 1.9×,但e2e 首测
  回退——先查构建卫生,再用"单内核启用/禁用"配对消融复核;反之
  微基准慢也不代表 e2e 慢(布局效应)。非单调的消融结果 = 布局或
  构建污染的信号,不是真实内核效应。

### 形状级优化与伪串行链(第 29-32 轮)
- **跨步加载惩罚跨代成立**:X100 上 stride-4B 短跨度(32B)仍
  1.76× 慢于 unit-stride(qmf 形状实测)。凡 lane 间索引步进 ≠ 1
  的内核,先找奇偶/模 N 结构做**解交织预处理**(qmf_synth:xx 偶/
  奇流反转存储,tap 加载全变 vle16,实测解码 +5.6~8.7%)。
- **伪串行链识别**:携带变量的循环先代入几项——若携带值恰是上一
  迭代的某数组元素(如 lsp_to_lpc 的 xout1=xp[j+1]),整链是逐元素
  公式,i32 环绕下结合性使其可位精确向量化。已两度命中
  (compute_rms16 的 max-abs 扫描、lsp_to_lpc 的 ak 循环)。
- **精确 i64 乘积替代 hi/lo 模拟**:MULT16_32_Q14 型的向量模拟
  (~8 条)常可换成 vwmacc_vx_i64m4 + vnsra(2 条):i64 无中间
  回绕,算术移位恒等式 + 截断 = C 的 mod-2³² 结果,即使 C 中间量
  回绕也同余。
- **语义陷阱三则**:①libspeex 编码器原地修改调用方输入帧
  (highpass 直写)——重复编码不幂等,基准 harness 每轮须恢复
  pristine 输入;②int16 扫描变量的回绕(ABS(−32768)=32768 装回
  int16 变负)破坏 redmax 等价性——精确最大值命中 32768 时回退
  标量路径;③钳位必须在窄化之前(vncvt 先做会把超范围值回绕)。
- **独立差分测试 > 进程内调试钩子**:钩子可能根本没执行(dbg 构建
  0 MISMATCH 与 A/B 失败并存);随机矩阵上并排运行参照与被测的
  独立小程序一次定位 off-by-one(new[j]==ref[j-1] 的移位模式)。
- **工具链升级可能解锁旧实验**:gcc-15 修复了 gcc-14 的 RVV+LTO
  ICE——被 ICE 阻断的实验要登记"解锁条件"而不是简单放弃。
- **vslide1down 依赖链不值得优化**:iir K1 形态(每样本
  vmv.x.s+slide1down+wmac)实测 3.7× 快于纯标量 order-10 IIR——
  顺序递推内核的向量状态形态已是正确答案,不要凭直觉猜瓶颈。

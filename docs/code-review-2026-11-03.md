# libspeex-rvv 独立代码审查与继续优化方案

日期：2026-11-03。审查对象：`0f103b62720b9ab899afb7e844da30af3cb9300d`。

本次交付是审查与计划，不实施修复、性能改写或设备部署。下文实验是用户明确“只做计划”之前已经完成的离线验证；之后仅整理证据和编写本文。没有本轮真机性能复测。

## 1. 判断与证据边界

**目前不能接受“全部位精确、优化已收敛、无剩余 ≥5% 可行目标”的项目结论。** 原因首先是可复现的正确性错误，其次是微基准和测试证据存在缺陷。继续优化应依次完成：正确性修复 → 可靠测试与归因 → 有预算约束的内核实验 → 真机端到端验收。

这不等于否定历史端到端加速。已有优化完全可能有效，但本轮没有独立重测其时间数据，也无法从现有探针证明 X60 的吞吐上限或剩余空间。不能把 QEMU 执行时间当作 X60 性能。

审查覆盖八个 RVV 头文件、主要接入点（`cb_search.c`、`filters.c`、`lsp.c`、`ltp.c`、`vq.c`）、构建配置、七个差分测试、A/B 驱动、设备脚本、基准和剖析器；并检查 LPC/LSP 搜索与量化的后续机会。以下区分三类证据：

- **已复现**：从当前源码重新构建，在本机或 QEMU 观察到失败。
- **静态确认**：由源码和标准语义直接确定；没有把它写成已完成的动态检测。
- **优化假设**：存在可解释的开销，必须由正确性测试、汇编和目标机配对测量决定是否保留。

优先级 P1 表示应在下一轮性能结论或发布前解决；P2 表示边界、构建、证据或可维护性问题。没有据此认定线上 PCM 必然触发每个人工边界用例。

## 2. 以 RISC-V 官方文档为准的规则

本轮读取的是 RISC-V 官方在线 ISA 文档和官方 C Intrinsics v1.0 文档。官方定义指令与接口语义，**不保证某条指令在 X60 上更快**。社区优化建议和项目注释不作为标准依据。

| 官方依据 | 标准含义 | 本项目的直接要求 |
|---|---|---|
| [ISA：尾部与掩码策略](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#sec-agnostic) | agnostic 元素可以保留旧值，也可以被写为全 1；可以混合，不能依赖固定行为 | 初始化过整个向量，不代表后续短 VL 操作会保留尾部 |
| [Intrinsics：策略命名](https://docs.riscv.org/reference/vector-c-intrinsics/v1.0/rvv-intrinsic-spec.html#policy-and-masked-naming-scheme)、[策略控制](https://docs.riscv.org/reference/vector-c-intrinsics/v1.0/rvv-intrinsic-spec.html#control-of-policy) | 无策略后缀的相关接口默认 tail-agnostic；`_tu` 等后缀显式指定保留行为 | 持久累加器、候选索引、零填充状态需要逐条检查，而非只检查 load |
| [Intrinsics：agnostic 值](https://docs.riscv.org/reference/vector-c-intrinsics/v1.0/rvv-intrinsic-spec.html#agnostic-value-in-the-rvv-c-intrinsics) | agnostic 值是 indeterminate，求值会构成未定义行为 | 把短 VL 结果随后按 VLMAX reduce/store，属于源代码正确性问题 |
| [ISA：VL 约束](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#constraints-on-setting-vl) | 当 VLMAX < AVL < 2×VLMAX，VL 不必等于 VLMAX | 使用实际返回 VL；不可默认所有非最后一块都满宽 |
| [Intrinsics：reduction](https://docs.riscv.org/reference/vector-c-intrinsics/v1.0/rvv-intrinsic-spec.html#reduction-instructions) | 相关参数顺序是向量 `vs2`、标量载体 `vs1`、VL | “GCC 与官方 reduction 顺序相反”的注释不成立 |
| [ISA：vmerge](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#30-1-11-15-vector-integer-merge-instructions) | `vmerge vd, vs2, vs1, mask` 在 mask 为真时选第二个源 | 项目当前 false/true 用法符合标准，不应称为 GCC 13 特例 |
| [ISA：segment/AoS](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#sec-aos) | segment load 可拆交错数据，且 EMUL×NFIELDS ≤8 | 可试 `vlseg4e16` 替代四次跨步 load；合法性和速度分开验证 |
| [ISA：vslide1down](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#30-1-16-3-5-vector-slide-1-down-instruction) | 将标量插入当前 VL 的最后一个活动元素 | 滤波状态可在 VL=ord 下移位并补零，无需维持整个 VLMAX 的零尾 |
| [ISA：vslidedown](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#30-1-16-3-2-vector-slide-down-instructions)、[vrgather](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html#30-1-16-4-vector-register-gather-instructions) | 两者都有越过 VLMAX 取零的规则；gather 可读到 VL 之外但 VLMAX 之内的元素 | gather 不会替程序修复已被污染的“零尾”；ord<VLMAX 时，读 ord 位置也不是越过 VLMAX |
| [Intrinsics：配置管理](https://docs.riscv.org/reference/vector-c-intrinsics/v1.0/rvv-intrinsic-spec.html#bookkeeping-of-configurations)、[版本宏](https://docs.riscv.org/reference/vector-c-intrinsics/v1.0/rvv-intrinsic-spec.html#test-macro) | 编译器管理 vtype/VL；intrinsics API 版本与 ISA 版本不同 | 看实际汇编中的配置切换；分别记录 `__riscv_v` 和 `__riscv_v_intrinsic` |

实际 GCC 13.3 环境报告 `__riscv_v=1000000`，但 `__riscv_v_intrinsic=11000`（0.11 API）。因此 `rvv_arch.h:12` 关于“ratified 1.0 intrinsics ABI”和 GCC/Clang 最低版本的泛化声明也应重写，不能把 ISA 1.0 当作 Intrinsics API 1.0 的证明。本文用 v1.0 官方文档校核接口语义；当前旧版编译器的实际生成代码另外核验。

## 3. 审查发现与优先级

### R1 · P1 · 点积与平方和把 agnostic 尾部参加归约【已复现】

位置：[math_rvv.h:51](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/math_rvv.h:51)（inner_prod 更新，57 行归约）、106–141 行（batch5）、291–328 行（pitch_xcorr）；[filters_rvv.h:249](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/filters_rvv.h:249)（sumsq）。

代码先按 VLMAX 清零累加器，随后按动态 VL 用无 `_tu` 的 add 更新，最后又按 VLMAX 做 reduction。短 VL 更新允许破坏之前的尾元素；第一次更新就是短块时，同样会破坏本应为零的未用 lane。全宽初值不能建立跨操作的不变式。

VLEN=256 的最小例子：`len=40`，x/y 每个元素都是 8，标量每四个乘积和右移 6 得 4，总和为 40。普通 QEMU 得 40，允许尾部写全 1 的模式得 **34**。

计划修复：持久累加器采用正确 passthrough 的 tail-undisturbed 操作，并定义归约范围和零长度语义；或比较每块归约方案的成本。只把最终 reduction 的 VL 改成最后一块 VL 会丢掉先前 lane 的累积，不能作为修复。

### R2 · P1 · FIR/IIR/filter 的零填充状态并未得到保证【已复现】

位置：[filters_rvv.h:92](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/filters_rvv.h:92) 及 129–143、166–185 行。

`vslideup(zero, load(...,ord),0,ord)` 没有 `_tu`，不会保证 `[ord,VLMAX)` 为零。后续却用 VLMAX 做 MAC/gather，污染从高 lane 逐样本进入有效滤波状态。该错误与输入是否接近饱和值无关。

最小例子：VLEN=256，FIR `ord=10,N=32,num=0,mem=0,x[i]=12000`。输出应始终等于输入且状态保持零。普通模式通过，全 1 尾模式第一次错误出现在输出索引 11，得到 11999；输出与状态合计 31 处不符。

计划先明确保留尾部的最小修复；性能实验再比较 VL=ord 的 `vslide1down(...,0,ord)`，消除零尾依赖。不能仅换 gather 为普通 slide 就假定 ord 位置自动补零。

### R3 · P1 · VQ / 开环基音的候选尾部可能产生非法索引【已复现】

位置：[vq_rvv.h:105](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/vq_rvv.h:105) 及 182–198 行；[ltp_rvv.h:199](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/ltp_rvv.h:199)。

候选值、候选索引用短 VL 的无 `_tu` merge 更新，随后按 VLMAX 存出并标量选优。未活动 lane 可以变成全 1，成为虚假最小值/候选。VQ sign 路径再用 `codebook+bi` 重算符号，错误索引会扩大为越界访问；open_loop 也有用选中索引访问 num/gain 的后续路径。

QEMU 合法全 1 尾模式下，`vqtail_diff` 在 VLEN128/256/512 均以 SIGSEGV（139）结束，open_loop 大量失配。这个结果不能解释为“目标板恰好没有此行为所以代码正确”。

计划修复：分别保证值、索引及其有效性在短块之间保留；无候选 lane 显式 invalid；归并时保留标量 first-wins 与 N>1 的稳定顺序。把 `_tu` 放到一个不相关临时向量上不能解决候选状态污染。

### R4 · P1 · `gshelp_diff` 在分配区之前读取历史数据【静态确认】

位置：[gshelp_diff.c:50](/Users/tony/work/libspeex-rvv/test/gshelp_diff.c:50) 的分配、63–64 行的初始化，以及 105–113 行的访问。

`exc2=malloc(hist*sizeof(...))` 指向分配起点，但标量参考读取 `exc2[j-pp]`，第一次 j=0、pp≥16 就越界；向量也从 `exc2-pp` 读取。注释说有历史窗口，实际没有把该指针移到历史区末端。两个实现读到同一片非法内存，仍可能比较 PASS。

因此 8400 次比较中的 exc-build 部分不能作为正确性证据；new-target 部分应单独评价。本轮没有运行 sanitizer 来定位它，越界结论来自确定的索引关系。不能由测试程序越界直接推断生产 helper 也同样越界。

计划修复：区分 allocation base 和历史终点指针，初始化真实历史区域、释放 base；加红区/guard-page 或可用的地址检测。先让测试本身合法，再讨论被测内核是否通过。

### R5 · P1 · CPU 探针实际没有执行声称的操作数【汇编已确认】

位置：[cpu_probe.c:70](/Users/tony/work/libspeex-rvv/test/cpu_probe.c:70) 及 128–168、263–275 行。

- “4 独立流”最终只消费 a0/a1，a2/a3 被删除；“8 独立流”只消费 a0/a7，其余六条被删除。
- “load+MAC 4 流”只消费 a0，生成的循环只有一组向量 load+MAC。
- unit-stride 对同一地址反复 load，被提升到计时循环之外；strided 地址变化，load 留在循环内。两者比值不是单纯 load 指令成本比。
- 标量探针乘常数 3，并未证明生成了标量乘法指令，且仅消费部分结果；“标量 MAC 吞吐”比较没有同等工作量依据。
- 计时器校准后再乘假定 1.6GHz，把 time ticks 换算成 cycles；这不是实际 core cycles 的测量。

用探针自己的 `-O2 -march=rv64gcv` 生成汇编，`.L8/.L9` 都只有两条 vmacc，`.L11` 只有一组 load+vmacc，`vle16 v28` 在 `.L12` 前。证据：`build/review/logs/cpu-probe-O2.s`。

所以“需 ≥8 独立流才能达到 1 vec-op/cycle”“跨步 load 慢 8 倍”“向量潜力约 40 倍”等结论应撤回并重测。**这不推翻已经实现的列主序优化，只撤回其硬件解释和上限推断。**

### R6 · P2 · VQ 最大距离与哨兵相等时不接纳第一个条目【已复现】

位置：[vq_rvv.h:81](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/vq_rvv.h:81)；sign 版本同类初始化/严格比较。

标量参考会强制接纳第一个条目；向量仅用 `dist<INT32_MAX` 更新，实值等于 INT32_MAX 时索引留在 UINT32_MAX。后面的按索引打破平局无法恢复从未保存的候选。注释所说“真实最大距离会赢哨兵”与代码不符。

不依赖有符号加法溢出的复现输入：`len=2,entries=1,N=1,in={-32768,-32768},codebook={32767,1},E={2147483646}`。raw=-1073741824，E/2-raw=2147483647。普通 QEMU 下预期 `(idx=0,dist=2147483647)`，实际 `(-1,2147483647)`。

这是内核参数边界反例，尚未证明正常编解码数据能形成该 E/input 组合。计划用有效位或“索引未初始化时强制接纳”处理，保留后续严格比较与稳定 tie-break。

### R7 · P1 · 开环基音扫描漏掉 -32768，改变缩放与恢复【已复现】

位置：[ltp_rvv.h:59](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/ltp_rvv.h:59)；对照 [arch.h:80](/Users/tony/work/libspeex-rvv/src/libspeex/arch.h:80) 的 ABS16。

标量 ABS16 宏先做 C 整数提升，`ABS16((int16_t)-32768)` 得到 int 32768；向量在 i16 中取负仍是 -32768，max scan 可能漏掉唯一越过阈值的样本。注释把 ABS16 当成 NEG16 的窄位宽回绕，推理错误。

复现：304 个样本初始化为 101，sw 指向第 144 个样本，`sw[-144]=-32768`，`start=17,end=144,len=160,N=1,gain=NULL`。标量应触发缩小再恢复，使 `sw[0]` 从 101 变 100；向量漏检，仍为 **101**。这个输入变化本身已经是可观察差异，无需用随机溢出的相关值作为证据。

计划直接检查 `(x>16383)||(x<-16383)` 的 mask 并归约是否存在，或先拓宽再 abs；增加 INT16_MIN 确定性测试。

### R8 · P2 · 两种公开构建组合失败【已复现】

1. macOS arm64，默认 native 全目标构建，PROF=OFF：`benchmark/bench_e2e.c:2` 的 weak 数据符号引用导致 `_speex_prof_enabled` 链接失败。本轮 native A/B 是单独构建 `speex_ab` 才完成的，不能称默认全构建通过。证据：`build/review-native-ab.log`。
2. 交叉构建 `FIXED_POINT=OFF,_USE_RVV=ON,RVV_KERNELS=ON`：helper 定义在 FIXED_POINT 内，`filters.c:463,506,...` 与 `ltp.c:518,531` 的调用只检查 `_USE_RVV`，链接出现 `rvv_gain_build_exc` 等未定义符号。证据：`build/review/logs/float-build.txt`。

计划统一“此内核确实可用”的能力宏，浮点保留合法 C 路径；剖析器调用采用可移植的构建期接口。对不支持的组合，配置阶段明确报错也优于编译后期隐式声明/链接失败，但需明确项目支持范围。另将 DISABLE_ENCODER / DISABLE_WIDEBAND 等裁剪加入编译矩阵；本轮没有宣称这些组合已全部失败或通过。

### R9 · P2 · target_update override 默认没有启用【静态确认】

位置：[cb_search_rvv.h:87](/Users/tony/work/libspeex-rvv/src/libspeex/rvv/cb_search_rvv.h:87)，以及 `cb_search.c:93`。

条件要求 `defined(OVERRIDE_TARGET_UPDATE)`，但该宏仅在条件内部定义；默认构建没有外部定义，整个实现被排除。项目“20 个 override”、第 4 轮包含此优化的叙述不能代替实际预处理结果。

计划修正 guard，并以预处理/调用点反汇编核验是否启用。启用后是一个待验收的新行为，尤其需要覆盖高 complexity 和 N>1；不能直接计入已有收益。

### R10 · P2 · 回归和性能流程缺乏可追溯性【静态确认】

- `test/CMakeLists.txt` 只构建 A/B 驱动和三个上游 testenc，没有注册七个差分测试，也没有 CTest 门禁；设备脚本没有构建/运行这些差分测试。
- `device-test.sh:78–80` 的 ab 重新传的是已有二进制，没有保证先重建或关联源码 hash；bench 则既不重建也不部署。结果可能来自不同版本的产物。
- 两个 A/B 脚本默认只覆盖 q2/q8、2 秒合成信号，未覆盖高 complexity、VBR、DTX/VAD、PLC、reset 等。脚本注释声称双向交叉解码，实际只显式跑 aa/bb/ab，缺 ba；在码流完全相同时 ba 往往冗余，但报告应与实现一致。
- `speex_ab.c:154–164` 对不完整帧头/帧体直接结束并返回成功，且未检查解码返回值；非法输入/空输出可能形成误导比较。后续必须同时核对帧数、长度和 API 错误。
- `bench_e2e` 用最小值，设备脚本固定 A→B、未绑核，也没有配对区间估计。脚本还会 best-effort 修改 governor，不能把“同会话”当作环境已经完全一致。
- `prof.c` 明确统计 inclusive time；父子耗时不能相加得到 100% 的预算。运行时 gate 能限制归属，但退出 hook 在检查 gate 前仍读时间；它不能消除插桩开销，细小函数占比仍可能失真。
- 当前 Release 实际库编译参数为 **`-O3 -DNDEBUG -std=gnu99 -march=rv64gcv`**，不是历史说明中的统一 O2。差分驱动和 CPU 探针的手动编译则使用 O2，应分别记录。

### R11 · P2 · baseline 不能称作未经修改的官方 1.2.1 发布版【已比对】

将 baseline 的 87 个 C/H 与 [Xiph 官方 Speex 1.2.1 发布包](https://downloads.xiph.org/releases/speex/speex-1.2.1.tar.gz) 对比。压缩包 SHA256：`4b44d4f2b38a370a2d98a78329fefc56a0cf93d1c1be70029217baae6628feea`。仅提取内容比对，没有构建执行该发布包。

差异不只有格式：baseline `nb_celp.c:515–516` 的带宽扩展使用 QCONST16，而发布包对应位置使用浮点常量；baseline `sb_celp.c:1377–1378` 对 filter_ratio 加了 EPSILON 下限；还有质量范围与初始化修正。无法仅凭这些差异确定是项目自改还是来自后续上游提交，但足以否定“字面等同发布包”。

计划冻结现有 baseline 作为**当前行为参照**，补齐来源 commit / release hash 和补丁清单；如果产品要求兼容官方发布版，另建该参照的独立比较。不能直接用发布包覆盖 baseline，把既有修复当作优化回归来回滚。

### R12 · P2 · “全 int16 域位精确”的 C 算术依据不足【静态确认】

`fixed_generic.h:63–73` 中 ADD32/SUB32/MAC16_16 等使用普通有符号 C 运算。项目 RVV 注释把它们统一解释为模 2^32 回绕，但 signed overflow 在 C 中并无这一保证；部分左移和 INT32_MIN 取负也需要逐项定义。此问题可能继承自上游，不能全归因于 RVV 改写。

计划把参考算术契约写清：用 uint32/更宽中间值在规定位置截断的独立 oracle，或明确并固定两边一致的编译器扩展/选项（例如有意采用 `-fwrapv`，仍需另查移位语义）。实际合法输入域与全范围 fuzz 分开。不得把发生 UB 的随机参考输出当作数学证明。

特别注意：inner_prod 的语义是**每四项的 32 位和先截断/回绕，再算术右移 6，再累加**；不是整个长点积用 int64 求和后统一右移。QMF 现有系数取负再窄化的边界也必须按选定参考保留，不能凭“更精确”改变结果。

## 4. 本轮已经取得的独立验证结果

### 4.1 环境

- 离线现有镜像 `speex-rvv-buildenv`，image ID `8353d9eb2ef865b22005b7dd40e3064c517f32c7bebad1dc84422f64ea4910fb`；无网络容器内交叉构建。
- GCC 13.3.0，QEMU user 8.2.2；当前源码重建，库为 Release/O3、定点、RVV、PROF=OFF，静态链接。
- 普通模式和全 1 模式使用同一组程序；后者设置 `rvv_ta_all_1s=true,rvv_ma_all_1s=true`，不是修改程序来人为制造错误。
- 全 1 是 ISA 允许的行为，用来检验可移植正确性。它不是对 X60 当前尾部实现的描述。TAIL/MASK 两种策略本轮同时打开，后续门禁应分别打开以区分问题。

### 4.2 七组差分测试

普通尾部模式，VLEN=128/256/512 下七组均报告 PASS；但 R4 的无效内存访问与 R12 的算术问题使这些 PASS 的证明力受限。合法全 1 模式得到：

| 测试 | VLEN128 | VLEN256 | VLEN512 |
|---|---:|---:|---:|
| inner_prod | 52000/80000 失配 | 60000/80000 | 64000/80000 |
| batch5 | 21000/27000 失配 | 27000/27000 | 27000/27000 |
| vqtail | SIGSEGV / 139 | SIGSEGV / 139 | SIGSEGV / 139 |
| open_loop | 7763/14000 失配 | 5477/14000 | 5556/14000 |
| gainvq | 0/2000 | 0/2000 | 0/2000 |
| gshelp | 报告 0/8400；exc-build 部分证据无效 | 同左 | 同左 |
| qmf | 0/240000 | 0/240000 | 0/240000 |

不同测试计数单位并不相同，例如 QMF 包含输出元素/隔离检查，不能将所有数字简单汇总为同类独立样本或完整覆盖率。

### 4.3 扩展端到端 A/B

对同一 2 秒合成输入，检查 nb/wb/uwb×q0..10，以及 aa/bb/ab/ba 四种解码组合：

| 环境 | 结果 | 可支持的结论 |
|---|---|---|
| native arm64，单独构建 speex_ab | 33/33 PASS | 该输入与配置的 C 路径相同；不检验 RVV |
| QEMU VLEN128，普通尾模式 | 33/33 PASS | 所测输入/默认控制参数匹配 |
| QEMU VLEN256，普通尾模式 | 33/33 PASS | 同上 |
| QEMU VLEN256，全 1 尾模式 | 首个 nb q0 就失败，码流第 7 字节不同 | 错误可影响完整编码流程，不仅是孤立微测试 |

不扩大为所有语音、复杂度与控制模式通过；没有据此给出新性能数据。

### 4.4 证据位置与复查方式

本轮本地原始证据位于 `build/review/`（被 git 忽略，属于临时审查产物，不随本文自动进入版本历史）：

- `run-cross.sh`：重建、七组差分与 VLEN/尾策略矩阵；`e2e.sh`：33 组和四种解码组合。
- `contract-probe.c`：R1/R2/R6/R7 的最小输入；结果已记录在本文相应发现中。
- `logs/*-v*-poison*.txt`：逐测试结果；vqtail 崩溃日志为空，退出 139 来自运行器。
- `logs/*expanded-ab.txt`、`logs/qemu-poison-ab.txt`：端到端结果。
- `logs/cpu-probe-O2.s`、`logs/ltp-disasm.txt`、`logs/filters-disasm.txt`：探针被删除的操作及内核代码生成。
- `logs/upstream-check.json`、`logs/upstream-*.diff`：发布版对比。
- `logs/float-build.txt`；另有 `build/review-native-ab.log`：构建失败。

下列仅是供后续执行者使用的复查命令示例，本文没有在用户要求“只做计划”后执行它们：

```sh
# 前提：已有同工具链重建的静态测试程序；QEMU 需支持这两个选项。
qemu-riscv64 -cpu rv64,v=true,vlen=256,elen=64,vext_spec=v1.0,rvv_ta_all_1s=true,rvv_ma_all_1s=true build/review/bin/inner_prod_diff
qemu-riscv64 -cpu rv64,v=true,vlen=256,elen=64,vext_spec=v1.0,rvv_ta_all_1s=true,rvv_ma_all_1s=true build/review/bin/vqtail_diff
# 核验探针：检查计时循环内实际保留的 MAC/load 数量。
riscv64-linux-gnu-gcc -O2 -march=rv64gcv -S test/cpu_probe.c -o /tmp/speex-cpu-probe-review.s
```

## 5. 后续工作分期与交付物

以下为一个熟悉定点 DSP/RVV 的工程师的粗略人日估算，作为排期预算，不是收益承诺。测试框架与现有工程兼容程度会影响工期。**第一批应立刻安排的是 M0、M1；M2 的排序在可靠归因之后调整。**

| 阶段 | 工作与交付 | 依赖 | 估算 | 放行条件 |
|---|---|---|---:|---|
| M0a | 冻结参照来源、算术契约、失败用例；修复 gshelp 测试 | 无 | 1–2 人日 | 参考与测试不再靠越界/未定义尾值工作 |
| M0b | 单独补丁修复 R1/R2/R3/R6/R7；构建 guard、target_update、native benchmark 问题 | M0a | 2–4 人日 | 定向反例通过，尾策略矩阵与状态检查通过，支持的配置可构建 |
| M1 | CTest/回归矩阵、产物 manifest、正确探针、配对基准与归因 | M0 | 2–3 人日 | 可从干净 checkout 重现结果，产物能关联 commit/flags/input |
| M2a | 滤波状态、QMF、点积/相关三类实验，各自独立对照 | M1 | 各 2–3 人日 | 正确性全通过，汇编符合假设，目标机收益有区间支持 |
| M2b | 码本布局/VQ、LSP、小函数分派；只做预算支持的项 | M1 的归因 | 2–5 人日总预算 | 阶段收益足以覆盖代码、表尺寸与维护成本 |
| M3 | 工具链/旗标与可选目标分派、资源与完整端到端验收 | 已保留的 M2 项 | 2–3 人日 | 相同语义、可重复性能、无关键模式回退，有回退方案 |

建议首轮总体预算约 **3–5 周**。每类内核先给 0.5–1 人日验证主要假设，证据不支持则停止该方向，不为用完预算继续改写。M0 修复的性能影响必须如实计入新基线，不能以保留错误换速度。

补丁建议分为：测试修复/确定性反例、tail 正确性、数值边界、构建与开关、测量设施、每个独立内核实验。每个性能补丁都与“上一版正确实现”比较；另报告相对原始 HEAD 和已冻结 C 参考的结果。这样能分辨修复代价和后续优化收益。

## 6. M0/M1：先建立可信的验证体系

### 6.1 内核正确性矩阵

1. 将现有七组差分测试注册到 CTest；先修 R4，再纳入门禁。增加 FIR/IIR/filter、compute_weighted_codebook、target_update、interp_pitch、lsp_to_lpc、normalize16 的直接测试，避免仅靠完整编解码间接覆盖。
2. QEMU 至少覆盖 VLEN=128/256/512。尾部与掩码组合：默认、TA 全 1、MA 全 1、两者全 1；工具可用时再扩到 1024。模拟器不能枚举所有合法 VL 分块，代码还需依据官方 VL 约束逐循环证明正确。
3. 对每个 SEW/LMUL，长度覆盖 0、1、VLMAX−1/VLMAX/VLMAX+1、2×VLMAX−1/+1、多完整块+短尾；块点积另覆盖 len%4=0/1/2/3。只对声明支持的参数域断言值；负长度、非法 order 等要明确属于拒绝还是调用前置条件，避免无意扩张接口。
4. 定向值覆盖 0、±1、INT16_MIN/MAX、INT32_MIN/MAX、舍入阈值、饱和阈值、相等距离/相等比例、所有距离等于哨兵、entries<VLMAX、N=1 和生产 N>1。先验证 oracle 没有发生未定义运算，再进行全范围 fuzz。
5. 有状态内核不仅比输出，还比完整有效 mem；一次调用与分割为多次调用结果应相同。检查 x==y、非别名、历史窗口、短帧、redzone；只测试接口允许的别名关系。
6. 标量参考、手工 RVV 和自动向量化版本必须独立可选。记录并检查实际启用的 override，不以宏数量等同于执行覆盖。

验收：所有定向反例通过；全矩阵零不符、零越界；明确记录继承的算术约束和仍未覆盖的参数域。随机样本数量作为补充，不能替代边界和不变式证明。

### 6.2 端到端与工作负载

先定义产品真正关心的权重：模式、quality、complexity、编码/解码占比、流数、目标时延。没有这些权重时，报告每个模式独立结果，不把某个 wb q8 最优值当作总体收益。

基础回归覆盖 nb/wb/uwb×q0..10，complexity 取项目支持范围内的 0/2/5/10（检查每次 ctl 返回值），使 N>1 的搜索实际被调用。对 VBR、VAD/DTX、enhancer 开关、PLC、reset/连续流运行做组合与序列测试；过大的笛卡尔积可分为每次提交的核心集和夜间完整集，不能悄悄跳过高 complexity。

语料包含真实语音（多说话人/有噪声）、静音、DC、脉冲、满幅与 INT16_MIN 交替、长连续流；合成 chirp 继续保留但仅是其中一种。保存语料许可来源与 hash。交叉解码明确输出四种组合；核对非零期望帧数、码流长度、PCM 长度和全部 API/I/O 返回值；截断/损坏输入单独要求驱动报错，避免空结果 PASS。

### 6.3 参照组与产物 manifest

至少保留三组，从相同的已冻结语义参考构建，用来区分改写收益和编译器自身收益：

| 组别 | 配置含义 | 用途 |
|---|---|---|
| S | 标量 C，RV64GC，关闭 loop/SLP 自动向量化 | 给出明确标量参照，核验汇编中没有 V 指令 |
| A | 相同 C，RV64GCV，允许自动向量化，无手工 RVV | 真实编译器参照 |
| R | 相同公共选项，RV64GCV，手工 RVV | 判断手工内核的增量价值 |

`RVV_KERNELS=OFF` 只是关闭手工 override，并不等于关闭自动向量化。R/A 对比优先回答“手工改写值不值得”；S/A/R 三者一起才能合理解释加速来自哪里。另保留当前 HEAD 作为历史对照，不能混用不同树与 flags 却只写一个 baseline。

每次测量保存：源码 commit 和 dirty diff、参照来源/补丁、compiler/binutils/QEMU 版本、CMake cache 和完整编译命令、宏与 ISA/API 版本、二进制及输入 SHA256、链接库、实际 override 清单、CPU/内核/可见频率/亲和性/温度（可读取时）、运行顺序、原始样本、汇总脚本版本。部署前后核验同一二进制 hash。clean build、ab、bench 必须共享并核验 manifest，发现过期直接失败。

### 6.4 基准与剖析改进

- 正式时间数据使用 PROF=OFF；初始化、I/O、码流生成和测量区分开。明确是冷启动延迟还是稳态吞吐，重复前重置状态，避免 A/B 编解码器经历不同历史。
- 在获准的目标机实验中固定相同 core/亲和性，记录可见频率和温度。先预热，采用随机 A/B 或 AB/BA 配对，至少 20–30 对独立重复；使用成对时间比的中位数与 bootstrap 95% 区间，另外报告 p95/p99 帧时延。运行仍有漂移就增加时长/配对，而不是只挑最小值。
- 设备已有桌面/服务属于实际环境的一部分；可另设受控实验环境，但必须分别标注，不能通过不声明的服务停用制造漂亮数值。本次计划不包含自动停桌面、改 governor 或部署动作。
- 热点归因优先使用目标环境实际支持的采样工具；不支持时，对少量顶层阶段计时并用逐内核消融交叉验证。inclusive 与 exclusive 明确区分，估算并标注 hook 成本；不要从大量插桩小函数的百分比直接推出剩余性能上限。
- 修 CPU 探针时消费**全部 lane/全部累加流**，将必要的归约/输出放在计时区外；检查机器码确认所有预期运算仍存在。构造等工作量、等地址变化/缓存条件的 load 对照，阻止不希望的提取，但不要用额外屏障把待测吞吐本身扭曲。
- 对 1/2/4/8 累加链、不同 LMUL、长/短 VL、stride 和工作集分开测。统一单位：ns、time ticks、元素/秒；只有可靠可用的 cycle counter 或已验证频率条件下才报告 cycles。明确“每组”有几条向量指令。

## 7. M2：按源码可见开销设计优化实验

以下优先序来自可见浪费和可验证性，最终投入依据 M1 的无插桩阶段占比调整。每项只接受通过 M0/M1 门禁的候选；不得以关闭边界测试换取吞吐。

### K1 · FIR / IIR / filter：让滤波状态留在寄存器

**现状与假设。** `filters_rvv.h:102,139,178` 每个样本都将 ord 个 32 位状态写回 mem，只为标量读取 mem[0]；随后用 gather 做向量移位，并在整个 VLMAX 上执行。ord=10 时每样本约写 40 字节状态，最终还再次存出。减少这种往返比凭猜测增加 MAC 链更值得先试。

**候选设计。** 有效 VL 固定为 ord，系数和状态只加载有效元素；用 `vmv.x.s` 取 state[0]，用 `vslide1down` 插入 0，按原顺序 MAC，循环结束仅存一次状态。保留现有饱和、PSHR、窄化和 NEG16 行为。输入输出允许重叠时，在写 y 前保存当前 xi，不能让 IIR/filter 再读取已覆盖的 x[i]。

**向量宽度。** VLEN128 的 e32m2 只有 8 个 lane，当前 ord=10 会回退标量；对其试 e16m2/e32m4 或维持标量，不能默认为所有目标都采用 VLEN256。选择由实际 order、VLMAX 和测量决定，保留通用回退。IIR 的时间反馈仍顺序执行，不能简单把连续输出样本并行后宣称保持位精确。

**检查与验收。** ord=8/10、多个 VL，N=0/1/短/完整帧、x==y、非零 mem、分段调用、全 1 尾策略。看汇编是否消除每样本 state store/load、gather 索引和冗余 vset；测该函数及编码/解码阶段。若 `_tu` 最小正确修复更快，就保留它。

### K2 · QMF synth：把运行时调度解释器还原为固定计算

**现状与假设。** `filters_rvv.h:405–463` 每次调用建立五个 `int[4*M]` 调度数组，执行时逐 term 读元数据、选择源、选择 accumulator。M=64 时光这些数组就是 5120 字节；N=320 时四个 short scratch 合计另有 1536 字节，约 6.5KiB，还未计其他局部量。`xx→yyp` 两次整理、重复窗口加载和四次 strided store 都是明确的可研究成本。

**候选设计。** 推导四 tap 的固定循环或小范围展开，直接表达四个输出累加器与共享窗口；去掉每次构建/解释 schedule。尝试仅一次数据整理、复用相同窗口，比较四个跨步 store 与 `vsseg4e16`（满足 EMUL×NFIELDS≤8）。如使用常量调度表，应优先编译期生成并只读，不引入全局可写缓存或线程竞态。

**约束。** 保留参考的每个乘积符号、系数窄化、回绕、最终 PSHR/饱和和 mem 更新。不能直接把注释里的和差公式当成已证明等价，尤其系数 -32768 的取负窄化已经是参考行为。对未支持的 N/M 形状保留真实 C fallback，不让 helper 的提前 return 变成无输出。

**检查与验收。** 扩展现有逐 tap 隔离测试，检查全输出与 mem；覆盖实际 M=64,N=320/640、现有 N=160/M=24 用例、短块和边界。测每次 setup、热循环、总函数、峰值栈/代码尺寸、wb/uwb 解码收益。segment 指令只作为候选，不根据 ISA 文档断言它更快。

### K3 · inner_prod / batch5 / pitch_xcorr：降低活跃寄存器和归约成本

**现状与假设。** block-parallel 组织每个 lane 算四项，使用多次 stride=8 的 e16 load，batch5 与四 lag 相关保持较多活跃值。当前 GCC 13/O3 的 `ltp-disasm.txt` 已能看到 spill；pitch_xcorr 在约 0x744/0x762 的循环内有整组向量 reload/store。先证明具体热点，再衡量减少 spill 与共享加载的净收益。

**第一组实验。** 四次跨步 load 对照 `vlseg4e16`；不同输入流依次消费，缩短生命周期；batch5 对照 3+2、逐点积，LMUL1/2 对照。不是所有输入都应同时 load 后再算；更大批次可能抵消共享收益。短 len 的标量切换阈值由结果决定。

**第二组实验：pitch 的 lane=lag。** 对一个 lag tile，每 lane 代表一个 `_y+i` 的候选；遍历四采样 block，广播 x[4b+k]，连续加载该 k 在不同 lag 下的 y，四次 widening MAC 形成 part，按参考右移 6，再更新每 lag 总和。可消除横向 reduction，并使 y 在 lag 方向连续。结果仍按现有 `corr[nb_pitch-1-i]` 的顺序写出，保持 pitch tie-break。

**约束与验收。** 每四项先回绕/右移的边界不能变；不允许整个长点积最后移位。比较 len=40/80/160 与实际其他长度、nb_pitch 小值（如 7）及约 128 的主路径；检查尾部保留、合法的 VL 分块、数组边界和 reverse store。记录热循环 load/MAC、spill、vset 与 reduction 数量，再测 X60；不能由指令数量直接换算速度。

### K4 · weighted codebook / VQ：把布局变化的成本和收益一起算

**现状与假设。** 目前 resp/respT 双布局有利于不同消费者，但静态 shape 码本仍以跨步方式加载并在每次调用准备向量 scratch。project 对“8 倍 load 差距”的解释无效，布局优化本身仍值得保留并重新评估。

**候选设计。** 对固定只读 shape 表，编译期生成转置的 int8 表或按测量选择 int16 表，去掉运行时反复 gather/拓宽准备；响应 resp/respT 是否都需要，按 N=1/N>1 实际消费者决定。比较 2/4/8 分链和小范围展开，但只在修正后的探针及汇编显示延迟/寄存器预算支持时采用。

**搜索尾部。** N=1 的稳定 argmin 和 sign 保持严格标量语义；N>1 优先对照“向量距离+标量稳定 top-N”，避免为极少出现的路径引入复杂掩码排序。修正 target_update 开关后独立测它在高 complexity 的真实调用收益。

**验收。** 直接比 resp、respT、E、距离、索引和符号；多流独立状态与线程安全；表尺寸、scratch、缓存命中和 setup 成本一起统计。若额外 ROM/双写让端到端退化，即使距离循环更快也应回退。

### K5 · LSP/LPC 与剩余小函数：根据占比决定是否继续

`lsp_to_lpc` 在 order=10 时有大量仅 2..8 元素的阶段，指令配置/搬运可能占主要成本。对照标量按 order=8/10 特化展开、P/Q 独立链调度和现有 RVV；保持 `MULT16_32_Q14` 的高低位分拆与分别截断，不能用一个“更准确”的宽乘替换。若小阶段标量更快，可以只向量化收益明确的部分。

`quant_lsp.c` 的固定码本搜索可尝试 lane=entry，按 dim=5/10 累加并稳定选优；带权变体同样检查窄化和乘法拆分。历史“<3%”必须由新归因确认，若阶段占比很小，则仅给约半天原型预算。

`lpc_to_lsp` 的自适应根搜索具有数据依赖，不宜改变网格、分支或二分次数来追求位精确优化；但这不等于“所有优化数学上无解”。先考察 degree=4/5 特化、内联、公共常量和重复计算消除、P/Q 独立计算调度。推测式批量求值只有在新剖析显示占比高且能严格保持选择次序时才做小实验。

interp_pitch、normalize16、gain helper 等小内核优先完善确定性测试与实际开销归因；避免为了“全 RVV”让短向量路径变慢。

## 8. M3：工具链、分派与资源预算

- **先可移植 VLA，再可选特化。** 通用实现不依赖固定 VLEN。仅当部署范围确实需要时，增加有明确编译/运行条件的 X60/VLEN256 快路径并保留回退；检测可用扩展/向量长度应使用目标系统可用的 hwprobe/getauxval/合法的向量查询机制，不能在不支持 V 的系统先执行 vlenb CSR。单一已知目标镜像可以选择构建期分派，避免不必要的运行时复杂度。
- **分开 API 与 ISA。** GCC 13 的旧 intrinsics 与支持标准 v1.0 API 的工具链分别建适配与 CI；选择版本依据实际宏和能力检查，重写“gcc/clang 语义反转”说明。对至少一个可用的新 GCC/Clang 版本做编译与正确性对照，不能只凭版本号假定兼容。
- **旗标单独做实验。** O2/O3、LTO、小范围内联/展开、实际支持的 `-mtune` 分别与同 flags 的参照比较。记录代码尺寸、栈与 spill；对编译器不认识的 target/tune 不猜名字，也不添加未经目标能力确认的扩展。已经做过的旗标阴性结果，只有配置和证据可复现才有排除价值。
- **资源与功能一起验收。** 除 µs/帧，还记录峰值栈、只读表和 text 尺寸、初始化成本、单帧 p99；嵌入式环境应先约定允许的资源上限。新增全局可写 scratch、跨状态缓存和隐藏静态状态需要避免。
- **多流并行放在后续产品层。** 独立 codec state 可以流级并行，但 8 核不保证 ×8：要测频率、缓存、带宽与任务调度，单流反馈依赖也不能靠线程自然消失。本轮优先解决单线程正确性与可见浪费；有明确多流需求后再安排专门实验。

## 9. 收益判断、停止条件与最终验收

对每项实验，先从可靠数据得到其在目标阶段的独占占比 p。若该部分局部加速 s 倍，且其他开销不变：

`T_new / T_old = (1 - p) + p / s`

例如 p=20%、s=1.5，整个阶段时间仅下降约 6.7%，不是下降 20% 或 50%。p 不能由父子 inclusive time 相加得到，也要计入新 setup/packing、分派与缓存代价。编码和解码分别评估；合成产品指标前使用预先约定的负载权重。

建议默认候选门槛：局部内核约 ≥10–15% 或目标阶段约 ≥2% 的可重复改善，配对 95% 区间支持真实收益，并且关键模式、p99、资源没有超出事先约定的回退预算。这个数值是实验筛选建议，不是必须强行达到的承诺；简单、低维护且有可靠小收益的改动也可保留。模式允许回退多少应在看到候选结果前确定，不能事后挑选。

下一轮可以把“选定阶段额外减少 5–10% 时间”设为研究目标，**目前不能把它写成预测结果**。K1/K2/K3 是否占到足够预算，必须由 M1 确认；正确性修复也可能先使时间增加。

满足以下条件后，才可以重新讨论“收敛”：

1. 本文 P1 正确性与测试问题解决；源码、编译器及 VL/tail 策略门禁全部通过。
2. 官方发布版/既有行为参考的关系、定点算术契约和支持配置已明确。
3. 目标机无插桩端到端数据有可追溯产物、原始配对样本和区间，语料与功能覆盖达到约定要求。
4. K1/K2/K3 的主要假设至少经过一次可解释的试验；阴性结果保存源码/汇编/输入与数据，可以被独立复查。
5. 剩余独占预算与合理局部加速上界已低于项目定义的最小有用收益，或者实验成本超出预算。不能仅用“连续三轮最小值改善不大”代替这个判断。

最终交付应包含：正确性报告、参照与算术说明、支持构建矩阵、每项实验的保留/回退结论、各模式与真实负载的性能区间、资源变化、已知限制、可复现命令和产物 manifest。历史性能表保留历史身份，撤回已被反证的语义与硬件推断，并用新证据更新当前结论。

## 10. 本次范围与设备说明

用户最终要求为“只做计划，不做实施”。本文是本次唯一纳入版本管理的新增产物；生产源码、测试源码和设备脚本均未修订，未提交 commit。

此前设备部署测试请求被自动审批拒绝，理由是向外部设备传输并执行项目产物需要明确授权；之后用户选择仅做计划，该设备验证步骤已取消。本文没有将任何历史板端结果冒充本轮实测。后续执行本计划时，设备运行与环境设置应单独纳入获准的执行范围。

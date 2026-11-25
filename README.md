# mt76 testmode 连续波干扰 —— 编译工程

把 N30 Pro 的 mt7915/7976 从「发 beacon 帧」升级到「发连续波」，用掉 100% 的空口。

## 背景：为什么 beacon 到不了 50% 以上

我们实测过所有软件层参数：

| 尝试 | 单台 tx 占空比 |
|---|---|
| 8 BSS / 50ms | 39.8% |
| 16 BSS / 50ms | 43~54% |
| 16 BSS / 20ms | 52.9%（理论要 200% 空口，发不全） |
| basic_rate 改 2Mbps | 24.6%（腰斩） |
| 分片 frag=256 | 无变化 |
| 速率掩码 | 固件不受控 |
| AQL 开关 | 无变化 |
| 广播洪流（有客户端） | 38~51%（波动大，依赖手机+电脑） |

根本原因：**802.11 的 CCA**——AP 每次发射前必须"听"空口，忙就退避。
这个规则写在硬件里，任何配置都绕不开。

**唯一绕开的办法是进测试模式**，固件直接发连续波，不做 CCA。

## 原理

`mt7915/Makefile` 里这一行是关键：

```makefile
mt7915e-$(CONFIG_NL80211_TESTMODE) += testmode.o
```

**整个 testmode（含连续波）只受 `CONFIG_NL80211_TESTMODE` 控制**，没有其他开关。

打开后，`mt7915_tm_set_tx_cont()` 会向固件发 `TX_CONT_START (0x05)` 命令：

```c
tx_cont->control_ch = chandef->chan->hw_value;   // 指定信道
tx_cont->bw = CMD_CBW_20MHZ / 40MHZ;             // 指定带宽
tx_cont->rateval = mode << 6 | rate_idx;         // 指定调制
mt7915_tm_rf_switch_mode(dev, RF_OPER_RF_TEST);  // 切到 RF 测试模式
```

MediaTek 开发者自己的描述：

> continuous tx, which is used for sending tx without time gap.
> **Note that continuous tx mode doesn't send real packets, instead, it's pure phy signal**

纯物理层信号，没有帧、没有间隙、没有监听。

## 目标状态

```
1 号  ch1  40MHz 连续波  → 2402-2442  100%
2 号  ch5  40MHz 连续波  → 2422-2462  100%
3 号  ch9  40MHz 连续波  → 2442-2482  100%
                    ─────────────────
               2402-2482 全频段 100% 饱和
```

## ⚠️ 路线结论：**不要只换模块，要刷完整镜像**

这条是花了两次变砖换来的，务必先看。

**试过的错误做法**：只把 `cfg80211 / compat / mac80211 / mt76 / mt76-connac-lib / mt7915e`
六个 `.ko` 传进路由器的 `/lib/modules/6.12.74/`，然后重启。

**结果：内核启动循环，链路反复起落，机器变砖。** 而且：

| 检查项 | 结果 |
|---|---|
| 6 个模块 `sizeof(struct module)` | 全部 640，与目标内核一致 ✓ |
| vermagic | 全部 `6.12.74 SMP mod_unload aarch64` ✓ |
| 是否同源（同一次构建） | 是，整套 6 个一起换 ✓ |
| 内核加载 | **依然崩溃** |

更麻烦的是：崩溃反复硬复位会把 **UBI 状态弄坏**，此后**重刷 `fit` 卷也修不好**，
必须 `mtd erase ubi` 整片擦除才行（见下方「救砖」）。

**正确做法：刷完整固件镜像。** 一次编译 → 内核、驱动、testmode、工具全部同源，
不存在 overlay 写入，也不存在模块混搭；而 U-Boot 的 `upgrade_fw` 只重写 `fit` 卷，
**不碰 overlay、不碰 bootloader**，风险比换模块低得多。

所以 `build.sh collect` 会把 `openwrt-*-squashfs-sysupgrade.itb` 一起收进产物。

## 关键坑：模块必须和内核的 `struct module` 大小一致

这是**最容易踩、报错也最莫名其妙**的一个坑，务必先看：

```
module cfg80211: .gnu.linkonce.this_module section size must match
                 the kernel's built struct module size at run time
```

即使 vermagic、内核版本、架构全都对得上，只要 `struct module` 大小不一致，
内核就拒绝加载（`insmod` 返回 255，`lsmod` 空空，`nl80211 not found`，无线全废）。

- 每个 `.ko` 里的 `.gnu.linkonce.this_module` 段就是编译期的 `struct module`
- 它的字节数由**内核配置**决定，最典型的影响项是 `CONFIG_KALLSYMS`
  （开/关正好差 **64** 字节）

实测数据：

| 来源 | `sizeof(struct module)` | 结果 |
|---|---|---|
| 官方 25.12.1 模块 | **640** | 正常加载 |
| 我们第一版（`make defconfig` 默认） | **704** | 加载失败 |

根因：官方 release 的构建配置里明确写着

```
# CONFIG_KERNEL_KALLSYMS is not set
```

而自己手写 `.config` 再 `make defconfig`，`KERNEL_KALLSYMS` 会用默认值 **y**。

**解决办法**：`build.sh` 现在直接拿官方的
[`config.buildinfo`](https://downloads.openwrt.org/releases/25.12.1/targets/mediatek/filogic/config.buildinfo)
当 `.config`（已存为本目录的 `config.buildinfo`），只改设备名和要编的包。
这样所有这类隐藏选项都和官方一致。

**编完立刻自检**，不用等刷机失败：

```bash
python3 tool/ko_check.py out/cfg80211.ko out/mt76.ko out/mt7915e.ko
#   模块                     实测   结论
#   cfg80211.ko              640   一致，能加载
```

`build.sh collect` 和 GitHub Actions 都会自动跑这一步。

## 编译（二选一）

### 方式 A：本地 WSL2（推荐，迭代快）

```bash
# Windows 管理员 PowerShell 里先装 WSL
wsl --install -d Ubuntu
# 重启，然后进入 Ubuntu：
cd /mnt/e/e_coding/test/路由器干扰环境刷openwrt/firmware-build
bash build.sh
```

首次全流程约 1-2 小时（10 核），之后改代码重编只要几分钟。

### 方式 B：GitHub Actions 云编译（不动本地系统）

仓库：<https://github.com/Nullenterspace/mt76-testmode>

推代码就自动编（`concurrency.cancel-in-progress`，新一轮会取消上一轮，省额度）：

```bash
git push origin main
```

等 40~90 分钟，在 Actions 页面 → 最新一次运行 → Artifacts 里下载
`mt76-testmode-modules.zip`（含三个 `.ko` + `mt76tm`）。
同一页面还有 `build-log` 便于排错。

> 注意：`.gitattributes` 里强制 `*.sh` 等用 LF，缺了它 Linux runner 会报
> `/bin/bash^M: bad interpreter`。

产物一样。

## 产物

`out/` 目录：

| 文件 | 用途 |
|---|---|
| `cfg80211.ko` | nl80211 TESTMODE 命令入口，**必须替换** |
| `mt76.ko` | mt76 公共 testmode 代码 |
| `mt7915e.ko` | mt7915 连续波实现 |
| `mt76tm` 或 `mt76tm_*.ipk` | 控制工具 |

## 安装到路由器

**不刷固件**，只替换内核模块，可随时回滚。

现成脚本（自动备份 → 上传 → 重启 → 校验 → 报错就提示回滚）：

```bash
# 把下载的产物解到 out/ 里，然后一条命令
python tools/testmode.py install --node 1
```

<details>
<summary>手打命令（等价）</summary>

```bash
ssh root@192.168.1.1 '
  mkdir -p /root/kmod-backup &&
  cp /lib/modules/6.12.74/cfg80211.ko /lib/modules/6.12.74/mt76.ko \
     /lib/modules/6.12.74/mt7915e.ko /root/kmod-backup/
'
# 路由器没装 sftp-server，scp 可能失败；用 cat 传更稳：
cat out/cfg80211.ko | ssh root@192.168.1.1 'cat > /lib/modules/6.12.74/cfg80211.ko'
# …mt76.ko / mt7915e.ko 同理，再传 mt76tm 到 /usr/bin/ 并 chmod +x
ssh root@192.168.1.1 'sync; reboot'
```
</details>

重启后必须**验证模块真的加载了**，别急着下一步：

```bash
python tools/testmode.py check --node 1    # 期望：testmode 可用 ✓
```

如果 `lsmod` 里没有 `mt7915e`，看 `dmesg | grep this_module`，
然后 `python tools/testmode.py rollback --node 1`。

## 验证连续波

**testmode 要求射频处于 monitor 模式**（驱动里硬性检查
`hw->conf.flags & IEEE80211_CONF_MONITOR`），否则报 `ENOTCONN (-107)`。
所以流程是「关 AP → 建 monitor → 发射」。全套一条命令：

```bash
python tools/testmode.py cw --node 1 -c 1 -w 40 -m ht -M 7 -n 2 -a 3
```

它会依次：`wifi down` → 建 `mon0` → `iw dev mon0 set freq` →
`mt76tm cont mon0 …` → 读 `iw survey` 报 `busy/rx/tx`。

关键判据：**tx 接近 100%** 才说明纯载波真的占满信道（beacon 方案单台最多 44%）。

```bash
# 停连续波并恢复干扰配置
python tools/testmode.py restore --node 1
```

## 回滚

```bash
python tools/testmode.py rollback --node 1
```

（内部就是 `cp /root/ko-bak-testmode/*.ko /lib/modules/6.12.74/ && reboot`）
断电重启没用——模块在 overlay 上，必须换回旧文件。

## 救砖（实测流程，2026-11-24 两次走通）

换模块导致启动循环时，**按下面顺序**，前两步缺一不可：

| 步骤 | 操作 | 作用 | 日志标志 |
|---|---|---|---|
| 1 | 断电 → 按住 **WPS 按钮** → 上电 15 秒 | 厂商流程：重写 BL2/FIP + **`mtd erase ubi`** | `RRQ bl2.img` → `RRQ fip.bin` → `RRQ customer`(404) |
| 2 | 断电 → 按住 **Reset 孔** → 上电 15 秒 | `upgrade_fw`：写入 `fit` 卷 | `RRQ ...squashfs-sysupgrade.itb` |

**只做第 2 步修不好**（实测：重刷 fit 后仍然循环），因为损坏在 UBI 层，不是系统分区。

前置准备（PC 侧）：

```powershell
# tftp-root 里必须放齐这三个文件（横杠命名，无版本号前缀）：
#   bl2.img       = 原厂 BL2 备份，1,048,576 字节（整分区）
#   fip.bin       = 官方 ...-bl31-uboot.fip 补零到 2,097,152 字节
#   openwrt-mediatek-filogic-netis_nx30v2-squashfs-sysupgrade.itb
python tools\tftp_server.py --bind 192.168.1.254 --root tftp-root
```

> ⚠️ `bl2.img` / `fip.bin` 必须和机器现状**字节级一致**（前者用 `backup-original` 里的整分区备份，
> 后者就是当初写进去的那个 FIP + 补零）。因为厂商流程用的是 `nand write $addr <分区>`
> **不带长度**，文件不够大就会把内存里的垃圾写进 bootloader。
> 写入期间**绝对不能断电**。

## 已知风险

1. **`struct module` 必须一致**（见上文「关键坑」）。vermagic 对了不代表能装，
   靠 `tool/ko_check.py` 提前确认 640 字节。
2. **testmode 会独占射频**，那台机器不能再当 AP 用，也没有 SSH over WiFi。
   所以操作全程走网线，且必须先用 `mt76tm off` / `restore` 恢复。
3. **替换 cfg80211.ko 比较敏感**，但可回滚，且不动固件分区。
4. 连续波是纯物理层信号，**自己也在被干扰**——测鼠标时电脑别连这台机器的 WiFi。

## 目录结构

```
firmware-build/
├── build.sh                          # 主编译脚本
├── config.buildinfo                  # 官方 25.12.1 构建配置（必须，决定 struct module）
├── tool/
│   ├── mt76tm.c                      # 工具源码（纯 netlink，无依赖）
│   └── ko_check.py                   # 产物自检：sizeof(struct module) 是否为 640
├── package/mt76tm-Makefile           # 打成 OpenWrt 软件包用
├── .gitattributes                    # 强制 LF，否则 runner 上 build.sh 跑不了
└── .github/workflows/build.yml       # 云端编译
```

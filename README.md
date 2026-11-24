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

1. 新建一个 GitHub 仓库
2. 把本目录所有文件传上去（含 `.github/workflows/build.yml`）
3. 仓库页面 → Actions → "Build mt76 testmode modules" → Run workflow
4. 等 40-90 分钟，在 Artifacts 里下载 `mt76-testmode-modules.zip`

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

```bash
# 1. 先备份原模块
ssh root@192.168.1.1 '
  mkdir -p /root/kmod-backup &&
  cp /lib/modules/6.12.74/cfg80211.ko   /root/kmod-backup/ &&
  cp /lib/modules/6.12.74/mt76.ko       /root/kmod-backup/ &&
  cp /lib/modules/6.12.74/mt7915e.ko    /root/kmod-backup/
'

# 2. 传新模块和工具
scp out/cfg80211.ko out/mt76.ko out/mt7915e.ko root@192.168.1.1:/lib/modules/6.12.74/
scp out/mt76tm root@192.168.1.1:/usr/bin/
ssh root@192.168.1.1 'chmod +x /usr/bin/mt76tm'

# 3. 重启
ssh root@192.168.1.1 reboot
```

## 验证

```bash
# 探测 testmode 是否可用
mt76tm status phy0-ap0
# 期望输出：testmode 可用 ✓

# 开始连续发射（单台 40MHz）
mt76tm cont phy0-ap0 -c 1 -w 40 -m ht -M 7 -n 2

# 看空口占用（另开一个 ssh，或在电脑上看监控面板）
iw dev phy0-ap0 survey dump | grep -A6 'in use'
# 期望 tx 接近 100%

# 停止
mt76tm off phy0-ap0
```

## 回滚

```bash
ssh root@192.168.1.1 '
  cp /root/kmod-backup/*.ko /lib/modules/6.12.74/
  reboot
'
```

或者直接断电重启也没用——要恢复必须换回旧模块。

## 已知风险

1. **vermagic 必须匹配**。用同一版本 OpenWrt（v25.12.1）编译出来的模块 vermagic 应该一致
   （`6.12.74 SMP mod_unload aarch64`）。如果 `insmod` 报 vermagic 不符，说明版本没对上。
2. **testmode 会独占射频**，那台机器不能再当 AP 用，也没有 SSH over WiFi。
   所以操作全程走网线，且必须先用 `mt76tm off` 恢复。
3. **替换 cfg80211.ko 比较敏感**，但可回滚，且不动固件分区。

## 目录结构

```
firmware-build/
├── build.sh                          # 主编译脚本
├── tool/mt76tm.c                     # 工具源码（纯 netlink，无依赖）
├── package/mt76tm-Makefile           # 打成 OpenWrt 软件包用
└── .github/workflows/build.yml       # 云端编译
```

#!/bin/bash
# =============================================================================
# build.sh —— 在 Linux 上编译「开了 mt76 testmode 的内核模块」+ 配套工具
#
# 产出（都在 out/ 目录里）：
#   *.ko            三个内核模块：cfg80211 / mt76 / mt7915e
#   mt76tm          aarch64 工具（也可打包成 .ipk）
#
# 为什么只需要这三个模块：
#   mt7915/Makefile 里写着  mt7915e-$(CONFIG_NL80211_TESTMODE) += testmode.o
#   所以 testmode（含连续波 TX_CONT）完全由 CONFIG_NL80211_TESTMODE 控制。
#   打开它需要重编 cfg80211（命令入口在这）、mt76（公共部分）、mt7915e（实现）。
#
# 用法：
#   bash build.sh                 # 全流程
#   bash build.sh deps            # 只装依赖
#   bash build.sh config          # 只生成配置
#   bash build.sh build           # 只编译
#   bash build.sh collect         # 只收集产物
# =============================================================================
set -euo pipefail

OPENWRT_VER="${OPENWRT_VER:-v25.12.1}"
TARGET="mediatek"
SUBTARGET="filogic"
DEVICE="netis_nx30v2"
KERNEL_VER="6.12"
JOBS="${JOBS:-$(nproc)}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/openwrt"
OUT="$ROOT/out"

log() { echo -e "\n\033[1;36m[$(date +%H:%M:%S)] $*\033[0m"; }
die() { echo -e "\033[1;31m错误: $*\033[0m" >&2; exit 1; }

# ---------------------------------------------------------------- 依赖
do_deps() {
    log "安装编译依赖"
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        build-essential clang flex bison g++ gawk gcc-multilib g++-multilib \
        gettext git libncurses-dev libssl-dev python3 python3-setuptools \
        python3-pyelftools rsync swig unzip zlib1g-dev file wget \
        libelf-dev ecj fastjar java-propose-classpath \
        || die "依赖安装失败"
    log "依赖完成"
}

# ---------------------------------------------------------------- 源码
do_source() {
    if [ -d "$SRC" ]; then
        log "源码已存在，跳过克隆"
        return
    fi
    log "克隆 OpenWrt $OPENWRT_VER（浅克隆）"
    git clone --depth 1 --branch "$OPENWRT_VER" \
        https://git.openwrt.org/openwrt/openwrt.git "$SRC" \
        || die "克隆失败"
    cd "$SRC"
    log "更新 feeds（只装基础几个，省时间）"
    ./scripts/feeds update -a
    ./scripts/feeds install -a
}

# ---------------------------------------------------------------- 配置
do_config() {
    cd "$SRC"
    log "写 .config（以官方 release 的 config.buildinfo 为底）"

    # ------------------------------------------------------------------
    # 为什么必须用官方 config.buildinfo 当底？
    #
    # 官方 25.12.1 里有一句  "# CONFIG_KERNEL_KALLSYMS is not set"
    # （https://downloads.openwrt.org/releases/25.12.1/targets/mediatek/filogic/config.buildinfo）
    #
    # CONFIG_KALLSYMS 会给 struct module 加上 kallsyms / core_kallsyms 等字段，
    # 结构体大小从 640 变成 704。模块的 .gnu.linkonce.this_module 段里存的
    # 就是 struct module，内核加载时会比对大小，不一致就报：
    #   "this_module section size must match the kernel's built
    #    struct module size at run time"     -> insmod 直接失败
    #
    # 自己手写 .config 再 make defconfig，KALLSYMS 会用默认值 y，
    # 编出来的模块就装不上去。所以这里整份照抄官方构建配置。
    # ------------------------------------------------------------------
    [ -f "$ROOT/config.buildinfo" ] || die "缺少 $ROOT/config.buildinfo"
    cp "$ROOT/config.buildinfo" .config

    # 官方那份是"给全部设备、全部 kmod 出包"的构建机器人配置，我们只要一台设备
    sed -i -E '/^CONFIG_TARGET_DEVICE_/d'          .config
    sed -i -E '/^CONFIG_TARGET_DEVICE_PACKAGES_/d' .config
    sed -i -E '/^CONFIG_TARGET_ALL_PROFILES=/d'    .config
    sed -i -E '/^CONFIG_TARGET_MULTI_PROFILE=/d'   .config
    sed -i -E '/^CONFIG_ALL_KMODS=/d'              .config
    sed -i -E '/^CONFIG_ALL_NONSHARED=/d'          .config
    # 去掉调试信息 / SDK / luci 等，缩短编译时间（都不影响 struct module）
    sed -i -E '/^CONFIG_(IB|SDK|SDK_LLVM_BPF|BUILDBOT|JSON_CYCLONEDX_SBOM|DEVEL|COLLECT_KERNEL_DEBUG|REPRODUCIBLE_DEBUG_INFO)=/d' .config
    sed -i -E '/^CONFIG_PACKAGE_(luci|ucode-mod|rpcd|uhttpd|owut|px5g|cgi-io|attendedsysupgrade|libiwinfo|liblucihttp)/d' .config

    cat >> .config <<EOF
CONFIG_TARGET_${TARGET}=y
CONFIG_TARGET_${TARGET}_${SUBTARGET}=y
CONFIG_TARGET_MULTI_PROFILE=n
CONFIG_TARGET_DEVICE_${TARGET}_${SUBTARGET}_DEVICE_${DEVICE}=y
CONFIG_TARGET_DEVICE_PACKAGES_${TARGET}_${SUBTARGET}_DEVICE_${DEVICE}=""
# CONFIG_KERNEL_KALLSYMS is not set
CONFIG_PACKAGE_iw=y
CONFIG_PACKAGE_mt76tm=y
EOF

    make defconfig >/dev/null || die "defconfig 失败"

    # 万一 defconfig 又把 KALLSYMS 打开了，这里立刻拦下来，别白编 40 分钟
    grep -q '^# CONFIG_KERNEL_KALLSYMS is not set' .config \
        || die "CONFIG_KERNEL_KALLSYMS 没关掉！struct module 会不一致"
    log "  KALLSYMS 保持关闭（struct module = 640 字节）"

    log "打开 CONFIG_NL80211_TESTMODE"
    local cfg="target/linux/${TARGET}/${SUBTARGET}/config-${KERNEL_VER}"
    [ -f "$cfg" ] || die "找不到 $cfg"

    # 先删掉可能存在的旧行（-m / # CONFIG_... is not set），再追加
    sed -i '/^CONFIG_NL80211_TESTMODE/d' "$cfg"
    sed -i '/^# CONFIG_NL80211_TESTMODE is not set/d' "$cfg"
    echo "CONFIG_NL80211_TESTMODE=y" >> "$cfg"

    log "插入自定义软件包 mt76tm"
    mkdir -p package/mt76tm/src
    cp "$ROOT/tool/mt76tm.c" package/mt76tm/src/
    cp "$ROOT/package/mt76tm-Makefile" package/mt76tm/Makefile

    # 让 mt76tm 参与编译
    echo "CONFIG_PACKAGE_mt76tm=y" >> .config
    make defconfig >/dev/null

    log "确认配置："
    grep -c "CONFIG_NL80211_TESTMODE=y" "$cfg" || die "写入失败"
    grep -c "CONFIG_PACKAGE_mt76tm=y" .config || die "mt76tm 未启用"
    log "配置完成"
}

# ---------------------------------------------------------------- 编译
do_build() {
    cd "$SRC"
    log "下载源码包（-j$JOBS）"
    make -j"$JOBS" download || log "部分下载失败，继续（编译时会重试）"

    log "开始编译（这一步最久，10 核约 40-90 分钟）"
    make -j"$JOBS" V=s 2>&1 | tee "$ROOT/build.log" || die "编译失败，见 build.log"

    log "编译完成"
}

# ---------------------------------------------------------------- 收集
# 这一整组 wifi 模块必须来自同一次构建！
#   compat/cfg80211/mac80211 是一家（backports 包），
#   mt76/mt76-connac-lib/mt7915e 是一家（mt76 包）。
# 只换其中一部分（比如只换 cfg80211+mt76+mt7915e，留着官方的 mac80211），
# 两边的内部结构就会对不上，模块加载时会 panic——实测就是开不了机、
# 网口都没链路，只能按 Reset 孔用 U-Boot 救。
#
# 注意 mt76-connac-lib 是**横杠**（设备上装的也是这个文件名，
# 只有 lsmod 显示时才会变成下划线），写错就收不到，整套不齐。
WIFI_MODULES="compat cfg80211 mac80211 mt76 mt76-connac-lib mt7915e"

do_collect() {
    mkdir -p "$OUT"
    cd "$SRC"

    log "收集内核模块（整套：$WIFI_MODULES）"
    local kdir
    kdir=$(find build_dir -maxdepth 4 -type d -name "linux-${TARGET}_${SUBTARGET}" | head -1)
    [ -n "$kdir" ] || die "找不到内核 build_dir"

    # 注意：模块文件名可能带横杠也可能带下划线
    #   设备上装的是 mt76-connac-lib.ko（lsmod 里显示才会变成 mt76_connac_lib）
    # 这里两种写法都找，避免漏收导致整套不齐
    for m in $WIFI_MODULES; do
        local f alt_d alt_u
        alt_d=$(echo "$m" | tr '_' '-')
        alt_u=$(echo "$m" | tr '-' '_')
        f=$(find "$kdir" \( -name "${m}.ko" -o -name "${alt_d}.ko" -o -name "${alt_u}.ko" \) -type f 2>/dev/null | head -1)
        if [ -n "$f" ]; then
            cp "$f" "$OUT/$(basename "$f")"
            log "  $(basename "$f")  $(du -h "$f" | cut -f1)"
        else
            log "  !! 没找到 $m.ko（试过 $m / $alt_d / $alt_u）"
        fi
    done

    log "收集工具 ipk"
    find bin -name "mt76tm_*.ipk" -exec cp {} "$OUT/" \; 2>/dev/null || true

    log "收集裸二进制（备用，可直接 scp）"
    local tdir
    tdir=$(find build_dir -path "*mt76tm*" -name "mt76tm" -type f | head -1)
    [ -n "$tdir" ] && cp "$tdir" "$OUT/" && log "  mt76tm  $(du -h "$tdir" | cut -f1)"

    log "产物清单："
    ls -la "$OUT"

    # 整套必须齐全：少一个都不许当成品用（上次就是少收 3 个才变砖）
    local have want miss=""
    have=$(ls "$OUT"/*.ko 2>/dev/null | xargs -r -n1 basename | tr '-' '_')
    for m in $WIFI_MODULES; do
        want=$(echo "$m" | tr '-' '_')
        echo "$have" | grep -qx "${want}.ko" || miss="$miss $m"
    done
    if [ -n "$miss" ]; then
        log "!! 模块不全，缺:$miss —— 这套产物不能用"
    else
        log "整套 6 个模块齐全 ✓"
    fi

    # 关键自检：模块能不能被目标内核加载，此时就能确定，不用等刷机失败
    log "自检 sizeof(struct module)（必须 640，否则 install 会失败）"
    python3 "$ROOT/tool/ko_check.py" "$OUT"/*.ko \
        || log "!! 自检未通过：模块装不上，别急着刷"
}

# ---------------------------------------------------------------- 主流程
mkdir -p "$OUT"
case "${1:-all}" in
    deps)    do_deps ;;
    source)  do_source ;;
    config)  do_config ;;
    build)   do_build ;;
    collect) do_collect ;;
    all)
        do_deps
        do_source
        do_config
        do_build
        do_collect
        ;;
    *) die "未知参数 $1" ;;
esac

log "完成。产物在 $OUT"

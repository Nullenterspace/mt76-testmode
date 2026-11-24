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
    log "写 .config"

    cat > .config <<EOF
CONFIG_TARGET_${TARGET}=y
CONFIG_TARGET_${TARGET}_${SUBTARGET}=y
CONFIG_TARGET_${TARGET}_${SUBTARGET}_DEVICE_${DEVICE}=y
CONFIG_TARGET_MULTI_PROFILE=n
# 只编我们需要的，减小体积、加快速度
CONFIG_PACKAGE_wpad-basic-mbedtls=y
CONFIG_PACKAGE_iw=y
CONFIG_PACKAGE_dropbear=y
CONFIG_PACKAGE_libustream-mbedtls=y
CONFIG_STRIP_KERNEL_EXPORTS=y
CONFIG_DEBUG_INFO=n
CONFIG_SIGNED_PACKAGES=n
EOF

    make defconfig >/dev/null || die "defconfig 失败"

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
do_collect() {
    mkdir -p "$OUT"
    cd "$SRC"

    log "收集内核模块"
    local kdir
    kdir=$(find build_dir -maxdepth 4 -type d -name "linux-${TARGET}_${SUBTARGET}" | head -1)
    [ -n "$kdir" ] || die "找不到内核 build_dir"

    for m in cfg80211 mt76 mt7915e; do
        local f
        f=$(find "$kdir" -name "${m}.ko" -type f | head -1)
        if [ -n "$f" ]; then
            cp "$f" "$OUT/"
            log "  $m.ko  $(du -h "$f" | cut -f1)"
        else
            log "  !! 没找到 $m.ko"
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

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ko_check.py —— 内核模块能否被目标内核加载的快速自检

原理
----
每个 .ko 里都有一个 ELF 段  .gnu.linkonce.this_module，
它的内容就是编译期那个内核的  struct module  （模块描述符本身）。
段大小 == sizeof(struct module)。

内核 insmod 时会比对：
    if (this_module 段大小 != 本内核的 sizeof(struct module))
        拒绝加载:
        "module xxx: .gnu.linkonce.this_module section size must
         match the kernel's built struct module size at run time"

而 sizeof(struct module) 由内核配置决定，最典型的影响项是
CONFIG_KALLSYMS（开/关差 64 字节）。

所以：只要本脚本报出的数字与目标内核一致，模块就一定能加载。
本工程目标内核 = OpenWrt 25.12.1 r32768 / mediatek filogic / 6.12.74，
官方构建配置里 KALLSYMS 是关的，struct module = 640 字节。

用法
----
    python3 ko_check.py out/cfg80211.ko out/mt76.ko out/mt7915e.ko
    EXPECT_MODULE_SIZE=640 python3 ko_check.py out/*.ko

返回值
------
    0  全部一致
    1  有不一致 / 解析失败
"""

import os
import struct
import sys

# 目标内核（OpenWrt 25.12.1 r32768-b21cfa8f8c, 6.12.74, KALLSYMS=n）
DEFAULT_EXPECT = 640
SECTION = ".gnu.linkonce.this_module"


def this_module_size(path):
    """返回 .gnu.linkonce.this_module 的 sh_size，找不到返回 None"""
    with open(path, "rb") as f:
        d = f.read()

    if d[:4] != b"\x7fELF":
        return None, "不是 ELF 文件"

    ei_class = d[4]          # 1=32bit, 2=64bit
    ei_data = d[5]           # 1=LE, 2=BE
    if ei_data != 1:
        return None, "不是小端 ELF"
    is64 = ei_class == 2

    if is64:
        e_shoff = struct.unpack_from("<Q", d, 0x28)[0]
        e_shentsize = struct.unpack_from("<H", d, 0x3A)[0]
        e_shnum = struct.unpack_from("<H", d, 0x3C)[0]
        e_shstrndx = struct.unpack_from("<H", d, 0x3E)[0]
    else:
        e_shoff = struct.unpack_from("<I", d, 0x20)[0]
        e_shentsize = struct.unpack_from("<H", d, 0x2E)[0]
        e_shnum = struct.unpack_from("<H", d, 0x30)[0]
        e_shstrndx = struct.unpack_from("<H", d, 0x32)[0]

    def shdr(i):
        o = e_shoff + i * e_shentsize
        if is64:
            return {
                "name": struct.unpack_from("<I", d, o)[0],
                "off": struct.unpack_from("<Q", d, o + 0x18)[0],
                "size": struct.unpack_from("<Q", d, o + 0x20)[0],
            }
        return {
            "name": struct.unpack_from("<I", d, o)[0],
            "off": struct.unpack_from("<I", d, o + 0x10)[0],
            "size": struct.unpack_from("<I", d, o + 0x14)[0],
        }

    strtab = shdr(e_shstrndx)
    base = strtab["off"]
    for i in range(e_shnum):
        s = shdr(i)
        if s["name"] >= strtab["size"]:
            continue
        end = d.index(b"\0", base + s["name"])
        if d[base + s["name"]:end].decode("utf-8", "replace") == SECTION:
            return s["size"], None
    return None, "没有 %s 段（不是内核模块？）" % SECTION


def main(argv):
    expect = int(os.environ.get("EXPECT_MODULE_SIZE", DEFAULT_EXPECT))
    paths = [p for p in argv if p.lower().endswith(".ko")]
    if not paths:
        print("用法: python3 ko_check.py <*.ko ...>")
        return 1

    print("目标内核 sizeof(struct module) = %d 字节" % expect)
    print()
    print("  %-18s %10s   %s" % ("模块", "实测", "结论"))
    print("  " + "-" * 46)

    bad = 0
    for p in paths:
        size, err = this_module_size(p)
        name = os.path.basename(p)
        if err:
            print("  %-18s %10s   %s" % (name, "?", err))
            bad += 1
            continue
        if size == expect:
            print("  %-18s %10d   一致，能加载" % (name, size))
        else:
            diff = size - expect
            hint = ""
            if diff == 64:
                hint = "  <- 典型的 KALLSYMS 开着 (+64)"
            elif diff == -64:
                hint = "  <- 典型的 KALLSYMS 关着 (-64)"
            print("  %-18s %10d   不一致 (%+d)，装不上%s" % (name, size, diff, hint))
            bad += 1

    print()
    if bad:
        print("结果: %d / %d 个模块会被内核拒绝" % (bad, len(paths)))
        return 1
    print("结果: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""gen_fixture.py — listfiles 已知真值回归 fixture 生成器（测试需求规格 v2 第 5 项）

生成内容（main 模式）：
  big/            单目录 7 万+ 条目（os.link 硬链加速，内容尺寸一致）
  deepnest/...    普通嵌套目录若干
  gbk_*/
          含 GBK 编码的非法 UTF-8 文件名（bytes 路径，早期 Windows/FTP 遗留形态）
  link_file       有效文件软链；link_dir 目录软链（默认不跟随）；broken_link 悬空软链
  inject_target/  8 个子目录 + 20 个文件（LD_PRELOAD 假空/假 EOF 注入靶点，
                  nlink oracle 可检测：st_nlink-2=8 ≠ 实际读到子目录数）
  churn/          2000 个待并发删除文件（验证 ENOENT/ENOTDIR 豁免路径）

deep 模式：
  逐级 chdir 创建总长度 >4096 的路径（绕开单次 mkdir 的 PATH_MAX 限制），
  底层放 3 个文件。listfiles 必须以 ENAMETOOLONG 上报、非零退出（v15.5.7+）。

manifest：
  二进制安全遍历（os.fsencode），每行 "path st_size"，不含根目录自身；
  目录软链作为条目记录但不跟随（与 listfiles 默认 -D 行为对齐）。
"""
import argparse
import os
import shutil
import sys


def w(path_bytes, data=b"x\n"):
    with open(path_bytes, "wb") as f:
        f.write(data)


def build_main(root: bytes, big_count: int):
    os.makedirs(root, exist_ok=True)

    # 1) 超大单目录：big_count 个硬链条目（64 个模板轮转，规避 ext4 单 inode 65000 硬链上限）
    big = root + b"/big"
    os.makedirs(big, exist_ok=True)
    templates = []
    for t in range(64):
        tp = big + ("/.tpl_%02d" % t).encode()
        w(tp, b"fixture\n")
        templates.append(tp)
    for i in range(big_count):
        os.link(templates[i % 64], big + ("/f_%06d.dat" % i).encode())
    for tp in templates:
        os.unlink(tp)

    # 2) 普通嵌套
    for d in [b"/deepnest/a/b/c", b"/deepnest/a/b2", b"/deepnest/a2"]:
        os.makedirs(root + d, exist_ok=True)
    for i in range(10):
        w(root + b"/deepnest/a/b/c/n_%d.txt" % i)
        w(root + b"/deepnest/a2/n_%d.txt" % i)

    # 3) GBK 非法 UTF-8 文件名（"你好文件.txt" 的 GBK 编码等）
    gbk_dir = root + "/gbk_目录".encode("utf-8")
    os.makedirs(gbk_dir, exist_ok=True)
    w(gbk_dir + b"/\xc4\xe3\xba\xc3\xce\xc4\xbc\xfe.txt")   # GBK"你好文件.txt"，非法 UTF-8
    w(gbk_dir + b"/\xd7\xd4\xb6\xaf\xbb\xaf.csv")           # GBK"自动化.csv"
    w(gbk_dir + b"/normal_ascii.txt")

    # 4) 软链：文件软链、目录软链（默认不跟随）、悬空软链
    w(root + b"/real_target.txt", b"target\n")
    os.symlink(b"real_target.txt", root + b"/link_file")
    os.symlink(b"deepnest", root + b"/link_dir")
    os.symlink(b"/nonexistent_lf_target", root + b"/broken_link")

    # 5) 注入靶点：8 子目录 + 20 文件（nlink oracle：st_nlink-2 == 8）
    inj = root + b"/inject_target"
    os.makedirs(inj, exist_ok=True)
    for i in range(8):
        os.makedirs(inj + ("/sub_%d" % i).encode(), exist_ok=True)
        w(inj + ("/sub_%d/inner.txt" % i).encode())
    for i in range(20):
        w(inj + ("/top_%02d.txt" % i).encode())

    # 6) 并发删除用文件集
    churn = root + b"/churn"
    os.makedirs(churn, exist_ok=True)
    for i in range(2000):
        w(churn + ("/del_%04d.txt" % i).encode())


def build_deep(root: bytes):
    """逐级 chdir 构建总长 >4096 的路径，底层 3 个文件。"""
    os.makedirs(root, exist_ok=True)
    start = os.getcwd()
    os.chdir(root)
    try:
        comp = b"d" * 100
        depth = 0
        while len(os.getcwd()) < 4300:
            os.makedirs(comp, exist_ok=True)
            os.chdir(comp)
            depth += 1
        for i in range(3):
            w(("bottom_%d.txt" % i).encode())
        sys.stderr.write("[gen_fixture] deep path: depth=%d len=%d\n"
                         % (depth, len(os.getcwd())))
    finally:
        os.chdir(start)


def write_manifest(root: bytes, out_path: str):
    lines = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames:
            full = dirpath + b"/" + name
            if os.path.islink(full):
                # 目录软链：不跟随（os.walk 默认），作为条目记录一次
                lines.append(full + b" " + str(os.lstat(full).st_size).encode() + b"\n")
            # 非软链子目录会在后续迭代作为 dirpath 出现，届时记录
        if dirpath != root:
            lines.append(dirpath + b" " + str(os.lstat(dirpath).st_size).encode() + b"\n")
        for name in filenames:
            full = dirpath + b"/" + name
            lines.append(full + b" " + str(os.lstat(full).st_size).encode() + b"\n")
    with open(out_path, "wb") as f:
        f.writelines(lines)
    sys.stderr.write("[gen_fixture] manifest: %d entries -> %s\n" % (len(lines), out_path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="fixture 根目录（存在则先清空重建）")
    ap.add_argument("--mode", choices=["main", "deep"], default="main")
    ap.add_argument("--big-count", type=int, default=70000)
    ap.add_argument("--manifest", default=None, help="真值 manifest 输出路径")
    args = ap.parse_args()

    root = os.fsencode(os.path.abspath(args.root))
    if os.path.exists(root):
        shutil.rmtree(root)

    if args.mode == "deep":
        build_deep(root)
    else:
        build_main(root, args.big_count)

    if args.manifest:
        write_manifest(root, args.manifest)


if __name__ == "__main__":
    main()

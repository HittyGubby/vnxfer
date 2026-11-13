# vnxfer

Linux 上的 VNC 文件传输客户端。目标是补齐 TightVNC / UltraVNC 在 Linux 端缺失的文件传输能力，不依赖 Wine。

当前进度：**M0–M5 完成** —— 三套后端（UltraVNC / TightVNC 2.x / TightVNC 1.3）、递归传输、断点续传、失败重试、Qt6 图形界面。

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

依赖：CMake ≥ 3.20、C++20 编译器、OpenSSL、zlib、Qt6 Widgets（仅 GUI）。

产物：`build/vnxfer`（CLI）、`build/vnxfer-gui`（GUI）。

## GUI

```bash
./build/vnxfer-gui
./build/vnxfer-gui --connect 192.0.2.10:5900 --password secret --path /C:/Users/Public
```

- 左栏本地文件、右栏远端文件，中间是上传/下载/新建文件夹/新建文件/删除/重命名。
- 两栏都支持点表头排序；远端路径栏可直接编辑回车跳转，`Up` 返回上级。
- 连接历史（最近 15 条）与"记住密码"（QSettings，base64 编码，**不是加密**）。
- 传输进度在底部，失败会弹窗并可自动重试（默认 3 次、指数退避）。

## CLI

```bash
./build/vnxfer probe HOST[:PORT] [--password PW]
./build/vnxfer ls    HOST[:PORT] --path /C:/Users
./build/vnxfer get   HOST[:PORT] --remote /C:/1.xml --local ./1.xml [--resume] [--recursive]
./build/vnxfer put   HOST[:PORT] --local ./a.bin --remote /C:/Users/Public/a.bin [--resume] [--recursive]
./build/vnxfer mkdir|rm|mv HOST[:PORT] --path ... | --from A --to B
```

通用参数：`--password`（或 `VNXFER_PASSWORD`）、`--timeout MS`、`--retries N`（默认 3）、`--retry-delay MS`（默认 500，指数退避）。

## 支持矩阵

| 协议 | 识别方式 | ls | get | put | mkdir | rm/mv | 递归 | 续传 |
|---|---|---|---|---|---|---|---|---|
| UltraVNC（消息类型 7） | 权限探测 → `Access(14)` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| TightVNC 2.x（0xFC0001xx） | Tight 认证 + caps 含 `0xFC000100` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| TightVNC 1.3（130–136） | Tight 认证 + caps 含 130–135 | ✅ | ✅ | ✅ | ✅ | — | ✅ | — |

## 已验证

| 服务端 | 结果 |
|---|---|
| x11vnc 0.9.17 `-ultrafilexfer` | 全部命令 + 递归树 diff 一致 + 5 MiB 往返 md5 一致 |
| TightVNC Server 2.8.x（Windows） | 全部命令 + 递归 + 1/2 MiB 续传 md5 一致，GUI 连接/浏览/自测下载通过 |
| libvncserver + TightVNC 扩展 | ls/get/put/mkdir + 递归 |

## 实现要点 / 踩过的坑

- **RFB 3.3/3.6 的 None 认证不能回类型字节**：服务端把那个字节当成 ClientInit，整条流错位。
- **UltraVNC 下载需要回执**：收到 `FileHeader` 后回一个空的 `FileHeader`，服务端才开始发 `FilePacket`；`sizeH` 4 字节别漏读。
- **UltraVNC 上传没有结束确认**：发完 `EndOfFile` 再 round-trip 一个驱动列表请求，否则连接先关会把文件截断。
- **TightVNC 2.x 的压缩块是 `deflate(Z_SYNC_FLUSH)` 且不 finish**：没有 Adler-32，要按输出字节数判定；同一连接共用一个流，inflate 必须只初始化一次并消费完每帧输入。
- **TightVNC 2.x 的文件名带内嵌 NUL**：不清掉会让路径拼接产生 `\0`，递归列表会无限循环。
- **TightVNC 2.x 上传新文件必须 overwrite=1**（服务端靠 truncate 创建），否则报"系统找不到指定的文件"。
- **TightVNC 2.x 的 FT 只在 Tight 安全类型连接上注册**，普通 VNC auth 连接发 0xFC 消息会被断开。
- **TightVNC 1.3 的目录条目没有 flags**：目录用 size=-1 表示；`ftproot` 前缀是 `$HOME`。
- **TightVNC 1.3 没有删除/重命名消息**。

## 路线图

- M0 ✅ 连接、认证、能力探测
- M1 ✅ UltraVNC 后端
- M2 ✅ TightVNC 2.x 后端
- M3 ✅ TightVNC 1.3 后端
- M4 ✅ 递归目录、断点续传、失败重试
- M5 ✅ Qt6 GUI

协议细节见 [research/PROTOCOL_SPEC.md](research/PROTOCOL_SPEC.md)。

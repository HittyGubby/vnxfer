# vnxfer

Linux 上的 VNC 文件传输客户端。目标是补齐 TightVNC / UltraVNC 在 Linux 端缺失的文件传输能力，不依赖 Wine。

三套后端（UltraVNC / TightVNC 2.x / TightVNC 1.3）、CLI 与 Qt6 GUI。

## 安装

Arch Linux：`packaging/build-arch-pkg.sh` 会构建本地包并用 `paru -U` 安装。

其他平台：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && cpack -G "TGZ;DEB;RPM"   # 或 ZIP / DragNDrop
```

依赖：CMake ≥ 3.20、C++20、OpenSSL、zlib、Qt6 Widgets；可选 libsecret（密码进系统钥匙串）。

产物：`build/vnxfer`（CLI）、`build/vnxfer-gui`（GUI）。

## GUI

```bash
./build/vnxfer-gui
./build/vnxfer-gui --connect 192.0.2.10:5900 --password secret --path /C:/Users/Public
```

- 左右对称的双栏树：两侧都有 `Up / Refresh / 路径栏`，文件夹优先排序、图标、展开箭头与本地一致。
- 中间：`Upload →`、`← Download`、`New folder`、`New file`、`Delete`、`Rename`。
- 上传/新建的目标目录跟随远端选中项：选中文件夹就是该文件夹，选中文件就是它所在的目录（不再是树的根）。
- 传输队列：多任务排队、串行执行、`Cancel` 清空并中断当前任务。
- 拖拽：本地文件/目录拖到右侧即上传，远端条目拖到左侧即下载。
- 断点续传：下载/上传时若目标已有部分文件，弹窗询问是否续传。
- 覆盖确认：上传前若远端已有同名文件会询问是否覆盖，目录已存在会询问是否合并。
- 传输完成后只刷新发生变化的目录，并恢复已展开的节点，不会整棵树折叠。
- 限速：`Settings → Rate limit...`（KB/s，0 为不限）。
- 校验：`Settings → Verify uploads (MD5)`，TightVNC 2.x 上传后自动比对。
- 心跳：连接后每 45 秒发一次轻量请求（TightVNC 2.x 用根目录列表，UltraVNC 用权限探测），失败只写日志与状态栏，不弹窗。
- 连接管理：历史下拉本身就是 profile——每个记住的 host 自动保存端口、密码与上次路径，选中即恢复；改密码/端口会自动更新记录，Host 框右侧的 `×` 删除当前 host（带确认），`Probe` 连接前探测协议。
- 密码：`remember` 时优先写入系统钥匙串（libsecret），不可用时退化为 QSettings base64（仅混淆）。
- 视图：`View → Show hidden files`、`Language → English/中文`、`Theme → System/Fusion`。
- 日志：底部 `Log` 停靠面板。

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

| 协议 | 识别方式 | ls | get | put | mkdir | rm/mv | 递归 | 续传 | MD5 |
|---|---|---|---|---|---|---|---|---|---|
| UltraVNC（消息类型 7） | 权限探测 → `Access(14)` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — | — |
| TightVNC 2.x（0xFC0001xx） | Tight 认证 + caps 含 `0xFC000100` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| TightVNC 1.3（130–136） | Tight 认证 + caps 含 130–135 | ✅ | ✅ | ✅ | ✅ | — | ✅ | — | — |

## 已验证

| 服务端 | 结果 |
|---|---|
| x11vnc 0.9.17 `-ultrafilexfer` | 全部命令 + 递归树 diff 一致 + 5 MiB 往返 md5 一致 |
| TightVNC Server 2.8.x（Windows） | 全部命令 + 递归 + 续传 md5 一致；GUI 连接、浏览、自测下载通过 |
| libvncserver + TightVNC 扩展 | ls/get/put/mkdir + 递归 |

## 实现要点 / 踩过的坑

- **RFB 3.3/3.6 的 None 认证不能回类型字节**：服务端把那个字节当成 ClientInit，整条流错位。
- **UltraVNC 下载需要回执**：收到 `FileHeader` 后回空的 `FileHeader`，服务端才发数据；`sizeH` 4 字节别漏读。
- **UltraVNC 上传没有结束确认**：发完 `EndOfFile` 再 round-trip 驱动列表，否则连接先关会截断。
- **TightVNC 2.x 的压缩是共享流的 `deflate(Z_SYNC_FLUSH)`**：无 Adler-32，inflate 只初始化一次并消费完每帧输入。
- **TightVNC 2.x 的文件名带内嵌 NUL**：不清掉会让路径拼接产生 `\0`，递归列表无限循环。
- **TightVNC 2.x 上传新文件必须 overwrite=1**（服务端靠 truncate 创建）。
- **TightVNC 2.x 的 FT 只在 Tight 安全类型连接上注册**。
- **TightVNC 1.3 的目录用 size=-1 表示**，且没有删除/重命名消息。
- **Qt 的 `signals` 宏会破坏 glib 结构体**：libsecret 头必须在 Qt 头之前 include。

## Commit 历史

1. `Import RFB core, UltraVNC/TightVNC backends, CLI and Qt GUI`
2. `GUI: symmetric dual-pane tree browsers with folder-first sorting`
3. `GUI: serial transfer queue with recursive directory jobs and cancel`
4. `GUI: drag-and-drop between panes for files and directories`
5. `Transfer: rate limiting, resume prompt, MD5 verification, keepalive, capability info, log dock`
6. `Connection: libsecret password storage, probe button, profiles`
7. `GUI: language switch and queue panel cleanup`

协议细节见 [research/PROTOCOL_SPEC.md](research/PROTOCOL_SPEC.md)。

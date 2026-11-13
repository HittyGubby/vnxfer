# vnxfer 协议规格（源码提取版）

所有字段均来自本地源码，不靠黑盒试探。项目符号：`→C` 客户端发，`S→` 服务端发；未注明字节序的整数均为**大端（网络序）**。

源码位置：`research/refs/{libvncserver,UltraVNC,tightvnc2,x11vnc}`。

---

## 0. 连接状态机（三种服务端通用骨架）

```
TCP
 └─ 版本握手（12 字节 ASCII "RFB 003.00x\n"）
     ├─ 3.3 / 3.6  → 安全类型 = uint32（4 字节）
     └─ 3.7 / 3.8  → 安全类型 = uint8 count + count×uint8
 └─ 安全握手
     ├─ None(1)        → 3.8 有 SecurityResult(u32)
     ├─ VNC(2)         → 16 字节挑战，DES-ECB，密码位反转后作 key
     └─ Tight(16)      → 见 §3.1（tunnel caps → auth caps → 认证）
 └─ ClientInit(u8 shared)
 └─ ServerInit(24 字节 + name)
     └─ [仅 Tight 安全类型] InteractionCaps（TightVNC 2.x / 1.3 都发，字段宽度不同）
 └─ 正常消息循环
     ├─ S→0 FramebufferUpdate   1 SetColourMapEntries   2 Bell   3 ServerCutText
     └─ 扩展：UVNC=type 7；TVNC2=首字节 0xFC + 3 字节 → u32 id；TVNC1.3=130..136
```

**教训**：不要用版本号判断能力。3.6/3.3 的 4 字节安全类型格式与 3.7+ 完全不同，混用会得到"security types 为空"的假象（我第一版探针就栽在这）。

---

## 1. 认证

### VNC (2)
```
key[i] = reverse_bits(password[i])   // 8 字节，不足补 0
challenge = S→ 16 字节
response  = DES-ECB(key, challenge[0:8]) || DES-ECB(key, challenge[8:16])
```

### Tight (16) —— 只有选它，TightVNC 2.x 才启用文件传输
```
S→ u32 nTunnelTypes
    [nTunnelTypes>0] nTunnelTypes × {u32 code, char vendor[4], char name[8]}   // "NOTUNNEL"
    [nTunnelTypes>0] C→ u32 code
S→ u32 nAuthTypes + nAuthTypes × cap
C→ u32 code      // ★ 发 code(2)，不是数组下标（发下标服务端直接断）
S→ 16 字节挑战 → 同 VNC
S→ u32 SecurityResult
```

来源：`tightvnc2/rfb-sconn/RfbInitializer.cpp:129-200`、`UltraVNC` 同构。

---

## 2. UltraVNC FileTransfer（RFB 消息类型 7）

### 2.1 头（12 字节）
```
u8 type=7 | u8 contentType | u8 contentParam | u8 pad | u32 size | u32 length | payload[length]
```
`contentParam` 实际只有 1 字节（libvncserver 的注释："UltraVNC forgot to Swap16IfLE"）。

### 2.2 消息表
| contentType | 名称 | 方向 | size 语义 | payload |
|---|---|---|---|---|
| 1 | DirContentRequest | C→ | 0 | path（**DOS 风格** `C:/x`） |
| 2 | DirPacket | S→ | 0 | 目录项 / 目录回显 / 空=结束 |
| 3 | FileTransferRequest | C→ | 1=要压缩 | path |
| 4 | FileHeader | 双向 | 文件大小 / -1 错误 | `"path,MM/DD/YYYY HH:MM"`，**后跟 4 字节 sizeH** |
| 5 | FilePacket | 双向 | 1=该包 zlib 压缩 | 数据 |
| 6 | EndOfFile | 双向 | 0 | — |
| 7 | AbortFileTransfer | 双向 | perm 探测：size=1 允许/-1 拒绝 | — |
| 8 | FileTransferOffer | C→ | 文件大小，**后跟 4 字节 sizeH** | `"path,时间"` |
| 9 | FileAcceptHeader | S→ | 0 成功 / -1 失败 | 回显 path |
| 10 | Command | C→ | — | cparam=1..6（mkdir/rmdir/create/delete/rename） |
| 11 | CommandReturn | S→ | 返回值 | 回显 path |
| 12 | FileChecksums | S→ | — | delta 用（libvncserver 未实现） |
| 14 | FileTransferAccess | S→ | 1 允许 / -1 拒绝 | — |

### 2.3 目录项 = `WIN32_FIND_DATA`，**小端**（`rfbserver.c:1359`）
```
u32 dwFileAttributes      // 0x10 目录, 0x80 普通
u32 ftCreationTime.lo, .hi     // Unix 秒
u32 ftLastAccessTime.lo, .hi
u32 ftLastWriteTime.lo, .hi
u32 nFileSizeHigh, nFileSizeLow
u32 dwReserved0, dwReserved1
char cFileName[...]        // 从偏移 44 开始
```
条目长度 = `44 + strlen(name) + 1`（服务端按 `sizeof(RFB_FIND_DATA)-MAX_PATH-14+strlen` 计算）。

> 字节序陷阱：这里服务端用 `Swap32IfBE`（LE 主机不交换）→ 线上小端；tight 1.3 用 `Swap32IfLE` → 线上大端。两套协议相反。

### 2.4 状态机
```
下载：C 3 → S 4(+sizeH) → C 4(空，表示"就绪") → S 5... → S 6
上传：C 8(+sizeH) → S 9 → C 5... → C 6
目录：C 1(cparam=1) → S 2... → S 2(cparam=0,len=0)
驱动：C 1(cparam=2) → S 2(cparam=3) payload="C:l\0\0"
权限：C 7(cparam>=1) → S 14(size=1/-1)；cparam=0 是旧版握手，会被拒
```

### 2.5 UltraVNC 新版（协议 v3/v4，`UltraVNC/rfb/rfbproto.h:1145`）
- S→17 `FileTransferProtocolVersion`：`contentParam`=版本(1..4)，v3+ 的 `size`=服务端块大小。
- C→15 / 16 SessionStart / SessionEnd（v3+）。
- v4：目录请求 `contentParam |= 0x8000` 表示支持 Unicode 目录项；条目中 `rfbADirUnicode` 标志；新增 `rfbADirInaccessible(18)`。
- 客户端策略：收到 17 才启用新行为，否则按 v2（读 4 字节 sizeH、ANSI 路径）。

---

## 3. TightVNC 2.x（0xFC0001xx）

### 3.1 启用条件（`rfb-sconn/RfbClient.cpp:246`）
```cpp
if (config->isFileTransfersEnabled() && rfbInitializer.getTightEnabledFlag())
```
- 必须 Tight 安全类型；普通 VNC auth 连接下发 0xFC 会被当未知消息断开（实测）。
- viewOnly 不注册。

### 3.2 InteractionCaps（ServerInit **之后**）
```
u16 nServerMsg, u16 nClientMsg, u16 nEnc, u16 pad
{ u32 code, char vendor[4], char name[8] } × (nServerMsg + nClientMsg + nEnc)
```
注意 code 是 **u32**（TightVNC 1.3 用的是 u16 code + 8 字节 name，见 §4）。

### 3.3 字符串
`StringUTF8 = u32 len + UTF-8 bytes`（`io-lib/DataOutputStream.cpp:120`）。

### 3.4 客户端 → 服务端
| ID | 字段（源码 `ft-client-lib/FileTransferRequestSender.cpp`） |
|---|---|
| 0xFC000100 | 无体 |
| 0xFC000102 | u8 level, UTF8 path |
| 0xFC000104 | UTF8 path, u64 offset, u64 size |
| 0xFC000106 | UTF8 path, u8 overwriteFlag, u64 offset |
| 0xFC000108 | u8 level, u32 compressedSize, u32 uncompressedSize, data |
| 0xFC00010A | u16 fileFlags, u64 modTime（毫秒） |
| 0xFC00010C | UTF8 path, u64 offset |
| 0xFC00010E | u8 level, u32 size |
| 0xFC000111 / 113 / 115 | UTF8 path（/ 两个 UTF8：old,new） |
| 0xFC000117 | UTF8 path |

### 3.5 服务端 → 客户端（`ft-client-lib/FileTransferReplyBuffer.cpp`）
| ID | 字段 |
|---|---|
| 0xFC000101 | u8 isCompressionSupported |
| 0xFC000103 | u8 level, u32 csize, u32 usize, block |
| 0xFC00010D | 无体 |
| 0xFC00010F | u8 level, u32 csize, u32 usize, data |
| 0xFC000110 | u8 fileFlags, u64 modTime |
| 0xFC000118 | u64 dirSize |
| 0xFC000119 | UTF8 错误消息 |
| 其余 reply | 无体（仅 ID） |

目录块（解压后）：
```
u32 count
count × { u64 size, u64 mtime(ms), u16 flags, UTF8 name }
flags: 0x1 目录, 0x2 可执行
```

### 3.6 路径
`/C:/Users/x`、盘符根 `/C:`、Unix 根 `/`；分隔符 `/`。文件名相对父目录，不含前导 `/`。

### 3.7 状态机
```
下载：C 0x10C(path,offset) → S 0x10D → 循环{C 0x10E(size) → S 0x10F(block)} → S 0x110(flags,mtime)
上传：C 0x106(path,overwrite,offset) → S 0x107 → 循环{C 0x108(data) → S 0x109} → C 0x10A(flags,mtime) → S 0x10B
列表：C 0x102(path) → S 0x103
失败：S 0x119(UTF8 msg)
```

---

## 4. TightVNC 1.3（消息 130–136）

前提：3.7t 扩展（tunnel caps + auth caps + interaction caps）。cap 结构 = `u16 code + char vendor[4] + char name[8]`。

服务器宣告（`rfbtightproto.h:229`）：130 FTS_LSDT / 131 FTS_DNDT / 132 FTS_UPCN / 133 FTS_DNFL；
客户端宣告：130 FTC_LSRQ / 131 FTC_DNRQ / 132 FTC_UPRQ / 133 FTC_UPDT / 134 FTC_DNCN / 135 FTC_UPFL。

### 4.1 消息体（全部大端）
```
FileListRequest   (C 130): u8 type, u8 flags, u16 dirNameSize, char name[]
FileListData      (S 130): u8 type, u8 flags, u16 numFiles, u16 dataSize, u16 compressedSize,
                            numFiles × {u32 size, u32 mtime}, names（NUL 分隔，共 numFiles 个）
                            dataSize = len(names)+numFiles；未压缩时 compressedSize=dataSize
FileDownloadReq   (C 131): u8 type, u8 compressLevel, u16 fNameSize, u32 position, char name[]
FileDownloadData  (S 131): u8 type, u8 compressLevel, u16 realSize, u16 compressedSize, data
                            若 realSize==compressedSize==0 → 后跟 u32 modTime（文件结束标记）
FileUploadReq     (C 132): 同 DownloadReq
FileUploadData    (C 133): u8 type, u8 compressLevel, u16 realSize, u16 compressedSize, data
FileDownloadCancel(C 134): u8 type, u8 unused, u16 reasonLen, reason[]
FileUploadFailed  (C 135): u8 type, u8 unused, u16 reasonLen, reason[]
FileCreateDirReq  (C 136): u8 type, u8 unused, u16 dirNameSize, name[]
```

### 4.2 根目录语义
`ConvertPath()` 把客户端路径直接拼在 `ftproot`（默认 `$HOME`）后面（`handlefiletransferrequest.c:233`）。客户端传 `tmp` 才是 `$HOME/tmp`；传 `/tmp` 会变成 `$HOME/tmp` 之外的错误路径。这与 UltraVNC 的 `C:` 语义完全不同。

> 我上一轮把 `/tmp` 直接发过去得到 numFiles=0，就是这个原因，不是协议问题。

---

## 5. 版本探测与能力降级（跨版本核心）

| 协议族 | 版本信号 | 客户端动作 |
|---|---|---|
| UltraVNC | S→17 消息 | 收到：按 contentParam 用 v3/v4 行为；未收到：按 v2（sizeH、ANSI） |
| UltraVNC ≤RC18 | 权限探测回 `7`（Abort）而非 `14` | 无 sizeH，路径/大小按 v1 |
| TightVNC 2.x | InteractionCaps 里出现 0xFC0001xx | 有 0x104 → 可断点续传；无 0x100 → 关压缩 |
| TightVNC 1.3 | InteractionCaps 里出现 130–135 | 走 FTS 后端 |
| 无 FT | 无上述任何信号，或 FT 请求后断开 | 明确报"服务端未启用文件传输"，不要重试 |

**不要按版本号分支**，只按能力分支。这样 UltraVNC 1.0–1.4、TightVNC 2.0–2.8、TightVNC 1.3 全都能落在三条后端内。

---

## 6. 待补的测试矩阵（需要装的服务端）

| 目标 | 版本 | 端口建议 | 覆盖点 |
|---|---|---|---|
| UltraVNC Server（Windows） | 1.0.2（v1） | 5902 | 无 sizeH、旧权限 |
| UltraVNC Server | 1.2.0.9（v2） | 5903 | 基准 |
| UltraVNC Server | 1.4.x（v4） | 5904 | session、unicode、inaccessible |
| TightVNC Server（Windows） | 1.3.10 | 5905 | FTS 130-136 |
| TightVNC Server | 2.0 / 2.5.2 | 5906 | 2.x 早期 caps 差异 |
| TightVNC Server | 2.8.84 | 5900/5901（已有） | 基准 |
| x11vnc（自编译 tight 宏） | 0.9.17 | 5907 | Linux FTS 服务端 |
| TigerVNC / RealVNC | 任意 | 5908 | 无 FT 时的降级 |

> 已有环境实测：5900 与 5901 的 caps 完全一致（nsrv=14/cli=13/enc=8，desktop 都是 ej-100），是同一个 TightVNC 2.8.x，不是两个版本。

---

## 7. 我这次踩过的坑（写进实现前的检查表）

1. 3.3/3.6 的安全类型是 u32，不是列表。
2. Tight auth 选 **code**，不是下标。
3. TightVNC 2.x 的 caps 在 ServerInit 之后。
4. UVNC 的 fileHeader 后面多 4 字节 sizeH；目录项是小端。
5. UVNC 下载必须回一个空 fileHeader 才继续。
6. Tight 1.3 的 ftproot 是 HOME 前缀。
7. 两套协议的字节序相反（UVNC 小端 / TVNC 大端）。
8. x11vnc 的 -tightfilexfer 在本机是空操作（见 §8）。

## 8. x11vnc 的 tight 支持真相

- tight 实现位于 **libvncserver**（`src/libvncserver/tightvnc-filetransfer/`），不在 x11vnc 里——你猜的方向对，但结论反过来：x11vnc 只是调用者。
- Arch 的 `x11vnc 1:0.9.17-1` 编译时没定义 `LIBVNCSERVER_WITH_TIGHTVNC_FILETRANSFER`：`strings /usr/bin/x11vnc | grep rfbRegisterTightVNC` = 0 条，`-verbose` 也没有 "rfbRegisterTightVNCFileTransferExtension: 6" 日志。所以 GUI 里的 tight 选项是灰的（能点，不生效）。
- 但**库本身支持**：我写了个 30 行的 libvncserver 服务端（`research/refs/tight13_server.c`），调用 `rfbRegisterTightVNCFileTransferExtension()` 后，3.8 连接拿到 `sec types [0x1,0x10]` 与 `caps 130–135`。这证明协议可测，只是 x11vnc 打包没接上。
- 要本机测 tight 1.3：自编译 x11vnc 时加 `-DLIBVNCSERVER_WITH_TIGHTVNC_FILETRANSFER`，或用该测试服务端。

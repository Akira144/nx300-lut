# NX300 机内 LUT（JPEG 直出）

在 **NX300（非 M）+ 固件 1.45** 上，让相机拍出来的 JPEG 直接带上 3D LUT 的样子。

不刷固件、不改 `di-camera-app`、不动 UBIFS。全部动作发生在 SD 卡上：开机时 `autoexec.sh` 以 root 启动一个常驻小程序 `lutd`，它盯着卡上新增的 `*.JPG`，等相机把文件写完关闭后，就地用 `.cube` 重新编码一遍。

```
拍摄 -> 相机写 SAM_xxxx.JPG -> inotify 收到"写完了"
     -> 解码 -> 查 3D LUT（三线性插值，纯整数运算）-> 重编码 -> 原子替换
```

---

## 0. 先看清楚边界

**能做到**：JPEG 直出带 LUT，文件就是相机里那张，可以直接拔卡、Wi-Fi 传手机、插电脑。

**做不到**：

- 取景/LCD 上**看不到** LUT 效果。你只能拍完看结果（或者拿手机连着看传过去的文件）。这是闭源 ISP 管线的限制，不是这段代码的锅。
- 只作用于 JPEG。RAW 不受影响，也不处理。
- 处理是"串行"的：一张 20 MP 大概几秒，连拍时任务会排队，等它处理完再关机会更安全。
- JPEG 是 8 bit，而且相机已经先把 Picture Wizard / Smart Filter "烤"进去了。**请把机内 Picture Wizard 设为 Standard、Smart Filter 关掉**，否则等于调了两次色。

---

## 1. 文件清单

```
nx300-lut/
├── autoexec.sh              # 放到 SD 卡根目录（LF 换行！）
├── lutd.conf                # 放到 SD:/lut/lutd.conf
├── src/lutd.c               # 全部逻辑，一个文件
├── Makefile                 # make（电脑测试用）/ make arm（相机用）
└── README.md
```

SD 卡最终布局：

```
/autoexec.sh
/lut/lutd                    <- 交叉编译出来的静态 armel 二进制，chmod +x
/lut/lutd.conf
/lut/look.cube               <- 你的 LUT
```

---

## 2. 第 0 步：先确认 1.45 上 autoexec.sh 还能跑（零风险，必做）

这一步不装任何东西，只是让相机开机时写个日志。**在确认这步通过之前，别做后面的事。**

1. 复制 `autoexec.sh` 到卡根目录。
2. 确认换行是 **LF**：

   ```bash
   file autoexec.sh          # 应显示 ASCII text，不带 CRLF
   sed -i 's/\r$//' autoexec.sh
   ```

3. 相机装卡、开机，等 10 秒，关机，拔卡插电脑。
4. 卡根目录应该出现 `autoexec.log`，内容里有 `uid=0(root)`。

结果判断：

- **有 log 且是 root** → 继续第 3 步。
- **没有 log** → 按顺序排查：换行是不是 CRLF；文件名/位置对不对；卡是 FAT32 还是 exFAT（都行，但别用 NTFS）；再把 `autoexec.sh` 复制一份改名 `auto_run.sh` 一起试（固件里 `libstorage.so` 也引用了这个名字）。仍然不行的话，用 [nxfiles 的固件存档](https://nxfiles.nx.tc/files/NX300/Firmware/) 降到 1.40/1.41 再试——社区在 NX300（非 M）上没有必须降级的报告，但 1.45 是个很晚的版本，值得留个后手。

> 别在 NX300 上刷 NX300M 的改版固件（比如 `NX300M-MFW`），型号不同，砖了没有自救手段。

---

## 3. 第 1 步：先在电脑上编译并验证（必做）

**这份代码我这边没能编译验证**（当前环境的 shell 起不来），所以务必先在本机跑通，再去交叉编译。WSL2 + Ubuntu 最省事：

```bash
sudo apt update
sudo apt install -y build-essential libjpeg-turbo8-dev

cd nx300-lut
make
./lutd --once ~/Pictures/test.JPG ~/Pictures/test_out.JPG ~/looks/kodak.cube 95
```

看 `test_out.JPG`：

- 颜色对不对、有没有偏色/断阶；
- `exiftool test_out.JPG` 里 EXIF（日期、镜头、方向）还在不在；
- 文件大小变化（q95 通常比原图大 10–40%）。

这一步过了，说明 LUT 解析、插值、JPEG 重编码都正常。

如果手头还没有 `.cube`，先用这个 17³ 的暖色测试 LUT 验证管线（真实风格还是得用正经的 33³ cube）：

```python
# gen_test_lut.py
n = 17
print('TITLE "warm test"')
print(f"LUT_3D_SIZE {n}")
for b in range(n):
    for g in range(n):
        for r in range(n):
            R, G, B = r/(n-1), g/(n-1), b/(n-1)
            print(f"{min(1.0, R*1.06):.6f} {G:.6f} {min(1.0, B*0.90):.6f}")
```

```bash
python3 gen_test_lut.py > warm.cube
./lutd --once test.JPG out.JPG warm.cube
```

（`.cube` 的 3D 表是 **R 变化最快**，上面这个循环顺序就是对的，正好也能验证解析器。）


---

## 4. 第 2 步：交叉编译 armel 静态版

NX300 是 **armel（软浮点 EABI）**，用 `arm-linux-gnueabi-`，不要用 `gnueabihf`。静态链接，避免和相机里那套老 glibc 打架。

因为要静态链接 libjpeg，先把 libjpeg-turbo 交叉编译一份（关闭 SIMD，免得装 NASM）：

```bash
sudo apt install -y gcc-arm-linux-gnueabi cmake git

git clone --depth 1 https://github.com/libjpeg-turbo/libjpeg-turbo.git
cmake -S libjpeg-turbo -B build-arm \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER=arm-linux-gnueabi-gcc \
  -DWITH_SIMD=0 -DENABLE_SHARED=0 -DWITH_TURBOJPEG=0 \
  -DCMAKE_INSTALL_PREFIX=$PWD/arm-jpeg
cmake --build build-arm -j"$(nproc)"
cmake --install build-arm

cd nx300-lut
make arm CROSS=arm-linux-gnueabi- JPEG_ARM=$PWD/../arm-jpeg
file lutd.arm
# 期望：ELF 32-bit LSB executable, ARM, EABI5, statically linked
```

产物大概 700 KB – 1 MB，正常。

---

## 5. 第 3 步：装到卡上

```
/autoexec.sh          （第 0 步那个，已改好）
/lut/lutd             （lutd.arm 改名，chmod +x）
/lut/lutd.conf
/lut/look.cube        （你的 LUT，33³ 最稳；65³ 也能跑）
```

`lutd.conf` 默认就是"就地替换 + 保留原图到 `.lut/orig`"，先这么用。等你确认稳定了，再把 `KEEP_ORIG=1` 改成 `0`（省一半空间）。

---

## 6. 第 4 步：实测

1. 装卡，开机（日志里应出现 `started lutd`）。
2. 拍 1 张，等 5–10 秒（让它处理完）。
3. 关机，拔卡插电脑，看：
   - `/mnt/mmc/lutd.log`：应有 `apply ... done`；
   - `DCIM/100PHOTO/SAM_xxxx.JPG`：颜色已经变了；
   - `.lut/orig/SAM_xxxx.JPG`：原图还在（可用来对比）；
   - `lutd.out`：进程的标准输出/错误，出问题时看这个。

日志正常之后，就可以正常拍照了。**关机前留几秒**，避免最后一张还在处理。

---

## 7. 性能、画质、已知限制

| 项 | 说明 |
|---|---|
| 速度 | 20 MP 一张大约 2–8 秒（单核整数插值 + libjpeg 解码/编码）。连拍会排队 |
| 体积 | q95 重编码后通常大 10–40%，卡消耗更快；想省空间把 `QUALITY` 调到 92 |
| 色度 | 重编码保留原图的 4:2:0/4:4:4 采样；RGB→YCbCr 往返在 q95 下肉眼不可见 |
| 缩略图 | `KEEP_META=1` 原样复制 EXIF，所以 **EXIF 里的缩略图还是没调色的**。Windows 资源管理器可能显示旧缩略图，相机回放和正经看图软件解的是主图，是调过色的。介意的话可以后续加"重建 EXIF 缩略图" |
| 预览 | 无。LCD 上看不到 LUT |
| 断电 | 处理中关机会留下 `.lutd-*.tmp`（无害），下次开机可以直接删 |
| 功耗 | 常驻一个几乎不占 CPU 的进程，只在拍照后忙几秒 |

---

## 8. 排错

| 现象 | 处理 |
|---|---|
| `lutd.log` 里 `cannot open ...cube` | 路径写错，或文件没拷进去。用绝对路径 `/mnt/mmc/lut/look.cube` |
| `not complete yet, skip` | 相机还没把文件写完/没关文件句柄。偶发一条无所谓；频繁出现就把 `MIN_BYTES` 调大 |
| `skip progressive jpeg` | NX300 不会出渐进式 JPEG；出现了说明文件异常 |
| `jpeg: ...` 报错 | 那张图被跳过了，原文件没动。查 `lutd.out` |
| 一张都没处理 | 先手动验证：`/mnt/mmc/lut/lutd --once /mnt/mmc/DCIM/100PHOTO/SAM_0001.JPG /mnt/mmc/t.jpg /mnt/mmc/lut/look.cube`（需要 telnet 或改 autoexec 跑一次） |
| 相机变慢/卡顿 | 处理是串行的，拍完等几秒；或把 `QUALITY` 降一点 |

---

## 9. 回滚

删掉卡根目录的 `autoexec.sh`（或改名 `autoexec.sh.off`），重启，世界恢复原样。这个方案从头到尾没有碰过固件，不存在"刷坏了"的状态。

原图在 `.lut/orig/`，随时可以搬回去。

---

## 10. 拍 RAW 也能出 JPEG（SRW 模式）

`lutd.conf` 里 `SRW=1` 时，拍 `.SRW` 也会自动出图：从 SRW 里抽出内嵌的那张 **5472×3648 全尺寸 JPEG**（就是你探针里 `[2] 0x8efd0` 那张，由 SRW 的 `JPEGInterchangeFormat` 指向），套 LUT，存成同名 `.JPG`。

实测两张 SRW 的数据：

| 项 | 值 | 含义 |
|---|---|---|
| 预览尺寸 | 5472 × 3648 | 全尺寸，路线可行 |
| 预览大小 | 1.96–2.17 MB | 约 0.77 bit/像素，**压缩比正常 JPEG 高很多** |
| 预览编码 | Baseline，采样 2x1（4:2:2），无 EXIF | 解码内存友好；4:2:2 会被原样保留 |
| RAW 本体 | 12 bit，`Compression 32770`（三星自家压缩），5546 × 3714 | 单通道 CFA，RGGB |
| 参考 JPEG（Fine） | 约 6–8 MB | 画质明显更好 |

**预览没有 EXIF**，而相机并不会把预览转正（`tag274 = 8` 的竖拍照片，预览仍是 5472×3648 横向）。所以 lutd 会**合成一段最小 EXIF** 写进生成的 JPEG：Make/Model、`Orientation`（来自 SRW 的 274）、`DateTime`、`DateTimeOriginal`（来自 SRW 的 306/36867）。这样竖拍不会躺倒，资源管理器里也有拍摄日期。

所以顺序是：

1. **想要画质**：拍 **RAW+JPEG**，`lutd` 只处理相机写的那张真 JPEG。SRW 检测到同名 `.JPG` 会直接跳过，不会重复处理。
2. **只拍了 RAW**（忘了切模式、或卡上只想放 RAW）：`SRW=1` 给你一张 2 MB 预览级别的带 LUT 的图，细节和暗部会肉一些，但至少有直出图，方向和日期是对的。

处理前会等 1.5 秒看有没有同名 `.JPG` 出现，所以 RAW+JPEG 模式不会误判。SRW 文件本身**永远不被修改**。

---

## 11. 后面还能加什么

- **多 LUT 切换**：读 `/mnt/mmc/lut/active`（里面写文件名），拍照前改一行就换风格，不用重刷卡。
- **重建 EXIF 缩略图**：解决资源管理器显示旧图的问题（要做 IFD1 重写，工作量中等）。
- **只处理新照片**：现在按"文件关闭"触发，天然只处理新的；如果你在电脑上往卡里拷图，也会被处理，注意这一点。
- **4:4:4 输出**：把 `h_samp/v_samp` 强制成 1，画质略好、体积更大。

---

## 附：为什么不是"改固件"

NX300 的色彩渲染在闭源 ISP 管线里（`di-camera-app-nx300`、`libcapture-fw-slpcam-nx300`、`d4library`、`libmm-photo`），没有插件接口。社区十几年来的成果只有两类：SD 卡 payload（本方案）和重打包固件（有人做出过 `NX300_FW_v1.45c_reclimit_repack`，证明可行）。要"取景实时 LUT + 直接烧进 JPEG"，得反汇编改闭源二进制再重打包，是 Magic Lantern 级别的工程，且变砖无自救。本方案是同等效果下风险最低的路径。

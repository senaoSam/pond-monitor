# 交接筆記（2026-09-01）

給接手的 session。專案背景看 [README.md](README.md)，這裡只寫**當下狀態**和
**踩過的坑**。

正在追的問題：**B 無法遠端更新**（`ESP_ERR_OTA_VALIDATE_FAILED`）。
這題 08-31 曾被誤判為結案，09-01 證實仍在。

## 系統現況

| | A | B |
|---|---|---|
| DEVICE_ID | `pond-site` | `watchdog` |
| 板子序號 | CH343 `5CBC033443` | CH343 `5CBC033428` |
| IP | 192.168.0.37 | 192.168.0.38 |
| 韌體版本 | **6** | **28**（USB 燒入） |
| 拉取式 OTA | ✅ 正常（v5→v6 自我更新成功） | ❌ **失敗中**，見下 |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |

兩片都在使用者台北家中運作，資料持續進 RTDB。**功能都正常** ——
B 的告警邏輯 09-01 實測過（拔掉 A，5 分鐘門檻正確觸發，A 回來後自動解除）。

**唯一問題是 B 無法遠端更新**，v28 是 USB 燒進去的。
工作目錄的原始碼是 v29（已 commit，release 已發布，但 B 裝不上）。

09-01 凌晨的實測進度：Discord user id 已修正並**確認送達成功**
（`discord notified for pond-site (msg 1544030776239194163)`）；
告警偵測邏輯實測正確；OTA 卡死問題已修好（下載中斷不再讓板子當機）。
剩下的就是 `VALIDATE_FAILED` 本身。

最終部署：A 去魚塭、B 去南部家裡，開發者在北部 —— 所以兩片都必須能遠端更新。
**B 現在不符合這個前提，出門前必須修好。**

## 未解決：B 的 OTA `ESP_ERR_OTA_VALIDATE_FAILED`

**狀態：根因不明。已誤判七次，不要再憑推論下結論。**

A（`pond-site`）遠端更新正常。B（`watchdog`）不行，只能 USB 燒錄。
兩片跑同一份 `shared/PullOta`、同樣的 `platformio.ini`。

### 目前的症狀

兩種失敗交替出現，取決於下載是否完整：

```
# 下載中途斷線（我加的逾時保護正確接住，板子不再當機）
fw pull: stalled at 751474/971120

# 下載完整時
install failed: Could Not Activate The Firmware slot_hdr=E905024F
  app0_state=2(magic=E9 chip=9 seg=5)
  app1_state=ERR:ESP_ERR_NOT_FOUND(magic=E9 chip=9 seg=5)
  | target=app1@0x650000 size=6553600
  | set_boot_partition=5379 (ESP_ERR_OTA_VALIDATE_FAILED) running=app0
```

序列埠（原生 USB 孔，COM5）另外拍到 bootloader 的抱怨：

```
E (115505) esp_image: Image hash failed - image is corrupt
```

### 已用 esptool 實測排除（不是推論，不要重查）

| 項目 | A | B | 結果 |
|---|---|---|---|
| flash 晶片 | `c2 2018` 16MB quad 3.3V | 同左 | **相同** |
| chip revision | v0.2 | v0.2 | **相同** |
| bootloader `0x0-0x8000` | sha256 `6ac230d6941c798d…` | 同左 | **逐 byte 相同** |
| 分區表 `0x8000` | app0@0x10000 app1@0x650000 各 0x640000 | 同左 | **逐 byte 相同** |
| **otadata `0xe000`** | `seq=3`+`seq=2`，兩 sector 皆 VALID | **`seq=1`+ 空白** | **← 唯一差異** |

**最關鍵的一項：** 失敗後把 B 的 app1 dump 出來（`read_flash 0x650000 0xEE000`），
與 GitHub 上的 release **逐 byte 比對，971408 bytes 全部相同，零個不符**。

**寫進去的資料是完美的，bootloader 卻說 hash 失敗。** 這是目前最核心的矛盾，
也是唯一還沒被解釋的事實。

其他已排除：PSRAM 宣告、heap 碎片化、分區表設定、`maximum_size` 覆寫、
anti-rollback（`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 未啟用；
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 是開的）、映像格式、寫入方向、
供電不足（換 USB 3.0 孔後不再當機，但 OTA 仍失敗）。

### 唯一還沒查的差異：otadata

B 的 otadata 第二個 sector 從未被寫過。ESP-IDF 的 OTA 記帳是雙緩衝，
兩個 sector 輪流寫、靠 `ota_seq` 遞增決定新舊。A 的 `seq=3`/`seq=2`
顯示它輪替過多次；B 停在 `seq=1`。

**但因果方向未定** —— 可能是 B 一直失敗所以 seq 停住（結果而非原因）。

下一步的候選動作（**未執行，需使用者同意**）：
`esptool erase_region 0xe000 0x2000` 抹掉 B 的 otadata，讓記帳從零重建。
抹掉後仍會從 app0 開機（v28 在那裡），風險低但不可逆。

otadata 解析方式：每 `0x1000` 一份，前 4 bytes `ota_seq`、offset 24 `ota_state`、
offset 28 CRC32（`zlib.crc32(seq_bytes, 0xffffffff)`，**不做最後 xor**）。
狀態：`0x0 NEW`、`0x1 PENDING_VERIFY`、`0x2 VALID`、`0x3 INVALID`、
`0x4 ABORTED`、`0xFFFFFFFF UNDEFINED`。

### 七次誤判（全部被後續證據推翻）

1. 「B 沒有 PSRAM」→ eFuse 證明有 8MB
2. 「`psram_type` 設錯該用 qio」→ qio 也失敗
3. 「heap 碎片化」→ 208KB 連續空間充足
4. 「分區被標記 INVALID」→ 當時以錯誤碼不符否定（方向其實最接近）
5. 「PSRAM 宣告是唯一剩下的變數」→ `/fwlog` 證明 v10 無宣告也失敗
6. 「otadata `state=0x2` 是 PENDING_VERIFY」→ **看錯 enum**，0x2 是 VALID
7. 「v19 加的 target erase 是元凶」→ 移除後（v28）仍失敗。
   當時只看「v13/v14 在它之前成功」就下結論，**沒去核對 v10→v11
   在它之前也失敗過** —— 反例一直在 fwlog 裡

**共同模式：拿 A/B 的片面差異當因果，樣本不足就宣告結案。**
v13/v14 兩次成功就寫下「已結案」，是浪費最多時間的一次。

### 給接手者的建議

不要再提第八個假說。資料寫入正確卻驗證失敗，這個矛盾指向
**驗證時的讀取路徑**（flash 快取、mmap、SPI 時序），而不是資料內容。
可考慮：在板子上直接呼叫 `esp_image_verify()` 並印出它拒絕的段落與偏移量。

## 部署前必須完成（東西還在台北時才能做）

使用者的顧慮：**之後 A 去魚塭、B 去南部，只能遠端更新，接不到板子。**
以下每一項都要在手邊時驗證完，出門後就沒機會了。

### 會導致「必須接板子」的情境

| 情境 | 現況 | 能否遠端救 |
|---|---|---|
| B 的 OTA 壞掉 | **現在就是壞的** | ❌ **出門前必須修好** |
| 推了開機就當的韌體 | rollback 會自動退回 | ✅ |
| 推了能開機但邏輯壞掉的韌體 | 只要還能輪詢 RTDB | ⚠️ 勉強 |
| WiFi 密碼改了 / 換路由器 | 寫死在 `secrets.h` | ❌ **必須接板子** |
| RTDB 網址或憑證失效 | 寫死 | ❌ **必須接板子** |
| GitHub release 下載失敗 | 會重試 | ✅ |
| NVS 損壞 | 未處理 | ❌ |
| 電源不穩寫壞 flash | 未處理 | ❌ |

### 待辦（依優先序）

- [ ] **修好 B 的 OTA** —— 阻塞項，沒解決就不能部署（根因不明，見上）
- [ ] 序列埠除錯已可用：`Serial.begin()` 在 v24 加入，走**原生 USB 孔**
      （COM5，VID 303A）；燒錄走 **CH343 孔**（COM6）。兩者要換孔
- [ ] **WiFi 後備連線** —— 清單裡最可能真實發生的。連不上主 WiFi 超過 N 分鐘
      就開 AP，用手機連上去改設定。能救掉整類「網路變動」情境
- [ ] **破壞性測試（只有現在能做）**：
  - [ ] 推一版故意在 `setup()` crash 的 → 驗證 rollback 能自動退回
  - [ ] 推一版故意連不上 WiFi 的 → 驗證能否自救
  - [ ] OTA 進行到一半拔電 → 驗證半寫入狀態能恢復
  - [ ] 改掉 WiFi 密碼 → 驗證後備機制
- [ ] A 也要更新到含韌性修正的版本（目前 v6，跑舊的 `shared/PullOta`）

## 使用者的偏好與已定決策

- **回覆用繁體中文。** 曾經誤用簡體字（断/静），已修正為 斷/靜
- 使用者熟 JS/Node，韌體交給 AI 實作，只在關鍵時刻 review
- 不要一直提醒等待中的事，超時沒關係，之後再看
- 判斷過的事不要重問：RTDB（非 Firestore）、公開 repo、憑證外洩可接受、
  不做溫度門檻告警、只做心跳、濕度已移除
- 之後會大量採購 ESP32，所以**根因比繞過更有價值**
- 每天人工檢查兩次，所以 B 掛掉會被發現，不必做雲端監控

## 硬體注意事項

- **兩片都需手動 BOOT+RST 才能 USB 燒錄**（CH343 無自動 reset 電路）
- **A 的原生 USB 孔（左）不能傳資料**，推測焊接影響 GPIO19/20。用 CH343 孔（右）
- **B 的原生 USB 孔可用**，進下載模式後 PID 由 4001 變 1001，埠號會變
- **細的 USB 線會讓 A 連不上 WiFi**（電壓降）。A 要用品質好的線
- 後續焊接**避開 GPIO19/20**（S3 原生 USB 固定用這兩支腳）

## 其他待辦（部署之後才做）

- [x] ~~`/fwlog` 尚無紀錄~~ 兩片都有紀錄了：
      `curl https://<RTDB>/fwlog/watchdog.json`（`pond-site` 同理）
- [ ] 前端圖表（使用者主場）：讀 `history/pond-site/<YYYY-MM>`，`orderByKey` 取範圍
- [ ] C 節點：燒 watchdog 韌體、改 `DEVICE_ID`，B/C 自動互相監控
- [ ] 溶氧、pH：可掛同一條 RS485（改站號），或依地理位置分組
- [ ] RTDB 規則全開放（Spark 方案最壞是服務中斷非計費）。**升 Blaze 前必須收緊**

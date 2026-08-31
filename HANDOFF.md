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
| 韌體版本 | **6** | **20** |
| 拉取式 OTA | ✅ 正常（v5→v6 自我更新成功） | ❌ **失敗中**，見下 |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |

兩片都在使用者台北家中運作，資料持續進 RTDB。**功能都正常** ——
B 的告警邏輯 09-01 實測過（拔掉 A，5 分鐘門檻正確觸發，A 回來後自動解除）。

**唯一問題是 B 無法遠端更新**，v20 是 USB 燒進去的。

最終部署：A 去魚塭、B 去南部家裡，開發者在北部 —— 所以兩片都必須能遠端更新。
**B 現在不符合這個前提，出門前必須修好。**

## 未解決：B 的 OTA `ESP_ERR_OTA_VALIDATE_FAILED`

**這題在 08-31 被標成「已結案」，那是錯的。** 09-01 又以完全相同的錯誤失敗，
`v19 -> v20` 是在 USB 乾淨燒錄後、從 app0 寫 app1 的最乾淨情境下失敗的。

### 症狀

```
install failed: Could Not Activate The Firmware slot_hdr=E905024F
  | set_boot_partition=5379 (ESP_ERR_OTA_VALIDATE_FAILED) running=app0
```

A（`pond-site`）用同一份 `shared/PullOta`、同樣的 platformio 設定，OTA 正常。
只有 B 會失敗，而且 USB 燒錄後能成功一到兩次，之後固定失敗。

### 已用 esptool 直接驗證的硬體事實（09-01，不是推論）

這些都查過了，都正常，不必重查：

- flash 晶片真的是 16MB（`Manufacturer: c2 Device: 2018`，eFuse quad / 3.3V）
- 分區表 `default_16MB.csv`：app0 `0x10000`、app1 `0x650000`，各 `0x640000`
- otadata（`0xe000`）內容正常：`ota_seq=1`、`ota_state=0x2`（**VALID**）、CRC 正確
- 寫入資料本身沒問題：`slot_hdr=E905024F`，magic `E9` 正確

也就是說：來源分區狀態正常、目標分區寫得進去、記帳資料完好，**卻仍被拒絕**。

### 下一步該怎麼查

**不要再提假說了。** 這題已經誤判六次（見下），每次都是從「A 和 B 哪裡不一樣」
推論因果、證據不足就宣告結案。

該做的是**直接比對 A 和 B 的可觀測狀態**：

1. 把 A 的 otadata（`0xe000`, `0x2000`）也 dump 出來，和 B 逐欄位比對
2. 比對兩片的 bootloader（`0x0`, `0x8000`）與分區表（`0x8000`, `0x1000`）二進位
3. 一片能更新、一片不能，差異必然落在某個可讀取的地方 —— 找出來，不要猜

dump 指令（板子需 BOOT+RST 進下載模式）：

```
python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3   --port COMx --no-stub read_flash 0xe000 0x2000 otadata.bin
```

otadata 解析：每 `0x1000` 一份，前 4 bytes `ota_seq`、offset 24 是 `ota_state`、
offset 28 是 CRC32（`zlib.crc32(seq_bytes, 0xffffffff)`，**不做最後的 xor**）。
狀態值：`0x0 NEW`、`0x1 PENDING_VERIFY`、`0x2 VALID`、`0x3 INVALID`、
`0x4 ABORTED`、`0xFFFFFFFF UNDEFINED`。

### 已排除、不必重試

- 分區表設定、`board_upload.maximum_size` 覆寫
- PSRAM 宣告（`/fwlog` 證明 v10 無宣告也失敗；兩種狀態都失敗過）
- heap 碎片化（`largest free block` 208KB，串流寫入不需連續空間）
- anti-rollback（`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 未啟用）
- otadata 損壞（09-01 dump 出來是好的）
- flash 容量造假（esptool 讀到真 16MB）
- task watchdog 拖慢導致回滾（v19 在無告警、loop 正常的情況下同樣失敗）

### 這題上犯過的錯（六次，避免重複）

1. 「B 沒有 PSRAM」→ eFuse 證明有 8MB
2. 「`psram_type` 設錯」→ `qio` 也失敗
3. 「heap 碎片化」→ 208KB 連續空間充足
4. 「分區被標記 INVALID」→ 錯誤碼不符（但方向其實最接近）
5. 「PSRAM 宣告是唯一剩下的變數」→ `/fwlog` 證明 v10 無宣告也失敗
6. 「otadata 的 `ota_state=0x2` 是 PENDING_VERIFY」→ **看錯 enum**，0x2 是 VALID

共同模式：**拿片面差異當因果，樣本不足就宣告結案。** v13/v14 兩次成功就寫下
「已結案」，是這次浪費最多時間的根源。

### 已完成的韌性修正（在 v19/v20 裡，與根因無關但值得保留）

- 無條件呼叫 `esp_ota_mark_app_valid_cancel_rollback()`，不再只在
  `PENDING_VERIFY` 時才標記
- 寫入前直接 `esp_partition_erase_range()` 抹目標分區，不依賴
  `esp_ota_erase_last_boot_app_partition()`（後者只作用於「上次開機的分區」，
  且要求當前 app 已 valid）
- 失敗版本改為重試 3 次（`OTA_BAD_RETRY_LIMIT`），不再永久黑名單 ——
  原本無法區分「真的開機失敗」與「下載後斷電」
- **OTA 輪詢移到 `runCheck()` 之前** —— 遠端更新是唯一的救援管道，
  不該排在最會塞車的工作後面
- Discord 發送失敗改指數退避 —— 原本 `RENOTIFY_S` 只節流成功發送，
  失敗每分鐘重試，曾因此拖垮 loop 撞上 task watchdog

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

- [ ] **修好 B 的 OTA** —— 阻塞項，沒解決就不能部署
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

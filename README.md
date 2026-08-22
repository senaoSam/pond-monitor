# 魚塭監控系統

水溫監測 + 心跳告警。感測器節點在魚塭採集資料寫入 Firebase RTDB，
看門狗節點在家裡監控心跳，失聯時透過 Discord 持續告警直到確認。

## 架構

```
[魚塭] sensor-node ──RS485──> QX-DT01P 水溫探頭
         │
         └──WiFi──> Firebase RTDB
                        ▲
[家裡]  watchdog ───────┘  讀取心跳、判斷失聯
         │
         └──> Discord (發送告警 + 輪詢 ✅ 確認)
```

看門狗刻意放在**不同地點**：若與感測器共用電源和網路，魚塭斷電時兩者一起失效，
告警永遠不會發出。

## 專案目錄

| 目錄 | 裝置 | 角色 |
|---|---|---|
| `sensor-node/` | ESP32-S3 (焊有 RS485 模組) | 讀水溫、上傳 RTDB |
| `watchdog/` | ESP32-S3 (裸板) | 監控所有節點、Discord 告警 |

## RTDB 結構

```
devices/pond-site/
    meta/    { name, scope:"site", model, fw, interval:60,
               sensors:["temp","humid"], bootAt }
    latest/  { ts, temp, humid }          # 每分鐘覆寫，watchdog 的心跳來源
devices/watchdog/
    meta/    { name, role:"watchdog", interval:60, staleMultiple:5, bootAt }
    latest/  { ts, checks, alerting }     # 自身心跳，供未來的 C 節點監控
history/pond-site/2026-08/
    <unix_ts>: { temp, humid }            # 每 5 分鐘一筆，按月分片
alerts/pond-site/
    { active, acked, ackedAt, firedAt, clearedAt, lastSeen, reason, by }
```

裝置 ID 綁定**監測目標**而非硬體（`pond-site` 而非 `esp32-a`），
換板子時歷史資料保持連續。

`meta.interval` 是 watchdog 的門檻來源 —— 改變感測器上傳頻率，
watchdog 自動跟隨，不需重燒。

## 新板子驗收

**不要相信板子上的絲印，也不要按型號推定設定。** 兩片板子都印
ESP32-S3-N16R8，eFuse 也確認確實有 8MB PSRAM，但 watchdog 那片的 PSRAM
在 Arduino 2.0.17 下就是初始化不了（`opi` / `qio` 兩種模式皆然），
而錯誤的宣告只會表現成「OTA 壞掉」，其他一切正常。

驗收步驟：

```bash
# 1. 硬體事實(eFuse,出廠燒死,最權威)
esptool  --chip esp32s3 --port COMx flash_id     # 看 Features 那行
espefuse --chip esp32s3 --port COMx summary      # PSRAM_CAP / PSRAM_VENDOR / FLASH_TYPE

# 2. Arduino 層是否真的用得到(燒 board-check,看 LED)
cd board-check && pio run -t upload --upload-port COMx
#   綠 = PSRAM 可用    黃 = 讀不到 PSRAM,勿宣告    紅 = 數值異常
```

兩者都要看：eFuse 說「硬體有」，board-check 說「軟體用得到嗎」——
**只有後者為真才可以宣告 `BOARD_HAS_PSRAM`。**

這個專案的韌體約用 60KB，內建 320KB heap 已足夠，不需要 PSRAM。

## 硬體參數

**QX-DT01P RS485 水溫探頭**（無手冊，參數由暴力掃描取得）

| 項目 | 值 |
|---|---|
| 通訊 | 9600 8N1, Modbus RTU, 站號 1 |
| 溫度 | 功能碼 0x03, 寄存器 0x0000, 值 ×10 |
| 濕度 | 寄存器 0x0001, 值 ×10（魚塭用不到，`PUBLISH_HUMIDITY` 可關閉） |
| 供電 | 12V |

**接線**（隔離式 TTL↔RS485 模組，正午物聯）

```
模組 VCC1 → ESP32 3V3        模組 A+ → 探頭白線
模組 GND1 → ESP32 GND        模組 B- → 探頭黃線
模組 Tx   → ESP32 GPIO17     模組 VCC2 → 12V+
模組 Rx   → ESP32 GPIO18     模組 GND2 → 12V-
```

模組為光耦隔離，**GND1 與 GND2 不可互接**。

注意腳位角色與模組絲印相反：GPIO17 是 ESP32 的 RX、GPIO18 是 TX。

## 燒錄

平常用 OTA，不需碰板子：

```
pio run -e ota -t upload          # sensor-node / watchdog 各自目錄下
```

首次燒錄或 OTA 失敗時走 USB。**這兩片板子的 CH343 沒有自動 reset 電路**，
必須手動進入下載模式：按住 BOOT → 按一下 RST → 放開 BOOT。
（進入下載模式後 COM 埠號會改變 —— 原生 USB 的 PID 從 4001 變 1001，要重查。）

```
pio run -e esp32-s3-devkitc-1 -t upload --upload-port COMx
```

### OTA 疑難排解

**`Could Not Activate The Firmware`**（傳輸 100% 完成但拒絕啟用）
→ **檢查狀態頁的 `psram:` 是否為 0。** 若是 0 卻宣告了 `-DBOARD_HAS_PSRAM`，
就是這個原因：PSRAM 初始化失敗的情況下宣告它會破壞 OTA，
而 **USB 燒錄和程式運行都完全正常**，所以極難歸因。移除
`board_build.psram_type` 和 `-DBOARD_HAS_PSRAM` 即可。

實測記錄（watchdog 那片）：

| 設定 | `getPsramSize()` | OTA |
|---|---|---|
| `psram_type=opi` + `BOARD_HAS_PSRAM` | 0 | ❌ |
| `psram_type=qio` + `BOARD_HAS_PSRAM` | 0 | ❌ |
| 兩者都不宣告 | — | ✅ 雙向皆可 |

已排除、不必再試：分區表設定、`maximum_size` 覆寫、完整 `erase_flash`
重燒、映像格式、寫入方向、檢查迴圈阻塞。

**`Error Uploading`**（傳輸中途失敗）
→ 裝置忙於阻塞式 TLS 請求。兩份韌體都已加入 `otaInProgress` 暫停機制，
若仍發生，先呼叫 `/check` 或 `/now` 再立即上傳，搶在週期空檔。

失敗時舊韌體完好無損（雙分區 app0/app1），不會變磚。

## LED 狀態

板子在現場無網路可查時，LED 是唯一診斷手段。
（板載 RGB 是 WS2812，需 `neopixelWrite`，`digitalWrite` 無效。）

| 顏色 | sensor-node | watchdog |
|---|---|---|
| 🟢 綠脈衝（每 2 秒） | 正常 | 所有節點正常 |
| 🔴 紅脈衝 | 上次週期失敗 | **有節點失聯中** |
| 🔴 紅閃三下 | 感測器讀取失敗 | — |
| 🟡 黃閃兩下 | 上傳失敗（感測器正常） | RTDB 連不上 |
| 🔵 藍閃 | 連線中 / OTA | 連線中 / OTA |
| 🟣 紫恆亮 | 兩組 WiFi 都連不上，即將重開 | 同 |

## 診斷端點

```
http://<ip>/                      狀態與日誌
http://<ip>/now                   sensor-node: 立即讀取並上傳
http://<ip>/check                 watchdog: 立即執行一次檢查
http://<ip>/testalert             watchdog: 發送測試告警
http://<ip>/testack?msg=<id>      watchdog: 查詢該訊息是否已確認
```

## 告警行為

1. 節點超過 `interval × 5`（預設 5 分鐘）未上傳 → 寫入 `alerts/<id>`
2. Discord 發送訊息並 @所有者，bot 預先加上 ✅
3. 每 5 分鐘重發，直到所有者點 ✅
4. 確認後靜音 1 小時，**然後恢復通知** —— 已確認但未修復的魚塭不該永久靜音
5. 節點恢復上傳 → 自動清除告警，保留 `firedAt` 以供追溯

## 已知限制

- **搬到魚塭後無法遠端更新或讀取狀態頁**（不同網路）。診斷靠 LED 顏色與
  RTDB 資料是否更新。韌體含雙 WiFi 憑證，搬遷不需重燒。
- **供電品質會影響 WiFi**。曾遇到細線造成電壓降，ESP32 在 WiFi 啟動時電流
  拉高即連線失敗（LED 持續藍閃）。部署請用品質良好的線與充電器。
- **sensor-node 的原生 USB 孔（左）無法傳輸資料**，推測焊接時影響到
  GPIO19/20（S3 原生 USB 固定使用該對腳位）。燒錄請用 CH343 孔（右）。
  **後續焊接請避開 GPIO19/20。**
- 通知狀態僅存於 RAM，重開機後會重新發送一次告警（偏安全方向）。

## 待辦

- [ ] C 節點：燒同一份 watchdog 韌體、改 `DEVICE_ID` 即可，B/C 自動互相監控
- [ ] 前端圖表：讀 `history/pond-site/<YYYY-MM>`，`orderByKey` 取時間範圍
- [ ] 溶氧、pH 等各池感測器：可掛在同一條 RS485 匯流排（改站號即可），
      或依地理位置分組多片 ESP32
- [ ] RTDB 規則目前為完全開放（`.read/.write: true`）。Spark 方案下最壞情況
      是服務中斷而非計費，**但若升級 Blaze 必須先收緊**

# 交接筆記（2026-08-31）

給接手的 session。專案背景看 [README.md](README.md)，這裡只寫**當下狀態**和
**踩過的坑**。

目前沒有正在追的問題：兩片都運作正常，也都能遠端更新。

## 系統現況

| | A | B |
|---|---|---|
| DEVICE_ID | `pond-site` | `watchdog` |
| 板子序號 | CH343 `5CBC033443` | CH343 `5CBC033428` |
| IP | 192.168.0.37 | 192.168.0.38 |
| 韌體版本 | **6** | **14** |
| 拉取式 OTA | ✅ 已驗證（v5→v6） | ✅ 已驗證（v12→v13→v14，全程無 USB） |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |

兩片都在使用者家中運作，資料持續進 RTDB。**功能正常，且兩片都能遠端更新** ——
B 的 OTA 問題已於 2026-08-31 結案，見下。

最終部署：A 去魚塭、B 去南部家裡，開發者在北部 —— 所以兩片都必須能遠端更新，
這個前提現在成立。

## 已結案：B 的 OTA `ESP_ERR_OTA_VALIDATE_FAILED`（2026-08-31）

### 根因：rollback 標記，不是 PSRAM

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（Arduino 的 sdkconfig 預設開啟，已確認）
之下，**一個曾經驗證失敗的 slot 會被 `esp_ota_set_boot_partition()` 永久拒絕**，
只有 chip erase 或 `esp_ota_erase_last_boot_app_partition()` 能清掉那個標記。

這解釋了先前唯一解釋不了的現象：完整 `erase_flash` 之後 OTA 能成功「一次」，
之後又失敗 —— erase 清掉標記，下一次失敗又把它種回去。

修法是 [`shared/PullOta/pull_ota.h`](shared/PullOta/pull_ota.h) 在每次寫入前呼叫
`esp_ota_erase_last_boot_app_partition()`，讓板子自己清，不必靠 USB。

### 驗證過程

```
22:49  10 -> 11  install-failed  VALIDATE_FAILED   ← 無 PSRAM 宣告也失敗
22:50  10 -> 11  install-failed  VALIDATE_FAILED
23:06  11 -> 12  rolled-back     ← USB 燒 v12（NVS 還留著 v11 的 pending，預期內）
23:11  12 -> 13  installed       ← app0→app1，但前面剛做過 USB，還不算數
23:16  13 -> 14  installed       ← app1→app0，全程無 USB。這筆才是證明
```

**關鍵是 v13→v14。** v13 那次成功時，app1 的標記是被 USB 燒錄清掉的，所以還符合
舊的「erase 後成功一次」模式；v14 中間沒有任何 USB 介入，`erase_last_boot` 那行
才第一次真正發揮作用。兩個寫入方向都通過。

A 同時從 v5 推到 v6，確認新版 shared/PullOta 在它身上也正常。

### PSRAM 假說是錯的（前一版交接筆記寫錯了）

前一版這裡寫著「唯一剩下的變數是 PSRAM 宣告」，並要求接手者把 B 的
`platformio.ini` 改成與 A 一致後燒錄。**那是錯的，不要照做。**

RTDB `/fwlog/watchdog` 的兩筆 v10→v11 失敗紀錄證明：v10 是在**完全沒有 PSRAM
宣告**的情況下失敗的。也就是說這個變數的兩種狀態都失敗過，它從來就不是 A 與 B
的關鍵差異。`watchdog/platformio.ini` 已改回不宣告 PSRAM。

（這會是同一系列誤判的第五次 —— 前四次見下。）

### 已排除、不必重試

- 分區表設定、`board_upload.maximum_size` 覆寫
- 韌體映像格式（magic `E9` 正確）
- heap 碎片化（`largest free block` 212KB，串流寫入不需連續空間）
- anti-rollback（`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 未啟用；真正相關的是
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，那個是開的）
- PSRAM 宣告（見上）

### 這題上犯過的錯（避免重複）

先後斷言過五次錯誤原因：

1. 「B 沒有 PSRAM」→ eFuse 證明有 8MB
2. 「`psram_type` 模式設錯（該用 qio）」→ `qio` 也失敗
3. 「heap 碎片化」→ 212KB 連續空間充足
4. 「分區被標記 INVALID」→ 當時以錯誤碼不符否定了它
5. 「PSRAM 宣告是唯一剩下的變數」→ fwlog 證明 v10 無宣告也失敗

第 4 點特別值得記：方向其實是對的（就是 rollback 標記），卻因為
`esp_ota_erase_last_boot_app_partition()` 在**已經沒有標記**時回
`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`，被誤判為「假說不成立」而放棄。
**一個修法沒生效，不代表病因診斷錯了。**

教訓：
- `ESP.getPsramSize()` 只說「初始化成功了嗎」，不說「硬體有沒有」。拿 eFuse
  當硬體事實依據。
- **查雲端紀錄（`/fwlog`）再假設。** 這次的突破不是新實驗，是去讀已經存在的
  失敗紀錄，發現它推翻了交接筆記的前提。

### 往後改韌體必須守住的不變條件

v13 起，開機後必須走到
[`esp_ota_mark_app_valid_cancel_rollback()`](shared/PullOta/pull_ota.h) 才算確認。
**任何在那行之前就當掉的版本（例如卡在 WiFi 連線）會被自動回滾。**

這不會變磚 —— 板子自己退回舊版繼續跑，對「A 在魚塭、B 在南部、人在北部」的
部署反而是保險 —— 但那一版就是上不去。改動開機路徑時要留意。

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

## 待辦

- [x] ~~B 的 OTA~~ 已結案，見上（根因是 rollback 標記）
- [x] ~~`/fwlog` 尚無紀錄~~ 兩片都有紀錄了：
      `curl https://<RTDB>/fwlog/watchdog.json`（`pond-site` 同理）
- [ ] 前端圖表（使用者主場）：讀 `history/pond-site/<YYYY-MM>`，`orderByKey` 取範圍
- [ ] C 節點：燒 watchdog 韌體、改 `DEVICE_ID`，B/C 自動互相監控
- [ ] 溶氧、pH：可掛同一條 RS485（改站號），或依地理位置分組
- [ ] RTDB 規則全開放（Spark 方案最壞是服務中斷非計費）。**升 Blaze 前必須收緊**

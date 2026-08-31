# 交接筆記（2026-08-31）

給接手的 session。專案背景看 [README.md](README.md)，這裡只寫**當下狀態**和
**正在追的問題**。

## 系統現況

| | A | B |
|---|---|---|
| DEVICE_ID | `pond-site` | `watchdog` |
| 板子序號 | CH343 `5CBC033443` | CH343 `5CBC033428` |
| IP | 192.168.0.37 | 192.168.0.38 |
| 韌體版本 | **5** | **10** |
| 拉取式 OTA | ✅ 已驗證（v4→v5 自我更新成功） | ❌ 失敗中，見下 |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |

兩片都在你/使用者家中運作，資料持續進 RTDB。**功能都正常**，唯一問題是 B 無法
遠端更新。

最終部署：A 去魚塭、B 去南部家裡，開發者在北部 —— 所以**兩片都必須能遠端更新**。

## 正在追的問題：B 的 OTA `ESP_ERR_OTA_VALIDATE_FAILED`

### 症狀

B 拉取式 OTA 失敗，狀態頁顯示：

```
install failed: Could Not Activate The Firmware
  | set_boot_partition=5379 (ESP_ERR_OTA_VALIDATE_FAILED) running=app0
```

下載到 100%、寫入完成，但 `esp_ota_set_boot_partition()` 拒絕啟用，
理由是**目標分區的映像檔驗證失敗**（寫進去的資料讀回來不是有效映像）。

espota（推送式）在 B 上也是同一個錯誤 —— **兩種傳輸路徑、同一個失敗點**，
所以問題在板子端的 flash 寫入，不是網路或傳輸方式。

### 下一步（未完成的實驗）

**唯一剩下的變數是 PSRAM 宣告：**

| | `platformio.ini` | 拉取式 OTA |
|---|---|---|
| A | `psram_type = opi` + `-DBOARD_HAS_PSRAM` | ✅ 成功 |
| B | 兩者都無 | ❌ VALIDATE_FAILED |

兩片 eFuse 完全相同（`PSRAM_CAP=8M`、`AP_3v3`、`FLASH_TYPE=4 data lines`）。

**`watchdog/platformio.ini` 已經改成與 A 一致並編譯過，但還沒燒進 B。**
接手時要做的就是：接 B 的 USB（資料線 + CH343 孔）→ BOOT+RST →
`pio run -e esp32-s3-devkitc-1 -t upload --upload-port COMx` → 按 RST →
發布新版 → `curl http://192.168.0.38/fwcheck` 看是否成功。

如果成功：根因就是 PSRAM 宣告與 flash 寫入路徑有關，把設定統一即可。
如果還是失敗：改查 app1 那塊 flash 是否實體損壞（v12 已加寫入後讀回
分區前 4 bytes 的診斷，尚未燒入）。

### 已排除、不必重試

這些我都試過且證明無效，別再繞：

- 分區表設定、`board_upload.maximum_size` 覆寫
- 完整 `erase_flash` 重燒（能讓**一次** OTA 成功，之後又失敗）
- 韌體映像格式（magic `E9` 正確，與正常那片一致）
- 寫入方向（`app0→app1` 與 `app1→app0` 都失敗過也都成功過）
- heap 碎片化（`largest free block` 212KB，映像 962KB 但串流寫入不需連續空間）
- `esp_ota_erase_last_boot_app_partition()`（我曾假設是 INVALID/ABORTED 標記，
  **錯的** —— 那會回 `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`，不是 VALIDATE_FAILED）
- anti-rollback（`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 未啟用）

### 我在這題上犯過的錯（避免重複）

我先後斷言過四次錯誤原因，都被後續證據推翻：

1. 「B 沒有 PSRAM」→ eFuse 證明有 8MB
2. 「`psram_type` 模式設錯（該用 qio）」→ `qio` 也失敗
3. 「heap 碎片化」→ 212KB 連續空間充足
4. 「分區被標記 INVALID」→ 錯誤碼不符

教訓：**`ESP.getPsramSize()` 只說「初始化成功了嗎」，不說「硬體有沒有」。**
拿 eFuse 當硬體事實的依據。

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

- [ ] 上述 B 的 OTA 實驗
- [ ] `/fwlog` 節點已實作但尚無紀錄（A 的 v5 之後才有那段程式碼，之後的更新會寫）
- [ ] 前端圖表（使用者主場）：讀 `history/pond-site/<YYYY-MM>`，`orderByKey` 取範圍
- [ ] C 節點：燒 watchdog 韌體、改 `DEVICE_ID`，B/C 自動互相監控
- [ ] 溶氧、pH：可掛同一條 RS485（改站號），或依地理位置分組
- [ ] RTDB 規則全開放（Spark 方案最壞是服務中斷非計費）。**升 Blaze 前必須收緊**

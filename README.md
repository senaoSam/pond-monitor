# 魚塭監控

## LED 狀態

### ESP32 的燈

| 顏色 | sensor-node | watchdog |
|---|---|---|
| 🟢 綠脈衝（每 2 秒） | 正常 | 所有節點正常 |
| 🔴 紅脈衝 | 上次週期失敗 | **有節點失聯中** |
| 🔴 紅閃三下 | 感測器讀取失敗 | — |
| 🟡 黃閃兩下 | 上傳失敗（感測器正常） | RTDB 連不上 |
| 🔵 藍閃 | 連線中 / OTA | 連線中 / OTA |
| 🟣 紫恆亮 | 兩組 WiFi 都連不上，即將重開 | 同 |

### 撥號模組的燈

**NET 慢閃 + DTU 恆亮 = 正常。** 通電後 30 秒內就看得出來。

| 燈 | 狀態 | 意思 / 檢查 |
|---|---|---|
| NET | 慢閃 | ✅ 可以撥電話 |
| NET | 快閃 | ❌ 還不能撥:4G / VoLTE 未註冊 → 等一兩分鐘;持續就查 SIM、天線、儲值 |
| NET | 恆亮 | 📞 撥號中 |
| NET | 不亮 | ❌ 模組沒電 → 查紅黑線 12V |
| DTU | 恆亮 | ✅ 跟 ESP32 連線正常 |
| DTU | 快閃 | ❌ ESP32 收不到模組回應 → 查 🟢 綠線 |
| DTU | 不亮 | ❌ 模組收不到 ESP32 → 查 🟡 黃線、⚪ 灰線 |

紅色 PWR 燈不代表模組在運作,看 NET。

## 撥號模組接線

| 排線 | 接到 |
|---|---|
| 🔴 紅 / ⚫ 黑 | 12V 變壓器 + / −(模組只能從這裡供電,Micro USB 不供電) |
| 🟡 黃 | ESP32 GPIO17 |
| 🟢 綠 | ESP32 GPIO18 |
| 🔵 藍 | ESP32 GPIO16 |
| ⚪ 灰 | ESP32 GND |

白、橘不接。

## 診斷端點

```
http://<ip>/                      狀態與日誌
http://<ip>/now                   sensor-node: 立即讀取並上傳
http://<ip>/check                 watchdog: 立即執行一次檢查
http://<ip>/testalert             watchdog: 發送測試告警
http://<ip>/testack?msg=<id>      watchdog: 查詢該訊息是否已確認
http://<ip>/testcall?pass=<OTA密碼>[&num=<號碼>] watchdog: 立刻撥測試電話(不帶 num = 名單每支各一通)
http://<ip>/fwcheck               立即檢查韌體更新（兩者皆有）
```

更多細節(架構、RTDB 結構、OTA、燒錄、告警行為):[docs/notes.md](docs/notes.md)

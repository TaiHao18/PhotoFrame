# PhotoFrame

## Danh sách linh kiện
- 1× **TFT ST7735S**
- 1× **ESP8266**
- 3× **Buttons** (TOGGLE, NEXT, PREV)
- 1× **Switch**

##TFT ↔ ESP8266
| TFT pin | ESP8266 pin |
|---|---|
| BLK | 3V |
| CS  | D0 |
| DC  | D2 |
| RST | D4 |
| SDA | D7 |
| SCL | D5 |
| VCC | 3V |
| GND | GND |

## Nút bấm
| Nút  | Chân ESP8266 | Thao tác | Chức năng |
|---|---|---|---|
| TOGGLE | D1 | Nhấn | Vào **Info AP** |
| TOGGLE | D1 | Giữ 1s | Vào **Menu** (trong menu hoạt động như **nút chọn**) |
| NEXT | RX | Nhấn | **Next** |
| NEXT | RX | Giữ 1s | Hiện **ảnh QR** đã tải lên |
| PREV | D3 | Nhấn | **Trở lại** |

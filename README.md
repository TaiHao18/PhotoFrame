# PhotoFrame

## List of components
- 1× **TFT ST7735S**
- 1× **ESP8266**
- 3× **Buttons** (TOGGLE, NEXT, PREV)
- 1× **Switch**

## TFT ↔ ESP8266
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

## Button
| Button  | ESP | operation | Function |
|---|---|---|---|
| TOGGLE | D1 | Click | Enter **Info AP** |
| TOGGLE | D1 | Hold 1s | Enter **Menu** |
| NEXT | RX | Click | **Next** |
| NEXT | RX | Hold 1s | draw **QR Code** |
| PREV | D3 | Click | **Back** |

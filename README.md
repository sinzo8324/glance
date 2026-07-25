# Glance

Glance는 GeekMagic SmallTV Ultra(ESP8266)와 SmallTV Pro(ESP32)에 암호화폐, 원/달러 환율, 국내 주식 시세와 Raspberry Pi 상태를 표시하는 커스텀 펌웨어입니다.

## 주요 기능

- 빗썸: BTC, ETH, KAIA, USDT, USDC 원화 시세
- Yahoo Finance: 코스피, 삼성전자, SK하이닉스, KODEX ETF, USD/KRW
- Open-Meteo: 현재 날씨
- 야간 시계, 밝기 및 화면 전환 주기 설정
- Raspberry Pi CPU, 메모리, 디스크, 온도 모니터링
- 웹 브라우저를 통한 설정 및 OTA 업데이트
- SmallTV Pro: 정전식 터치 메뉴, 비동기 데이터 갱신, 암호화폐 스파크라인

## 프로젝트 안내

이 저장소는 개인 학습을 위한 비공식·비상업적 프로젝트입니다. GeekMagic, CoinGecko 및 표시되는 암호화폐 프로젝트와 공식적인 제휴·후원·승인 관계가 없습니다.

GeekMagic을 비롯한 제품명, 회사명, 암호화폐 명칭과 로고 및 기타 상표는 각 권리자에게 귀속됩니다. 저장소에 포함된 명칭과 작은 아이콘은 호환 기기와 표시 대상을 식별하기 위해 사용됩니다.

화면용 글꼴 비트맵은 SIL Open Font License 1.1로 배포되는 Noto Sans와 Noto Sans CJK KR을 이용해 생성합니다. 생성 스크립트와 사용 글꼴 경로는 `tools/`에서 확인할 수 있습니다.

## 빌드

[PlatformIO](https://platformio.org/install)를 설치한 뒤 프로젝트 폴더에서 실행합니다.

```bash
# SmallTV Ultra
pio run -e esp12e

# SmallTV Pro
pio run -e esp32-pro
```

완성된 펌웨어는 각각 `.pio/build/esp12e/firmware.bin`과 `.pio/build/esp32-pro/firmware.bin`에 생성됩니다. 기종이 다른 펌웨어를 업로드하지 않도록 주의하세요.

## 설치

기존 SmallTV 웹 화면의 **Firmware Upgrade** 메뉴에서 생성된 `firmware.bin`을 업로드합니다. 최초 설치 전에 원본 펌웨어를 백업하는 것을 권장합니다.

USB-UART로 설치하려면 대상 기기의 TX, RX, GND를 연결하고 부팅할 때 GPIO0을 GND로 내려 업로드 모드로 진입한 후 기종에 맞는 명령을 실행합니다.

```bash
# SmallTV Ultra
pio run -e esp12e -t upload

# SmallTV Pro
pio run -e esp32-pro -t upload
```

SmallTV Pro의 확인된 화면 및 터치 핀 정보는 `platformio.ini`에 기록되어 있습니다.

## 처음 실행

1. 기기를 켜고 휴대폰에서 `SmallTV-Setup` Wi-Fi에 연결합니다.
2. 자동으로 열린 설정 화면에서 사용할 2.4GHz Wi-Fi를 선택합니다.
3. 연결 후 화면에 표시되는 IP 주소를 확인합니다.
4. 브라우저에서 `http://기기-IP/`를 열어 밝기, 야간 모드와 갱신 주기를 설정합니다.

## OTA 업데이트

`src/main.cpp`의 `OTA_PASS` 기본값을 반드시 원하는 비밀번호로 변경한 뒤 빌드하세요. 이후 `http://기기-IP/update`에서 사용자명 `admin`과 설정한 비밀번호로 로그인하여 새 `firmware.bin`을 업로드할 수 있습니다.

## 설정 변경

- 표시 종목: `src/main.cpp`의 `coins`, `stocks`, `stocks2`, `dollar` 및 `fetchAll()`
- 날씨 위치: `src/main.cpp`의 `LAT`, `LON`
- 시간대: `src/main.cpp`의 `TZ_OFFSET_SEC`
- 화면 방향과 색상: `src/main.cpp`의 `ROTATION`, `INVERT_COLORS`

## Raspberry Pi 모니터

Raspberry Pi에서 다음 명령으로 에이전트를 실행합니다.

```bash
python3 pi_agent.py
```

SmallTV 설정 화면에서 모드를 `Pi monitor`로 바꾸고 Raspberry Pi의 IP 주소와 포트 `8080`을 입력합니다.

## SmallTV Pro 터치 조작

- 짧게 터치: Market 모드에서 다음 페이지
- 길게 터치: 모드 선택 메뉴 열기 또는 닫기
- 메뉴에서 짧게 터치: 다음 항목
- 메뉴에서 두 번 터치: 선택 확정

## 데이터 출처

- [Bithumb](https://apidocs.bithumb.com/): 암호화폐 원화 시세
- Yahoo Finance 비공식 chart endpoint: 국내 주식, 코스피, 환율
- [Open-Meteo](https://open-meteo.com/): 날씨

별도의 API 키는 필요하지 않습니다. Yahoo Finance endpoint는 비공식이므로 향후 요청 제한이나 동작 변경이 생길 수 있습니다.

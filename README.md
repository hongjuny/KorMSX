# KorMSX

한국형 MSX1/MSX2 에뮬레이터. 이식성 있는 C 코어와 얇은 Swift/AppKit 프론트엔드로
구성됩니다(CGC Apple과 같은 구조). 기기 구성과 주변장치 동작은 MAME의
`src/mame/msx/msx.cpp`, `msx1.cpp`, `msx2.cpp`, `msx_keyboard.cpp`, `src/devices/video/tms9928a.cpp`,
`v9938.cpp`, `src/devices/sound/ay8910.cpp`를 참조했습니다.

## 기능

- **CPU**: Z80. 비문서화 명령, X/Y 플래그, MEMPTR, SCF/CCF의 Q 래치까지 구현했고,
  **ZEXDOC과 ZEXALL을 모두 통과**합니다. MSX처럼 M1 사이클마다 대기 1클럭을 넣습니다.
- **VDP (MSX1)**: TMS9918A. Graphic 1/2, Text 1, Multicolor와 비정상 모드 조합,
  스프라이트(라인당 4개, 5번째 스프라이트 플래그, 충돌, early clock)를 지원합니다.
- **VDP (MSX2/2+)**: V9938/V9958. MAME `v9938.cpp`를 C로 옮겼습니다. VRAM 128KB, 80열 텍스트,
  SCREEN 0–8(G6/G7의 VRAM 인터리브 포함), 스프라이트 모드 2, 팔레트, 라인 인터럽트,
  세로 스크롤, 명령 엔진(HMMV/HMMM/YMMM/HMMC/LMMV/LMMM/LMCM/LMMC/LINE/SRCH/PSET/POINT)을 지원합니다.
  V9958의 가로 스크롤과 YJK 모드(SCREEN 10–12)도 옮겼지만 아직 테스트하지 않았습니다.
- **MSX2 장치**: 메모리 매퍼(FC–FFh), RP5C01 시계(맥의 현재 시각, 설정 RAM은 메모리에만 보관),
  한자 폰트 ROM 포트(D8–DBh)를 지원합니다.
- **PSG**: AY-3-8910(톤 3채널, 노이즈, 엔벨로프) 위에 키 클릭을 섞습니다.
- **슬롯**: 기본 슬롯 4개와 확장 서브슬롯(FFFFh)을 지원하고, 기기 구성은 MAME의
  `add_internal_slot` 정의를 옮겨 왔습니다.
- **카트리지**: 일반 ROM(8/16/32/48/64 KB)과 Konami, Konami SCC(SCC 음원 포함),
  ASCII 8K, ASCII 16K 메퍼를 지원합니다. 메퍼는 자동으로 판별합니다.
- **키보드**: MAME `msxkr` 매트릭스(일본식 배열, ₩ 키, CODE 자리의 한글 키)를 씁니다.
  글자·숫자는 키 위치로, 기호는 문자 그대로 입력하며 ⌘V 붙여넣기도 됩니다.

## 기기

| 기기 | MAME 이름 | 구성 |
|---|---|---|
| C-BIOS MSX1 | — | 내장 무료 BIOS. 카트리지 전용, BASIC 없음 |
| MSX1 국제판 | — | `MSX.ROM`(CRC `94ee12f3`, 유럽판 BIOS+BASIC). RAM은 슬롯 3 |
| Daewoo IQ-1000 DPC-200 | `dpc200` | BIOS + 한글 ROM 16 KB |
| Goldstar FC-80U | `gsfc80u` | BIOS + 한글 ROM 8 KB |
| Samsung SPC-800 | `spc800` | BIOS + 한글 ROM 16 KB |
| Goldstar GFC-1080 | `gfc1080` | BIOS + 한글 ROM + Pasocalc |
| Daewoo Zemmix CPC-51 | `cpc51` | BIOS만 |
| C-BIOS MSX2 / MSX2+ | — | 내장 무료 BIOS. 카트리지 전용 |
| MSX2 국제판 | `nms8250` | `MSX2.ROM` + 서브 ROM `MSX2EXT.ROM`(Philips NMS 8250 구성) |
| Daewoo IQ-2000 CPC-300 | `cpc300` | MSX2. BIOS, 서브 ROM, 한글 ROM 32 KB |
| Daewoo X-II CPC-400 | `cpc400` | MSX2. BIOS, 서브 ROM, 한글 ROM, 한자 폰트 ROM |
| Daewoo X-II CPC-400S | `cpc400s` | MSX2. BIOS+디스크+서브 64 KB, 한글+한자 드라이버 64 KB, 한자 폰트 |

대우 MSX2는 슬롯 0을 서브슬롯 넷으로 나눠 BIOS(0-0), 한글 ROM(0-1), 매퍼 RAM 128 KB(0-2),
서브 ROM(0-3)을 둡니다. 실제 기계의 플로피 드라이브와 CPC-300의 S-1985 백업 RAM은 아직 없습니다.

한국형 기기는 모두 슬롯 0의 0000h–7FFFh에 BIOS, 8000h–BFFFh에 한글 ROM을 두고,
슬롯 1을 카트리지, 슬롯 2를 RAM 64 KB로 씁니다.

## 큐닉스 한글

큐닉스(Qnix)가 만든 한글 BASIC(1–3판)이 한국형 MSX1 기기의 한글 ROM에 들어 있습니다.
이 ROM을 정확히 에뮬레이트된 하드웨어에서 그대로 실행하므로, 한글 출력, `SET HAN`,
`CALL HANON` 같은 명령과 한글 입력은 원래 기계와 똑같이 동작합니다. 다만 ROM에 따라
판이 다릅니다. FC-80U와 초기 DPC-200에 든 1.0판에는 `CALL HANON`이 없습니다.

- **한글 키**: 오른쪽 Option(또는 한/영, かな 키). 제목 표시줄에 `[한글]` LED가 켜지면
  두벌식 자판 위치로 한글을 입력합니다. 맥 입력기는 영문으로 두세요.
- GRAPH는 왼쪽 Option, STOP은 F8, SELECT는 F9, INS는 F10/Help, HOME은 Home입니다.
  기계 메뉴의 "키 배치 안내"에도 정리되어 있습니다.

## ROM

BIOS와 한글 ROM은 저작권이 있어 저장소에 포함하지 않습니다. 직접 덤프한 ROM을 프로젝트의
`roms/`에 넣고 `scripts/build_app.sh`로 빌드하면 앱 패키지 안(`KorMSX.app/Contents/Resources/roms`)에
들어가, 앱 하나만으로 실행됩니다. 앱은 다음 순서로 ROM을 찾습니다.

1. `KORMSX_ROMS` 환경 변수가 가리키는 폴더
2. 파일 메뉴의 "ROM 폴더 선택…"으로 지정한 폴더
3. 앱 패키지 안의 `Contents/Resources/roms`
4. 앱 옆이나 현재 폴더의 `roms/`

앱은 사용자 라이브러리(`~/Library/Application Support` 등)에 파일을 만들지 않습니다.

파일 이름은 상관없습니다. ROM은 MAME 정의의 **CRC32**로 찾고, 맞는 것이 없으면
MAME 파일 이름(`200bios.rom`, `200han.rom` 등)으로 찾습니다. MAME 형식의 zip
(`dpc200.zip`)이나 기기 이름 하위 폴더도 읽습니다.

| 기기 | 파일 | CRC32 |
|---|---|---|
| 공통 한국 BIOS 32 KB | `200bios.rom` 등 | `3ab0cd3b` |
| DPC-200/100/180 한글 16 KB | `200han.rom` | `97478efb` |
| FC-80U 한글 8 KB | `fc80uhan.rom` | `0cdb8501` |
| SPC-800 한글 16 KB | `spc800han.rom` | `5ae2b013` |
| GFC-1080 BIOS / 한글 / Pasocalc | `gfc1080*.rom` | `d9cdd4a6` / `f209448c` / `4014f7ea` |
| CPC-51 BIOS | `cpc-51_v_1_01.ic05` | `c3a868ef` |
| MSX2 BIOS / 서브 ROM (NMS 8250) | `MSX2.ROM` / `MSX2EXT.ROM` | `6cdaf3a5` / `66237ecf` |
| CPC-300 BIOS / 서브 / 한글 | `300bios.rom` / `300ext.rom` / `300han.rom` | `53850907` / `d64da39c` / `e78cd87f` |
| CPC-400 BIOS / 서브 / 한글 / 한자 | `400bios.rom` / `400ext.rom` / `400han.rom` / `400kfn.rom` | `53850907` / `2ba104a3` / `a8ead5e3` / `b663c605` |
| CPC-400S 메인 / 한글 / 한자 | `400sbios.u38` / `400shan.u44` / `400skfn.rom` | `dbb0f26d` / `5fdb493c` / `fa85368c` |

## 빌드

macOS 13 이상과 Xcode 또는 Swift 툴체인이 필요합니다.

```sh
swift build -c release            # .build/release/KorMSX
scripts/build_app.sh              # build/KorMSX.app (C-BIOS + roms/의 ROM 포함, 개인용)
scripts/build_app.sh --no-roms    # ROM 없이 배포용으로 빌드
```

카트리지는 파일 메뉴에서 열거나 창에 끌어다 놓습니다(`.rom`, `.mx1`, `.zip`).
카트리지를 넣으면 기계가 다시 켜집니다.

## 테스트

```sh
scripts/run_tests.sh            # 슬롯/메퍼/SCC, PSG 톤, V9938 명령·80열, (ROM이 있으면) MSX1·MSX2 BASIC
scripts/run_tests.sh --zexall   # Z80 ZEXALL까지 (약 45초)
```

테스트 실행 파일과 캡처한 화면(`.ppm`)은 `tests/out/`에 생기고 git에서 제외됩니다.
`build/`에는 앱(`KorMSX.app`)만 둡니다.

`tests/data/zexdoc.com`, `zexall.com`은 Frank Cringle의 Z80 instruction exerciser(GPL)이고,
`psgtone.rom`(PSG 톤), `vdpcmd.rom`(SCREEN 5에서 HMMV·LINE·LMMV·HMMM),
`text80.rom`(80열 텍스트)은 이 저장소의 테스트 카트리지입니다.

## 아직 없는 것

- 플로피 디스크(대우 X-II의 MB8877A, NMS 8250의 WD2793)와 `.dsk` 이미지
- 카세트(`.cas`) 로딩, 조이스틱 포트(키보드 커서/스페이스로 하는 게임은 됩니다)
- MSX-MUSIC(YM2413), 터보 R, 프린터

## 라이선스

내장 C-BIOS는 C-BIOS 팀의 BSD 형식 라이선스를 따릅니다
(`Sources/KorMSX/Resources/cbios/LICENSE-cbios.txt`).

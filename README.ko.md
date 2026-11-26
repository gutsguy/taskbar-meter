<div align="center">

# TaskbarMeter

**Windows 작업표시줄에 CPU · RAM · GPU · 디스크 · 네트워크를 보여주는 아주 가벼운 프로그램.**<br>
원래 Windows에 있던 것처럼 보이고, 메모리는 1.3MB 정도만 씁니다.

[![Build](https://github.com/gutsguy/taskbar-meter/actions/workflows/build.yml/badge.svg)](https://github.com/gutsguy/taskbar-meter/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/gutsguy/taskbar-meter)](https://github.com/gutsguy/taskbar-meter/releases/latest)
![Windows 11](https://img.shields.io/badge/Windows-11-0078D4)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

<img src="docs/screenshot-dark.png" alt="다크 테마 작업표시줄의 TaskbarMeter" width="500"><br>
<img src="docs/screenshot-light.png" alt="라이트 테마 작업표시줄의 TaskbarMeter" width="500">

[English](README.md) · **한국어**

</div>

---

## 왜 만들었나

작업표시줄 모니터 대부분은 자기만의 상자·배경·글꼴을 쓰고, 숫자 두 개를 보여주는 데 메모리를 수십 MB씩 씁니다.
TaskbarMeter는 작업표시줄과 같은 글꼴로 글자만 그리고, 라이트/다크 테마를 따라가며, 눈에 거슬리지 않습니다.

- **순정 같은 모양.** Segoe UI, 투명 배경, 시계와 같은 스타일. 라이트/다크 테마, 모든 화면 배율 지원.
- **작음.** 약 70KB 실행 파일 하나. 설치·런타임·설정 파일 없음.
- **켠 만큼만 씀.** 꺼 둔 항목은 측정 자체를 하지 않습니다.

## 리소스 사용량

같은 Windows 11 PC에서 CPU + RAM + 네트워크를 표시한 상태로 30초 평균을 쟀습니다.

| | TaskbarMeter | TrafficMonitor Lite 1.86 |
|---|---:|---:|
| 실제 메모리 (Private) | **1.3MB** | 13.5MB |
| 작업 관리자 표시 메모리 | **8.5MB** | 47MB |
| CPU | **0.02%** | 0.06% |
| 배포 크기 | **파일 1개, 약 70KB** | 파일 약 60개, 압축 1MB |

모든 항목을 켜면 약 7MB를 씁니다 (GPU 카운터가 가장 무겁습니다). 왼쪽 끝에 표시하면 약 2MB가 더 듭니다.

> [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)는 스킨·그래프·온도·플러그인까지 훨씬 많은 걸 합니다.
> 그런 기능이 필요하면 TrafficMonitor를 쓰세요. TaskbarMeter는 숫자만 보고 싶은 사람을 위한 프로그램입니다.

## 기능

- CPU, RAM, GPU, 디스크, 네트워크(↑ 업로드 / ↓ 다운로드). 각각 켜고 끌 수 있음
- 90% 이상이면 숫자가 주황색으로 바뀜
- 시계 옆 또는 작업표시줄 왼쪽 끝에 표시 (위젯 버튼이 있으면 그 오른쪽에 붙고, 앱 아이콘과도 겹치지 않음)
- 더블클릭하면 작업 관리자 열림
- 탐색기가 재시작돼도 다시 붙음
- 메뉴는 한국어/영어 (Windows 표시 언어를 따름)

## 설치

1. [Releases](https://github.com/gutsguy/taskbar-meter/releases/latest)에서 `TaskbarMeter.exe`를 받습니다.
2. 실행합니다.
3. 부팅할 때마다 켜고 싶으면 숫자를 우클릭 → **Windows 시작 시 실행**.

처음 실행할 때 서명되지 않은 앱이라 SmartScreen 경고가 뜰 수 있습니다: **추가 정보 → 실행**.

## 사용법

| 동작 | 결과 |
|---|---|
| 더블클릭 | 작업 관리자 열기 |
| 우클릭 | 항목 켜기·끄기, 왼쪽에 표시, 시작 시 실행, 종료 |

설정은 `HKCU\Software\TaskbarMeter`에 저장됩니다.

## 직접 빌드

[Zig](https://ziglang.org/download/) 0.17 이상이 필요합니다 (C 컴파일러로 사용, Visual Studio 불필요).

```bat
git clone https://github.com/gutsguy/taskbar-meter
cd taskbar-meter
build.cmd
```

`zig`가 `PATH`에 없으면 환경 변수 `ZIG`에 `zig.exe` 전체 경로를 지정하세요.

## 동작 원리

프로그램 전체가 Win32 API만 쓰는 C 파일 하나, [`src/meter.c`](src/meter.c)입니다.

- 작업표시줄(`Shell_TrayWnd`)에 레이어드 자식 창을 붙이고, 1초마다 픽셀 단위 투명도로 다시 그립니다.
- CPU는 `GetSystemTimes`, RAM은 `GlobalMemoryStatusEx`, 네트워크는 `GetIfTable2` (실제 장치만).
- GPU와 디스크는 작업 관리자와 같은 성능 카운터(`GPU Engine`, `PhysicalDisk`)를 씁니다.
- 왼쪽 끝 모드에서는 UI Automation으로 위젯 버튼이 끝나는 위치를 찾고, 작업표시줄 설정이 바뀌면 바로 다시 확인합니다.

## 제한 사항

- Windows 11용으로 만들고 테스트했습니다. Windows 10 (1607 이후)에서도 실행은 되지만 테스트하지 않았고, 작업표시줄에 앱이 가득 차면 앱 버튼이 숫자 아래로 들어갈 수 있으며 세로 작업표시줄은 지원하지 않습니다.
- 주 모니터의 작업표시줄만 지원합니다.
- 온도는 지원하지 않습니다. 관리자 권한과 커널 드라이버가 필요해서 이 프로그램의 취지와 맞지 않습니다.

## 삭제

우클릭 → **Windows 시작 시 실행** 끄기 → **종료** 후 `.exe`를 지우면 됩니다.
원하면 레지스트리 `HKCU\Software\TaskbarMeter` 키도 지우세요.

## 라이선스

[MIT](LICENSE)

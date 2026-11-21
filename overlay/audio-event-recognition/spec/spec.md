**\<PROJECT 중간보고서\>**

**Linux/Android Porting on**

**ARM Cortex-A53 SoC**

**2012-13458 박현종**

**2023-13373 노윤철**

**엄현상 교수님**

**최으뜸 이사님 (CRZ Technology)**

**Table of Contents**

**[1\. Abstract	3](#1.-abstract)**

[**2\. Introduction	3**](#2.-introduction)

[**3\. Background Study	4**](#3.-background-study)

[A. 관련 접근방법/기술 장단점 분석	4](#a.-관련-접근방법/기술-장단점-분석)

[B. 프로젝트 개발환경	4](#b.-프로젝트-개발환경)

[**4\. Goal/Problem & Requirements	5**](#4.-goal/problem-&-requirements)

[**5\. Approach	5**](#5.-approach)

[**6\. Project Architecture	7**](#6.-project-architecture)

[A. Architecture Diagram	7](#a.-architecture-diagram)

[B. Architecture Description	7](#b.-architecture-description)

[**7\. Implementation Spec	8**](#7.-implementation-spec)

[A. Input/Output Interface	8](#a.-input/output-interface)

[B. Inter Module Communication Interface	9](#b.-inter-module-communication-interface)

[C. Modules	10](#c.-modules)

[**8\. Current Status	13**](#8.-current-status)

[**9\. Future Work	14**](#9.-future-work)

[**10\. Division & Assignment of Work	15**](#10.-division-&-assignment-of-work)

[**11\. Schedule	15**](#11.-schedule)

[**◆ \[Appendix\] Detailed Implementation Spec	17**](#◆-[appendix]-detailed-implementation-spec)

[A. 설정 파일 인식	17](#a.-설정-파일-인식)

[a. config\_parse()	17](#a.-config_parse\(\))

[B. 모듈 간 데이터 전달	17](#b.-모듈-간-데이터-전달)

[a. listen\_on\_socket()	17](#a.-listen_on_socket\(\))

[b. connect\_to\_socket()	17](#b.-connect_to_socket\(\))

[c. send\_to\_socket()	17](#c.-send_to_socket\(\))

[d. receive\_on\_socket()	17](#d.-receive_on_socket\(\))

[C. 오디오 입력 모듈	18](#c.-오디오-입력-모듈)

[a. find\_device\_name()	18](#a.-find_device_name\(\))

[b. open\_mic()	18](#b.-open_mic\(\))

[c. read\_mic()	18](#c.-read_mic\(\))

[D. 전처리 모듈	18](#d.-전처리-모듈)

[a. convert\_to\_mono\_f32()	18](#a.-convert_to_mono_f32\(\))

[b. resample()	18](#b.-resample\(\))

[E. 프레임 분할 모듈	19](#e.-프레임-분할-모듈)

[a. add\_samples\_to\_buffer()	19](#a.-add_samples_to_buffer\(\))

[b. get\_frame()	19](#b.-get_frame\(\))

[F. 추론 모듈	19](#f.-추론-모듈)

[a. load\_model()	19](#a.-load_model\(\))

[b. run\_model()	19](#b.-run_model\(\))

[G. 이벤트 판정 모듈	19](#g.-이벤트-판정-모듈)

[a. check\_event()	19](#a.-check_event\(\))

[b. find\_top\_n()	20](#b.-find_top_n\(\))

[H. 화면 출력 모듈	20](#h.-화면-출력-모듈)

[a. create\_window()	20](#a.-create_window\(\))

[b. update\_display\_state()	20](#b.-update_display_state\(\))

[c. render\_default()	20](#c.-render_default\(\))

[d. render\_alert()	20](#d.-render_alert\(\))

[I. 사용자 알림 모듈	21](#i.-사용자-알림-모듈)

[a. connect\_to\_broker()	21](#a.-connect_to_broker\(\))

[b. build\_json()	21](#b.-build_json\(\))

[c. send\_to\_broker()	21](#c.-send_to_broker\(\))

[J. MQTT 브로커 모듈	21](#j.-mqtt-브로커-모듈)

[a. handle\_packet()	21](#a.-handle_packet\(\))

[b. broadcast\_to\_subscribers()	22](#b.-broadcast_to_subscribers\(\))

# **1\. Abstract** {#1.-abstract}

본 프로젝트는 CRZ Technology가 제작한 TI(Texas Instruments) AM62x 기반 커스텀 보드에 2026년에 새로 배포된 TI Processor SDK를 이식하여, 해당 보드에서 Linux, Linux-RT, Android가 모두 동작하도록 하는 것을 목표로 한다. 이 보드는 현재 2023년에 배포된 SDK를 바탕으로 동작하고 있다.

TI의 SDK는 자사 reference board를 전제로 작성되어 있기 때문에, CRZ 보드에서 사용하려면 두 보드의 하드웨어 차이를 부팅 과정 전반에 반영해야 한다. 우리는 R5 SPL부터 A53 SPL, U-Boot, 그리고 Linux 커널 및 Android에 이르기까지 각 단계의 Device Tree, configuration, driver를 CRZ 보드의 구성에 맞게 수정한다. 이를 통해 SD 카드와 eMMC 두 가지 방식으로 부팅할 수 있도록 하고 보드의 Peripheral이 정상적으로 동작하도록 한다. 실제 사용자가 보드를 손쉽게 활용할 수 있도록, 미리 빌드된 이미지와 SD 카드 제작을 돕는 스크립트, 그리고 전체 과정을 설명하는 매뉴얼을 함께 제작할 예정이다.

또한 포팅이 올바르게 이루어졌음을 보여주기 위해, 이상 상황을 알리는 소리를 감지하는 오디오 이벤트 인식 기능을 구현한다. 이 기능은 USB 포트에 연결된 마이크로 주변 소리를 받아들이고, TensorFlow Lite로 경량 오디오 분류 모델인 YAMNet을 실행하여 소리의 종류를 판별한다. 이벤트가 감지되면 Ethernet 포트를 통해 MQTT로 사용자 기기에 알림을 보내고 LCD 화면에도 결과를 표시하므로, 이 기능 하나로 USB 입력, SoC에서의 연산, 네트워크, 화면 출력이 모두 정상적으로 동작함을 보일 수 있다.

# **2\. Introduction** {#2.-introduction}

CRZ Technology는 TI의 AM62x 프로세서를 탑재한 보드를 판매하고 있다. 현재 이 보드에는 2023년에 배포된 TI SDK가 적용되어 있는데, 2026년 새로운 버전의 SDK가 배포되면서 보드의 소프트웨어를 최신 SDK에 맞추어 갱신해야 한다. 그러나 TI SDK는 TI reference board를 기준으로 만들어져 있어, 하드웨어 구성이 다른 CRZ 보드에서는 그대로 사용할 수 없다. 따라서 최신 SDK를 CRZ 보드에서 활용하려면 두 보드 사이의 차이를 파악하고, 이를 부트로더와 커널 전반에 반영하는 포팅 작업이 요구된다.

본 프로젝트는 이 포팅 작업을 수행하는 것을 목표로 한다. 구체적으로는 최신 SDK 버전의 Linux 및 Android가 CRZ 보드에서 안정적으로 부팅되도록 하고, Touchscreen LCD와 USB를 비롯한 주요 Peripheral이 정상적으로 동작하도록 만든다. 아울러 다른 사람이 같은 결과를 다시 얻을 수 있도록 하는 스크립트 파일과 매뉴얼을 제공한다.

이와 함께 회사 측은 포팅이 올바르게 되었는지를 확인할 수 있는 애플리케이션을 선정하여 구현할 것을 요청하였다. 우리는 이 보드가 산업 현장에서 쓰인다는 점에 주목하여, 사이렌 소리나 물체가 부서지는 소리처럼 이상 상황을 알리는 소리를 인식하고 사용자 기기로 알림을 보내는 오디오 이벤트 인식 기능을 구현하기로 하였다. 이 기능은 마이크 입력, 네트워크 통신, 화면 출력을 모두 사용하기 때문에, 포팅 결과를 종합적으로 보여주는 데 적합하다.

# **3\. Background Study** {#3.-background-study}

## **A. 관련 접근방법/기술 장단점 분석** {#a.-관련-접근방법/기술-장단점-분석}

오디오 이벤트 인식은 주변에서 들리는 소리를 분석하여 그 소리가 어떤 사건에 해당하는지를 판별하는 기술이다. 가장 단순한 방법은 소리의 크기나 특정 주파수 성분이 일정 기준을 넘는지를 확인하는 규칙 기반 방식으로, 구현이 간단하고 연산량이 적다. 그러나 이 방식은 사이렌 소리와 단순히 큰 소음을 구별하기 어렵고, 감지하려는 소리마다 규칙을 직접 설계해야 한다는 한계가 있다. 이에 따라 최근에는 소리의 특징을 학습한 신경망 모델로 소리의 종류를 분류하는 방식이 널리 사용되고 있으며, 본 프로젝트에서도 이 방식을 채택하였다.

신경망 모델로는 YAMNet을 선택하였다. 모델을 직접 학습시키면 산업 현장의 소리에 특화된 결과를 얻을 수 있지만, 학습 데이터를 수집하고 라벨링하는 데 많은 시간이 필요하다. 반면 YAMNet은 대규모 오디오 데이터셋으로 이미 학습된 모델로, 사이렌 소리나 유리가 깨지는 소리처럼 우리가 감지하려는 소리를 포함한 521개 종류의 이벤트를 별도의 학습 없이 바로 구분할 수 있다. 또한 MobileNet v1을 기반으로 설계되어 파라미터 수가 약 3.7M개, 모델 크기가 약 15MB (양자화할 경우에는 4MB) 에 불과하다. 따라서 별도의 AI 가속기 없이 Cortex-A53 CPU와 1GB 메모리를 갖춘 AM62x 보드에서도 충분히 실행할 수 있다.

모델을 실행하는 추론 엔진으로는 TensorFlow Lite를 사용한다. 일반 TensorFlow는 모델 학습 기능까지 포함하고 있어 설치 용량이 크기 때문에, 자원이 제한된 임베디드 보드에서 사용하기에는 부담이 된다. 반면 TensorFlow Lite는 이미 학습된 모델의 추론만을 목적으로 경량화된 실행 환경으로, 적은 메모리로도 동작하며 ARM 기반 장치에서의 실행을 염두에 두고 만들어졌다. 또한 YAMNet은 TensorFlow Lite 형식의 모델로도 공개되어 있어, 별도의 변환 과정 없이 보드에서 바로 사용할 수 있다.

이벤트가 감지되었을 때 사용자 기기로 알림을 보내는 방식으로는 MQTT를 선택하였다. HTTP와 같은 요청-응답 방식에서는 사용자 기기가 보드에 주기적으로 요청을 보내야 이벤트 발생 여부를 알 수 있어, 알림이 늦어지거나 불필요한 통신이 반복된다. 반면 MQTT는 클라이언트들이 브로커를 통해 특정 topic을 구독(subscribe)하고 메시지를 발행(publish)하는 구조를 사용하므로, 보드가 이벤트를 인식하는 즉시 이를 구독하고 있는 여러 기기에 알림이 전달된다. 또한 메시지 헤더가 작고 프로토콜이 단순하여 자원이 제한된 IoT 장치에서 널리 사용되고 있다.

## **B. 프로젝트 개발환경** {#b.-프로젝트-개발환경}

본 프로젝트의 개발은 Ubuntu 22.04 LTS가 설치된 호스트 PC에서 진행한다. 호스트 PC는 x86-64이지만 보드의 CPU가 ARM 아키텍처이므로 빌드에는 TI SDK에 포함된 gcc 툴체인을 사용하여 AArch64로 cross-compile한다.

타깃 장비는 CRZ 제작 AM62x 보드이며, 빌드한 이미지는 microSD 카드나 보드에 내장된 eMMC를 통해 부팅한다. 디버깅 과정에서는 screen이나 minicom으로 UART serial console에 접속하여 부팅 메시지를 확인하고, Android 환경에서는 ADB와 Fastboot를 함께 활용한다. 부팅 이후의 동작을 검증할 때에는 dmesg와 journalctl로 커널 및 시스템 로그를 살펴보고, ethtool과 lsusb로 이더넷 및 USB 장치가 제대로 인식되었는지 점검하며, Linux-RT 환경에서는 cyclictest를 활용한다.

포팅의 대상이 되는 SDK는 두 가지이다. Android SDK는 2026년 2월 23일에 배포된 11.00.01 버전으로, U-Boot 2025.01과 함께 Linux LTS 커널 6.12 위에서 동작하는 Android 16을 포함한다. Linux 및 Linux-RT SDK는 2026년 7월 31일에 배포된 12.01.00.05.03 버전이며, U-Boot 2026.01과 커널 6.18.38로 구성되어 있다.

# **4\. Goal/Problem & Requirements** {#4.-goal/problem-&-requirements}

본 프로젝트에서 해결하고자 하는 문제는 TI가 배포한 최신 SDK를 CRZ 보드에서 곧바로 사용할 수 없다는 점이다. TI SDK는 TI reference board를 대상으로 작성되었기 때문에, 하드웨어 구성이 다른 CRZ 보드에 적용하려면 별도의 수정이 필요하다. CRZ Technology는 지금까지 2023년에 배포된 SDK를 사용해 왔으나, 2026년에 새 SDK가 나오면서 이를 새 버전에 맞추어 수정해야 하는 상황이 되었다.

이에 따라 본 프로젝트의 목표는 세 가지로 정리할 수 있다. 첫째, 최신 Linux 및 Android SDK가 CRZ 보드에서 안정적으로 부팅되어야 한다. 둘째, Touchscreen LCD와 USB를 포함한 주요 Peripheral이 정상적으로 동작해야 한다. 셋째, 빌드와 플래시 과정을 누구나 그대로 따라 할 수 있도록 스크립트를 제작하고 문서화해야 한다.

포팅 대상인 보드는 CRZ Technology가 제작한 TI AM62x 커스텀 보드로, Quad Cortex-A53 CPU를 내장한 TI AM6254 SoC를 탑재하고, 1GB DDR4 메모리와 8GB eMMC, 그리고 microSD를 저장 장치로 사용한다. 이 보드에서는 Linux, Linux-RT, Android 세 가지 OS가 모두 지원되어야 하고, 2개의 Ethernet 포트와 2개의 USB 2.0 Host 포트, Touchscreen LCD, UART, LED가 정상적으로 동작해야 한다. 부팅 방식으로는 SD 카드 부팅과 eMMC 부팅을 모두 지원해야 한다.

이 밖에도 포팅이 성공적으로 이루어졌음을 입증할 수 있는 애플리케이션을 선정하여 구현하는 것이 추가 요구사항으로 주어졌다. 우리는 이상 상황에서 들리는 소리를 감지하면 사용자 기기로 알림을 보내고 화면에 표시하는 오디오 이벤트 인식 기능을 탑재하기로 결정하였다. 마이크에서 오디오 입력을 받아 전처리 후 YAMNet을 사용해 이벤트를 인식하고, 이벤트가 발생했을 경우 화면에 출력 및 사용자 기기로 알림을 전송할 수 있어야 한다.

# **5\. Approach** {#5.-approach}

앞서 설명한 목표를 달성하기 위해, 본 프로젝트는 최신 TI Processor SDK를 바탕으로 하되 Device Tree, configuration, driver 가운데 CRZ 보드와 reference board의 차이가 드러나는 부분을 찾아 수정하는 방식으로 포팅을 진행한다.

작업은 다섯 단계로 나누어 진행한다. 먼저 부트로더인 U-Boot를 보드에 맞게 수정하여 부팅의 기반을 마련하고, 이어서 커널과 Device Tree를 수정하여 OS가 보드의 하드웨어를 올바르게 인식하도록 한다. 세 번째 단계에서는 각 Peripheral에 필요한 드라이버 등의 지원을 구현하고 그 동작을 검증한다. 네 번째 단계에서는 오디오 이벤트 인식 기능을 구현하며, 마지막 단계에서 전체 과정을 매뉴얼 등 문서로 정리하고 실사용자를 위한 스크립트를 완성한다.

포팅 결과는 여러 측면에서 검증한다. 우선 U-Boot Console에 출력되는 로그를 통해 부트로더가 정상적으로 동작하는지 확인하고, Linux와 Android가 문제없이 부팅되는지를 여러 차례 반복하여 시험한다. 부팅 이후에는 4개의 A53 코어가 모두 사용되고 있는지, 그리고 Display, USB 포트, Ethernet 포트, LED 등의 Peripheral이 제대로 작동하는지 점검한다. 오디오 이벤트 인식 기능에 대해서는 동일한 오디오 파일이 주어졌을 때 기준 PC에서 실행한 것과 결과가 같은지 검증한다.

또한 실사용자를 위한 지원 방안도 함께 마련한다. 사용자가 복잡한 과정 없이 이미지를 빌드할 수 있도록 빌드 스크립트를 제공하고, 부팅용 SD 카드와 eMMC 설치용 SD 카드를 각각 만들 수 있는 스크립트도 제공한다. 그리고 직접 빌드하지 않아도 바로 사용할 수 있도록 완성된 이미지를 함께 배포하며, 이미지 빌드와 SD 카드 제작 방법을 자세히 설명하는 매뉴얼을 작성할 예정이다.

# **6\. Project Architecture** {#6.-project-architecture}

## **A. Architecture Diagram** {#a.-architecture-diagram}

## ![][image1]

![][image2]

## **B. Architecture Description** {#b.-architecture-description}

AM62x 보드의 부팅은 Boot ROM, R5 SPL, A53 SPL, U-Boot, 그리고 Linux 또는 Android의 순서로 진행되며, 각 단계는 다음 단계를 실행할 수 있는 환경을 마련한 뒤 제어를 넘겨주는 역할을 한다. 전원이 들어오면 가장 먼저 SoC에 내장된 Boot ROM이 실행되어, 부팅 매체에 저장된 R5 SPL을 on-chip SRAM으로 불러온다.

R5F 코어에서 실행되는 R5 SPL은 보드의 DRAM을 초기화하여 이후 단계들이 큰 메모리 공간을 사용할 수 있도록 하고, 다음 단계인 A53 SPL을 DRAM에 로드한다. R5 SPL 실행 전까지는 아직 DRAM이 초기화되지 않았기 때문에 사용할 수 없고, R5 SPL은 SoC의 on-chip SRAM에서 실행된다. R5 SPL이 저장된 것과 같은 파일에서 TIFS (TI Foundational Security) 펌웨어가 로드되어 SoC의 SMS (Security Management Subsystem) 에서 실행된다.

A53 SPL은 R5F 코어가 아니라 메인 CPU인 Cortex-A53 코어에서 실행된다. 이때 SoC가 가진 4개의 A53 코어 중 Core 0 하나에서만 실행된다. A53 SPL은 다음 단계인 U-Boot의 이미지를 DRAM에 로드한다. A53 SPL이 실행되면 R5F 코어는 DM (Device Manager) 펌웨어로 전환되어 전원, 클럭 등을 관리하는 역할로 바뀐다.

U-Boot는 커널 이미지와 보드의 하드웨어 구성을 설명하는 DTB (Device Tree Blob) 를 DRAM에 로드한 뒤 커널을 실행한다. U-Boot도 마찬가지로 Core 0에서만 실행된다.

마지막으로 Linux 커널이 실행된다. 커널은 Device Tree를 바탕으로 하드웨어를 초기화한다. 이 과정에서 나머지 3개의 코어가 시작된다. 또한 패널/터치 드라이버들이 작동하면서 LCD 화면과 터치 입력이 가능해진다. 커널이 init process를 실행 시작하면 Linux/Android를 사용할 수 있게 된다.

이 부팅 과정의 각 단계는 TI reference board를 기준으로 작성되어 있으므로, CRZ 보드에서 정상적으로 동작하려면 메모리 구성, 탑재된 구성품, 부팅 방식, 드라이버 등을 CRZ 보드의 특성을 반영하도록 수정해야 한다. 단계별로 필요한 구체적인 수정 사항은 7.C Modules에서 설명한다.

오디오 이벤트 인식 기능에서는 마이크로 들어온 소리가 사용자 기기와 화면에 알림으로 전달되기까지 일곱 개의 모듈을 차례로 거친다. 오디오 입력 모듈이 USB 포트에 연결된 마이크로부터 소리를 받아들인다. 전처리 모듈은 마이크마다 다를 수 있는 오디오 형식을 YAMNet이 요구하는 형식으로 변환한다. 프레임 분할 모듈은 연속적으로 들어오는 소리를 모델이 한 번에 처리할 수 있는 길이의 프레임들로 나누는 역할을 한다. 추론 모듈은 분할된 각 프레임의 소리가 어떤 종류인지 YAMNet을 실행하여 분류하는 역할을 한다. 이벤트 판정 모듈은 분류 결과 중 이상 상황에 해당하는 소리만 골라내기 위해 필요하고, 사용자 알림 모듈과 화면 출력 모듈은 그 결과를 사용자 기기와 LCD 화면으로 전달하여 사용자가 이상 상황을 알 수 있게 한다. 추가적으로, MQTT 브로커 모듈이 보드와 사용자 기기 간 메시지 전달을 구현한다. 각 모듈의 구체적인 요구사항은 7.C Modules에서 설명한다.

# **7\. Implementation Spec** {#7.-implementation-spec}

## **A. Input/Output Interface** {#a.-input/output-interface}

CRZ AM62x 보드에서 지원해야 하는 Input/Output Peripheral은 저장 장치, 통신 및 연결 장치, 그리고 입출력 장치로 나누어 볼 수 있다. 저장 장치로는 보드에 내장된 8GB eMMC와 microSD 카드 슬롯이 있으며, 두 장치 모두 부팅 이미지를 담는 부팅 매체이자 파일을 저장하는 공간으로 사용된다. microSD 카드는 이미지를 쉽게 교체할 수 있어 개발 및 설치 과정에 주로 활용되고, eMMC는 이미지를 보드에 설치하여 실제 운용 환경에서 사용하는 용도로 쓰인다.

통신 및 연결 장치로는 2개의 Ethernet 포트와 2개의 USB 2.0 Host 포트가 있다. USB 2.0 Host 포트는 마우스나 마이크와 같은 외부 USB 장치를 보드에 연결하는 데 사용된다. Ethernet 포트는 보드를 네트워크에 연결하여 인터넷에 접속하거나, 같은 네트워크에 있는 다른 기기와 데이터를 주고받는 데 사용된다. IP 주소가 정상적으로 할당되고 외부 기기와 통신할 수 있어야 한다.

입출력 장치로는 Touchscreen LCD, UART, LED가 있다. Touchscreen LCD는 60 pin 커넥터를 통해 보드에 연결되며, 화면 출력과 터치 입력을 함께 담당하여 사용자가 보드를 직접 조작할 수 있게 한다. UART는 serial console을 제공하는 장치로, 부팅 과정의 로그를 확인하거나 화면과 네트워크가 동작하지 않는 상태에서도 보드에 명령을 입력할 수 있게 해 주므로 개발과 디버깅 과정에서 특히 중요하다. LED는 보드의 동작 상태를 외부에서 간단히 확인할 수 있도록 표시하는 용도로 사용된다.

Android는 Linux 커널을 기반으로 동작하므로 하드웨어와 커널 드라이버는 Linux와 대부분 공통으로 사용하지만, 커널에서 인식한 장치를 Android HAL(Hardware Abstraction Layer)과 Framework를 통해 애플리케이션에서도 사용할 수 있어야 한다. 따라서 Android에서는 커널 드라이버의 정상 동작뿐 아니라 Android 시스템에서 해당 장치에 접근할 수 있는지도 확인해야 한다.

Touchscreen LCD의 경우 커널에서 디스플레이 패널과 터치 컨트롤러를 정상적으로 인식한 뒤, Android 그래픽 출력 시스템과 입력 시스템에서도 이를 사용할 수 있어야 한다. Android 부팅 후 사용자 인터페이스가 정상적으로 표시되어야 하며, 터치 입력의 좌표가 1280×800 해상도에 맞게 전달되어야 한다.

Android의 개발 및 디버깅을 위해서는 ADB(Android Debug Bridge)도 사용할 수 있어야 한다. ADB를 통해 Android 시스템에 접근하여 로그를 확인하고, 애플리케이션을 설치하거나 실행할 수 있어야 한다. 특히 화면 출력이나 터치 입력이 정상적으로 동작하지 않는 경우 UART와 ADB를 이용해 문제를 분석할 수 있도록 한다.

오디오 이벤트 인식 기능은 USB 포트, Ethernet 포트, Touchscreen LCD의 세 가지 Input/Output Peripheral을 사용한다. 표준 USB 오디오 규격인 UAC를 따르는 마이크를 USB 포트에 연결하여 마이크로 들어오는 소리를 입력으로 받는다. 출력으로는 이벤트가 감지되면 Ethernet 포트를 통해 MQTT 프로토콜로 사용자 기기에 알림을 전송하고, 동시에 Touchscreen LCD에 감지 결과를 표시한다. 따라서 오디오 이벤트 인식 기능이 정상적으로 동작한다는 것은 이 세 가지 입출력 Peripheral의 포팅이 올바르게 이루어졌음을 함께 보여준다. 

## **B. Inter Module Communication Interface** {#b.-inter-module-communication-interface}

마이크에서 얻어진 raw data는 오디오 입력, 전처리, 프레임 분할, 추론, 이벤트 판정 모듈, 화면 출력 및 사용자 알림 모듈에서 차례로 전달되어야 한다. 보드 내부 모듈 간 통신에는 Unix domain socket을 사용한다. 데이터를 보내는 모듈은 정해진 경로에 소켓을 만들어 서버 역할을 하고, 그 데이터를 받는 다음 모듈은 정해진 경로로 접근하는 클라이언트 역할을 해야 한다. 예를 들어 전처리 모듈은 오디오 입력이 생성한 소켓을 읽어 오디오를 받고, 변환한 결과를 자신이 만든 소켓으로 내보내어 프레임 분할 모듈이 읽도록 해야 한다. 화면 출력 모듈과 알림 모듈은 둘 다 이벤트 판정 모듈의 소켓을 읽는다. 모듈 간 이동해야 하는 데이터의 내용은 아래 표와 같다.

| 보내는 모듈 | 받는 모듈 | 데이터 |
| :---: | :---: | :---: |
| 오디오 입력 | 전처리 | 가공되지 않은 마이크 오디오 (48 kHz, 스테레오, 16-bit 정수) |
| 전처리 | 프레임 분할 | 전처리된 오디오 (16 kHz, 모노, \[-1.0, 1.0\] float32) |
| 프레임 분할 | 추론 | \[-1.0, 1.0\] float32 값 15,600개 (16kHz에서 0.975초 분량) |
| 추론 | 이벤트 판정 | 521개 클래스의 점수 |
| 이벤트 판정 | 화면 출력 | 발생한 이벤트 종류와 그 점수 \+ 점수가 가장 높은 클래스, 각 클래스의 점수 |
|  | 사용자 알림 | 발생한 이벤트 종류와 그 점수 |

Android에서는 오디오 수집과 화면 출력을 하나의 애플리케이션으로 구현하고, 전처리 및 추론 모듈은 해당 애플리케이션 프로세스에서 실행되는 네이티브 라이브러리로 구성한다. AudioRecord로 수집한 PCM 데이터는 JNI를 통해 네이티브 모듈에 전달하며, 전처리와 추론은 별도의 작업 스레드에서 수행한다. 네이티브 모듈 간에는 버퍼 큐로 데이터를 전달하고, 뮤텍스와 조건 변수를 사용하여 접근을 동기화한다. 이에 따라 Android에서는 Linux의 Unix domain socket 기반 프로세스 간 통신을 애플리케이션 내부의 스레드 간 통신 구조로 변경한다.

Android에서는 추론 모듈에서 생성한 이벤트 종류, 점수, 발생 시각은 JNI 콜백을 통해 애플리케이션에 전달한다. 애플리케이션은 전달받은 결과를 메인 스레드에서 Android UI에 반영하고, 동일한 이벤트 정보를 MQTT 전송 모듈에 전달한다. 오디오 입력부터 추론 결과 표시까지 하나의 프로세스 안에서 처리하므로, 모듈 연결을 위한 Binder IPC나 별도의 소켓 접근 권한 및 SELinux 정책 추가는 사용하지 않는다.

이벤트 발생 시 알림 모듈에서 보드 밖의 사용자 기기로 메시지를 보내기 위해서는 MQTT 프로토콜을 사용한다. MQTT 프로토콜을 따라 알림 모듈이 브로커 모듈에 메시지를 전달하고, 브로커는 수신자로 등록된 사용자 기기들에게 메시지를 전달해야 한다. 이를 위해 브로커 모듈은 보드의 특정 포트를 통해 외부의 사용자 기기들이 접속할 수 있도록 해야 한다. 사용자 기기는 브로커 모듈과 연결하여 알림 모듈이 전달하는 메시지를 받을 수 있어야 한다. 메시지는 발생 시각, 보드 이름, 이벤트 종류, 점수 등의 필드를 가지는 JSON 형식을 가진다.

## **C. Modules** {#c.-modules}

Boot ROM을 제외한 부팅 과정의 각 단계는 CRZ 보드를 지원하기 위해 아래와 같이 수정되어야 한다. Boot ROM은 SoC 내부에 내장된 코드로 수정이 불가능하므로 변경 사항이 없다. 

모든 단계에서 DRAM 용량을 TI reference board의 2GB가 아닌 CRZ 보드의 1GB로 반영하기 위해 Device Tree를 수정해야 한다. 또한 HDMI, 오디오, I/O expander 등 CRZ 보드에 없는 부품들을 Device Tree를 수정해 삭제해야 한다.

R5 SPL, A53 SPL, U-Boot에서는 보드를 인식하기 위해 실행되는 코드를 공통적으로 수정해야 한다. TI reference board는 보드의 정보를 별도의 EEPROM에 담고 부팅 과정에서 이를 읽어 보드를 인식한다. 하지만 CRZ 보드는 해당 EEPROM을 가지지 않기 때문에 이 방식을 사용할 수 없다. 따라서 소프트웨어에서 보드 정보를 고정값으로 설정하여 보드가 인식되도록 해야 한다.

Boot ROM은 R5 SPL과 같은 파일에서 TIFS 펌웨어를 읽어 실행시키는데, TI SDK는 보안을 위한 기능이 탑재된 AM62x SoC를 기준으로 하고 있어 HS-FS (High Security \- Field Securable) 펌웨어를 빌드한다. 하지만 CRZ 보드의 SoC는 해당 기능이 없기 때문에 HS-FS 펌웨어를 사용하여 부팅할 수 없다. 따라서 GP (General Purpose) 펌웨어가 이미지에 탑재되도록 수정해 주어야 한다.

U-Boot에서는 다음 단계인 커널이 보드에 연결된 Touchscreen LCD 화면을 인식할 수 있도록 해야 한다. 이를 위해서는 커널과 함께 로드되는 DTB에 화면 장치에 대한 정보를 추가해야 한다. LCD 화면과 터치 컨트롤러 정보를 담은 DTB overlay를 작성하고, U-Boot가 커널 DTB를 로드할 때 이 overlay를 함께 적용하도록 설정해야 한다.

Linux/Android 단계에서는 패널 드라이버를 수정하여 CRZ에서 사용하는 bsd101wx1-300 디스플레이의 정보를 지원되는 디스플레이 목록에 추가해야 한다. 또한 터치 입력을 위한 드라이버를 수정하여 그 해상도를 디스플레이에 맞춘 1280×800으로 설정해야 한다. USB 포트와 Ethernet을 지원할 수 있도록 관련 부분을 수정해야 한다.

Android는 Linux와 동일하게 Boot ROM, R5 SPL, A53 SPL, U-Boot를 거쳐 부팅하지만, U-Boot 이후에는 Android의 부팅 구조에 맞는 이미지들을 로드해야 한다. 본 프로젝트에서는 TI Processor SDK Android 11.00.01을 기준으로 Android 16과 Linux 6.12 기반 커널을 사용한다. 따라서 CRZ 보드에 맞게 수정한 Bootloader뿐 아니라 Android 커널과 시스템 이미지를 함께 빌드하고 설치할 수 있어야 한다.

Android는 `boot.img`, `vendor_boot.img`, `dtbo.img`, `super.img` 등 여러 이미지와 파티션으로 구성된다. 각 이미지에는 커널, 부팅에 필요한 구성 요소, Device Tree Overlay, Android 시스템 및 Vendor 구성 요소 등이 포함된다. 실제 SDK에서 사용하는 이미지 및 파티션 구성에 맞춰 CRZ 보드용 커널과 Device Tree를 반영하고, SD 카드 및 eMMC 부팅을 지원해야 한다.

또한 Android Verified Boot(AVB)와 부팅 이미지 검증 설정이 부팅에 영향을 줄 수 있으므로, CRZ 보드의 SoC 보안 구성과 개발용 이미지의 부팅 설정을 확인해야 한다.

Android용 커널은 TI Android SDK에서 제공하는 커널 소스와 빌드 시스템을 사용하여 빌드해야 한다. Android 16 기반 SDK의 Kleaf/Bazel 커널 빌드 환경에서 CRZ 보드에 필요한 드라이버 변경사항과 Device Tree 수정사항을 반영해야 한다. 또한 LCD와 터치스크린을 위한 Device Tree Overlay가 Android의 DTBO 이미지 구성에도 포함되고 부팅 과정에서 정상적으로 적용되도록 해야 한다.

Android에서는 커널 드라이버의 정상 동작뿐 아니라 Android HAL 및 Framework에서 각 Peripheral을 사용할 수 있는지 확인해야 한다. 디스플레이는 Android 그래픽 시스템에서 정상적으로 화면을 출력해야 하며, 터치스크린은 Android 입력 시스템에서 터치 이벤트를 처리할 수 있어야 한다. USB 마이크는 Audio HAL을 통해 입력 장치로 제공되어야 하고, Ethernet은 Android 네트워크 관리 시스템을 통해 애플리케이션이나 서비스에서 사용할 수 있어야 한다.

오디오 이벤트 인식 기능의 각 모듈에 대한 요구사항은 다음과 같다. 모든 모듈은 공통적으로 Unix domain socket에 연결해 앞쪽 단계에서 데이터를 전달받고, 정해진 경로에 또다른 소켓을 만들어 뒤쪽 단계의 연결을 받아들이고 다음 데이터를 전송할 수 있어야 한다. 또한 모든 모듈은 INI 형식의 설정 파일을 읽어 주어진 설정에 맞게 동작할 수 있어야 한다.

오디오 입력 모듈은 설정 파일에 주어진 이름으로부터 USB 포트에 연결된 UAC 호환 마이크의 device name을 알아내고, ALSA를 통해 해당 마이크를 열 수 있어야 한다. 보드가 동작하는 동안 마이크를 통해 끊김 없이 오디오를 받아들일 수 있어야 한다. 이때 받아들이는 오디오 형식은 사용할 마이크가 지원하는 48 kHz, 스테레오, 16-bit 정수이다.

Android의 오디오 입력 모듈은 AudioRecord 기반으로 구현한다. 커널의 USB Audio 드라이버와 Android Audio HAL 및 오디오 정책을 구성하여 USB 마이크가 입력 장치로 제공되도록 하고, 애플리케이션에서 RECORD\_AUDIO 권한을 획득한 뒤 해당 장치를 선택한다. AudioRecord로 수집한 PCM 데이터는 JNI를 통해 애플리케이션 프로세스 내부의 네이티브 전처리 모듈에 전달한다.

전처리 모듈은 오디오 입력 모듈에서 전달받은 오디오 데이터를 YAMNet이 요구하는 오디오 형식으로 변환해야 한다. 우선 스테레오 입력을 모노로 변환하고 16-bit 정수에서 \-1.0에서 1.0 사이의 32-bit float 값으로 정규화해야 한다. 그리고 48kHz 오디오를 리샘플링하여 16 kHz로 변환하여야 한다.

프레임 분할 모듈은 전처리된 신호를 YAMNet 모델의 입력 길이인 0.975초 프레임으로 나누어야 한다. 16 kHz의 sampling rate에서는 프레임 당 15,600개의 샘플이 담기게 된다. 이때 0.48초마다 새 프레임을 만들어 이웃한 프레임이 절반씩 겹치도록 해야 한다. 이렇게 하면 0.48초보다 짧은 소리는 프레임 경계에 걸쳐 발생하더라도 반드시 어느 한 프레임 안에 온전히 포함된다.

추론 모듈은 TensorFlow Lite를 이용해 YAMNet의 .tflite 모델을 불러오고, 각 프레임에 대해 모델을 실행하여 521개 클래스 각각에 대한 점수를 계산해야 한다. 이렇게 계산된 점수는 같은 오디오 파일에 대해 기준 PC에서 YAMNet 레퍼런스 코드를 실행한 결과와 같아야 한다.

Android의 추론 모듈은 애플리케이션 프로세스 내부의 네이티브 라이브러리로 구현하며, TensorFlow Lite C++ 런타임을 함께 포함한다. Linux 및 기준 PC와 동일한 모델 파일을 사용하고, 네이티브 전처리 모듈에서 전달받은 16kHz 모노 오디오를 모델 입력 형식에 맞게 구성하여 별도의 작업 스레드에서 CPU 추론을 수행한다. 

이벤트 판정 모듈은 521개 클래스 중 사이렌 소리나 물체가 부서지는 소리와 같은 관심 클래스에 대해, 임계값과 해당 클래스에서 산출된 점수를 비교하여, 이벤트 발생 여부를 판단해야 한다. 관심 클래스와 각 클래스의 임계값을 설정 파일에 저장하여 쉽게 수정 가능하도록 한다. 이벤트가 실제로 인식된 경우에만 선택적으로 화면 출력 모듈과 사용자 알림 모듈에 이벤트 정보를 보낼 수 있어야 한다. Android에서는 이벤트 종류·점수·발생 시각을 JNI 콜백을 통해 애플리케이션에 전달하여 화면 표시와 MQTT 알림에 사용한다.

화면 출력 모듈은 이벤트가 발생했을 때 Touchscreen LCD에 감지 결과를 표시하여, 보드 근처에 있는 사용자도 이벤트를 바로 확인할 수 있도록 해야 한다. 사용자가 이벤트를 해제하거나 일정 시간이 지났을 때 이벤트 표시를 사라지게 해야 한다. 이벤트가 없을 때에도 현재 들리는 소리의 종류와 입력 크기를 함께 보여주어, 기능이 정상적으로 동작하고 있는지 화면에서 확인할 수 있어야 한다.

알림 모듈은 이벤트가 감지되면 감지된 소리의 종류, 발생 시각, 보드 이름 등의 정보를 담은 MQTT 메시지를 브로커 모듈로 전송하여, 이를 구독하고 있는 사용자 기기에 이벤트 발생 알림이 전달되도록 해야 한다. 

알림 모듈이 발행한 메시지를 사용자 기기로 전달하기 위해, 보드에서는 MQTT 브로커 모듈이 함께 실행되어야 한다. 브로커 모듈은 Ethernet 포트를 통해 사용자 기기의 접속을 받고, 알림 모듈이 전송한 메시지를 해당 주제를 구독한 모든 기기에 전달해야 한다. 사용자 기기가 접속할 수 있도록 설정 파일에 담긴 포트 번호를 사용해야 한다.

위의 모든 모듈은 각각 독립된 프로그램으로 구현되어 보드가 부팅되면 자동으로 실행되어야 하며, 어느 한 모듈이 다시 시작되더라도 나머지 모듈과의 연결이 자동으로 복구되어야 한다.

Android에서는 오디오 입력과 화면 출력을 하나의 애플리케이션으로 구현하고, 전처리·프레임 분할·추론·이벤트 판정 모듈은 동일 프로세스의 네이티브 라이브러리로 구성한다. 지속적인 오디오 수집은 microphone 유형의 Foreground Service에서 관리하며, AudioRecord와 네이티브 모듈은 JNI 및 버퍼 큐로 연결한다. 애플리케이션은 기본 홈 앱으로 설정하고, 최초 마이크 권한 승인 후 화면에 활성화된 상태에서 인식 서비스를 시작하도록 구성한다. USB 마이크 연결 해제나 MQTT 연결 단절 시에는 해당 장치와 연결을 다시 초기화하며, 앱 프로세스가 재시작되면 설정과 전체 처리 파이프라인을 다시 초기화한다. MQTT 브로커는 별도의 시스템 네이티브 서비스로 구성하여 init.rc에서 실행과 재시작을 관리한다.

# **8\. Current Status** {#8.-current-status}

현재까지 두 사람이 공통으로 진행한 작업은 다음과 같다. 먼저 회사로부터 과제 수행에 필요한 장비를 받고, 담당자와의 면담을 통해 프로젝트의 요구사항을 구체적으로 파악하였다. 이후 CRZ AM62x 보드의 사양을 분석하고 TI SDK의 구성을 살펴보았으며, 이를 바탕으로 개발 환경을 구축하였다. 또한 CRZ에서 기존에 사용하던 구 버전 소프트웨어와 최신 TI SDK를 비교하여 두 버전 사이의 차이를 파악하였다. 그 이후의 포팅 작업은 OS별로 나누어 진행하고 있으며, 각각의 진행 상황은 다음과 같다.

Linux 쪽에서는 먼저 U-Boot 포팅을 완료하였다. 보드에서 U-Boot console이 정상적으로 실행되었고, console에 입력한 명령어들도 올바르게 동작하는 것을 확인하였다. 이어서 microSD 에서의 Linux (Ubuntu 26.04) 부팅에 성공하였다. 부팅 후 UART console에서 기본적인 터미널 명령어들을 실행해 본 결과 모두 정상적으로 동작하는 것을 확인하였다. 새로 작성한 파일이 재부팅 후에도 유지되는 것을 확인하였다.

Peripheral 지원 역시 상당 부분 진행되었다. Display가 정상적으로 켜졌고, Weston을 통해 GUI가 화면에 렌더링되는 것을 확인하였다. 터치 입력은 터치 조작이 필요한 데모 애플리케이션을 실행하여 시험하였으며, 터치가 올바르게 인식되었다. USB 포트는 현재 하나가 동작하는 것을 확인하였고, 이를 통해 마우스와 마이크를 연결하여 사용할 수 있었다. Ethernet 포트도 정상적으로 동작하여 보드에서 인터넷에 접속할 수 있었다. 같은 네트워크에 있는 다른 기기에서 ssh를 사용해 보드에 접속하는 것도 가능하였다.

Android 측면에서는 TI Android SDK 11.00.01에 포함된 U-Boot 2025.01 및 커널 6.12를 기반으로 이식 작업을 수행하고 있다. CRZ 보드에서 최신 U-Boot의 구동을 완료하였으며, microSD 카드의 UHS 모드를 해제하여 부팅 호환성을 확보하였다. 또한 2023년 CRZ 기존 자료와 신규 커널의 Device Tree를 비교·분석하고, CRZ 보드의 1GB 메모리와 저장장치 및 주요 Peripheral 설정을 반영한 커널 Image와 DTB를 빌드하여 Linux 6.12.23 커널 포팅을 완료하였다. 실제 보드에서 해당 커널을 이용한 Ubuntu 20.04.6 부팅 및 로그인을 확인하였으며, microSD·eMMC 장치 인식과 microSD의 ext4 루트 파일시스템 마운트를 검증하였다. 

Android 16 사전빌드 이미지 패키지를 확보하고 boot, init\_boot, vendor\_boot, super 등 주요 이미지 구성을 확인하였다. flashall.sh를 분석하여 Fastboot 기반의 GPT 파티션 구성과 A/B 이미지 플래싱 절차를 파악하였다. 현재는 CRZ 보드에 적용하기 위한 부트로더 지원과 부팅 구성을 검토하는 단계이며, 실제 플래싱은 수행하지 않았다.

# **9\. Future Work** {#9.-future-work}

Linux 쪽에서는 앞으로 보드에 탑재된 모든 Peripheral을 지원하도록 구현을 마무리하고, 각 장치가 올바르게 동작하는지 검증할 계획이다. 현재는 microSD 카드를 통한 부팅을 확인한 상태이므로, eMMC에서도 부팅할 수 있도록 작업을 이어간다. 아울러 Linux-RT 부팅을 구현하고, 오디오 이벤트 인식 기능을 Linux 환경에 탑재할 예정이다. 마지막으로 다른 사람이 같은 결과를 재현할 수 있도록, 전체 과정을 정리한 매뉴얼을 작성할 것이다.

Android 영역에서는 기존 Ubuntu 환경의 DTB와 상세 명세서를 바탕으로 커널 6.12용 CRZ 맞춤형 Device Tree를 구축하고, RAM·eMMC 및 Peripheral 관련 설정을 반영할 계획이다. 이후 CRZ U-Boot의 Fastboot 지원 여부와 보드에 적합한 파티션·부팅 구성을 확인하고, 커널 및 Android 시스템 이미지를 제작하여 Android 16 부팅을 검증한다. 또한 SD·eMMC 부팅과 Display, Touch, Ethernet 등 주요 Peripheral의 동작을 테스트하고, WM8960 오디오 오류를 분석하는 한편 USB 마이크를 활용한 장치 인식 및 녹음 기능을 검증한다. 이후 이상 음향 분류 애플리케이션을 포팅하여 오디오 전처리, TensorFlow Lite 추론, Display 출력 및 MQTT 알림 기능을 연동하고, 최종적으로 검증된 전체 빌드 및 이미지 작성·설치 절차를 매뉴얼로 정리할 예정이다.

공통으로 수행할 작업도 남아 있다. 먼저 각자 완성한 포팅 결과를 서로 교차 검증하여, 보드가 Linux/Android에서 정상적으로 동작하는지를 점검한다. 오디오 이벤트 인식 기능에 대해서는 같은 오디오 파일을 주었을 때 기준 PC와 같은 결과를 보여줄 수 있는지 검증한다.

최종 시연은 OS 포팅 결과와 오디오 이벤트 인식 기능을 차례로 보여주는 방식으로 진행할 예정이다. 먼저 eMMC에 설치된 Linux와 Android를 각각 부팅하고, UART console에 출력되는 로그를 통해 U-Boot부터 OS 실행까지의 과정이 정상적으로 이루어지는 것을 보여준다. 부팅 이후에는 OS 및 커널 버전과 CPU 코어 수, 메모리 용량을 확인하여 요구된 버전의 OS가 실행되고 보드의 하드웨어가 올바르게 인식된다는 것을 보여준다. 이어서 GUI 화면 출력과 터치 입력, USB 장치 연결, Ethernet 통신을 시연하여 주요 Peripheral의 동작을 확인한다.

다음으로 오디오 이벤트 인식 기능을 실행하여, 평상시에는 화면에 현재 들리는 소리의 종류와 입력 크기가 표시되는 모습을 보여준다. 이후 미리 준비한 사이렌 소리와 유리가 깨지는 소리 등의 오디오 파일을 외부 기기에서 재생하여, USB 마이크로 수집된 소리를 프로그램이 인식하는지 확인한다. 이벤트가 감지되면 화면에 해당 이벤트의 종류와 점수가 표시되고, 동시에 MQTT를 통해 연결된 사용자 기기에 알림이 수신되는 모습을 시연한다. 이 과정을 Linux와 Android에서 각각 수행하여, 두 환경에서 오디오 입력부터 추론, 화면 출력, 네트워크 알림까지 전체 기능이 정상적으로 동작함을 보여줄 예정이다.

# **10\. Division & Assignment of Work** {#10.-division-&-assignment-of-work}

| 항목 | 담당자 |
| :---: | :---: |
| Android SDK에서 U-Boot 포팅 | 박현종 |
| Android SDK 포팅 / Android image build | 박현종 |
| Android에서 Peripheral 지원 및 검증 | 박현종 |
| Android에서 오디오 이벤트 인식 기능 탑재 | 박현종 |
| Linux SDK에서 U-Boot 포팅 | 노윤철 |
| Linux \+ Linux-RT SDK 포팅 / Linux image build | 노윤철 |
| Linux에서 Peripheral 지원 및 검증 | 노윤철 |
| Linux에서 오디오 이벤트 인식 기능 탑재 | 노윤철 |
| Linux/Android 정상 작동 여부 상호 검증 | 공통 |
| 오디오 이벤트 인식 기능 검증 | 공통 |

# **11\. Schedule** {#11.-schedule}

| 내용 | 9월 |  |  | 10월 |  |  |  | 11월 |  |  |  | 12월 |  |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
|  | 3주 | 4주 | 5주 | 1주 | 2주 | 3주 | 4주 | 1주 | 2주 | 3주 | 4주 | 1주 | 2주 |
| 장비 수령, 담당자 면담 통해 요구사항 파악 |  |  |  |  |  |  |  |  |  |  |  |  |  |
| 기존 소프트웨어 분석 / TI SDK 내용 파악 / 개발환경 구축 |  |  |  |  |  |  |  |  |  |  |  |  |  |
| U-Boot bring-up |  |  |  |  |  |  |  |  |  |  |  |  |  |
| Android porting (박현종) |  |  |  |  |  |  |  |  |  |  |  |  |  |
| Linux porting (노윤철) |  |  |  |  |  |  |  |  |  |  |  |  |  |
| Peripheral validation |  |  |  |  |  |  |  |  |  |  |  |  |  |
| 오디오 이벤트 인식 기능 구현 |  |  |  |  |  |  |  |  |  |  |  |  |  |
| Documentation \+ User scripts |  |  |  |  |  |  |  |  |  |  |  |  |  |

# 

# 

# **◆ \[Appendix\] Detailed Implementation Spec** {#◆-[appendix]-detailed-implementation-spec}

## **A. 설정 파일 인식** {#a.-설정-파일-인식}

### **a. config\_parse()** {#a.-config_parse()}

int config\_parse(const char \*path, app\_config\_t \*configuration, char \*err, size\_t errlen\_max);

\* app\_config\_t: 파싱된 설정을 저장하기 위한 타입

INI 형식으로 저장된 설정(configuration) 파일을 파싱한다. 설정으로는 마이크 이름, 모델 경로, 관심 클래스, 클래스별 임계값 등이 있다.

## **B. 모듈 간 데이터 전달** {#b.-모듈-간-데이터-전달}

### **a. listen\_on\_socket()** {#a.-listen_on_socket()}

server\_t \*listen\_on\_socket(const char \*path);

\* server\_t: Unix domain socket과 관련 metadata를 저장하기 위한 타입

앞쪽 모듈에서 Unix domain socket을 만들고 뒤쪽 모듈의 연결을 기다린다.

### **b. connect\_to\_socket()** {#b.-connect_to_socket()}

int connect\_to\_socket(const char \*path);

뒤쪽 모듈에서 앞쪽 모듈의 Unix domain socket에 연결한다. 앞쪽 모듈이 아직 시작되지 않았으면 소켓이 생길 때까지 재시도한다.

### **c. send\_to\_socket()** {#c.-send_to_socket()}

int send\_to\_socket(server\_t \*server\_socket, const void \*message, size\_t len);

연결된 뒤쪽 모듈에 메시지를 보낸다. Non-blocking으로 구현한다.

### **d. receive\_on\_socket()** {#d.-receive_on_socket()}

ssize\_t receive\_on\_socket(int fd, void \*message, size\_t max\_len);

앞쪽 모듈이 보낸 메시지를 받는다.

## **C. 오디오 입력 모듈** {#c.-오디오-입력-모듈}

### **a. find\_device\_name()** {#a.-find_device_name()}

int find\_device\_name(const char \*mic\_name, char \*device\_name, size\_t max\_len);

설정 파일의 마이크 이름을 사용해 USB 마이크의 device name을 찾는다.

### **b. open\_mic()** {#b.-open_mic()}

mic\_reader\_t \*open\_mic(const char \*device\_name, unsigned rate, unsigned channels, unsigned samples\_in\_buffer);

\* mic\_reader\_t: ALSA 핸들(snd\_pcm\_t), 실제 적용된 오디오 형식(sampling rate, 채널 수), ALSA 버퍼가 저장하는 샘플 수 등을 저장하기 위한 타입

마이크를 열어 마이크의 오디오 형식을 지정한다. 실제 적용된 오디오 형식은 다를 수 있다.

### **c. read\_mic()** {#c.-read_mic()}

long read\_mic(mic\_reader\_t \*reader, void \*samples, size\_t sample\_count);

마이크에서 오디오를 주어진 샘플 개수만큼 읽는다.

## **D. 전처리 모듈** {#d.-전처리-모듈}

### **a. convert\_to\_mono\_f32()** {#a.-convert_to_mono_f32()}

void convert\_to\_mono\_f32(const void \*samples, size\_t sample\_count, sample\_format\_t format, unsigned channels, float \*mono\_samples);

\* sample\_format\_t: 오디오 입력 모듈에서 받은 샘플들의 데이터 타입(16-bit, 24-bit 등)을 나타내기 위한 타입

마이크에서 받은 정수 샘플을 \[-1.0, 1.0\] 범위의 float32로 바꾸고, 여러 채널을 평균 내어 모노로 만든다.

### **b. resample()** {#b.-resample()}

size\_t resample(resampler\_t \*resampler, const float \*mono\_samples, size\_t sample\_count, float \*preprocessed\_samples, size\_t max\_preprocessed\_samples);

\* resampler\_t: input/output sampling rate와 resampler 상태를 저장하기 위한 타입

마이크의 sampling rate에서 YAMNet 입력 sampling rate인 16 kHz로 변환한다.

## **E. 프레임 분할 모듈** {#e.-프레임-분할-모듈}

### **a. add\_samples\_to\_buffer()** {#a.-add_samples_to_buffer()}

void add\_samples\_to\_buffer(framer\_t \*framer, const float \*preprocessed\_samples, size\_t preprocessed\_sample\_count);

\* framer\_t: 프레임이 차기 전까지 샘플들을 모아 두는 버퍼와 프레임/hop 크기를 저장하기 위한 타입

전처리 모듈에서 받은 샘플들을 버퍼에 추가한다.

### **b. get\_frame()** {#b.-get_frame()}

int get\_frame(framer\_t \*framer, float \*frame);

버퍼에서 프레임 하나가 완성되면 프레임을 전달하고 hop만큼 앞으로 이동한다.

## **F. 추론 모듈** {#f.-추론-모듈}

### **a. load\_model()** {#a.-load_model()}

model\_t \*load\_model(const char \*model\_path, int num\_threads);

\* model\_t: TensorFlow Lite 모델과 interpreter, input/output tensor를 저장하기 위한 타입

YAMNet .tflite 모델을 TensorFlow Lite로 불러오고, 모델이 사용할 수 있는 최대 thread 수를 지정한다.

### **b. run\_model()** {#b.-run_model()}

int run\_model(model\_t \*model, const float \*frame, float \*scores);

프레임 하나에 YAMNet을 실행해 521개 클래스 각각의 점수를 얻어낸다.

## **G. 이벤트 판정 모듈** {#g.-이벤트-판정-모듈}

### **a. check\_event()** {#a.-check_event()}

int check\_event(detector\_t \*detector, const float \*scores, event\_t \*event);

\* detector\_t: 관심 클래스별 임계값과 cooldown 상태를 저장하기 위한 타입

\* event\_t: 감지된 이벤트(클래스, 점수, 시각 등)를 나타내는 타입

관심 클래스의 점수가 임계값 이상이면 이벤트를 만든다. 같은 종류의 이벤트가 연속으로 생기지 않도록 cooldown 기간을 정한다.

### **b. find\_top\_n()** {#b.-find_top_n()}

void find\_top\_n(const float \*scores, int n, frame\_result\_t \*top\_n\_result);

\* frame\_result\_t: 한 프레임의 상위 N개 클래스·점수를 나타내는 타입

한 프레임에서 점수가 가장 높은 클래스 N개를 고른다.

## **H. 화면 출력 모듈** {#h.-화면-출력-모듈}

### **a. create\_window()** {#a.-create_window()}

ui\_t \*create\_window(int width, int height);

\* ui\_t: 화면을 그릴 shared buffer 등 Wayland 관련 정보를 담는 타입

Weston에 연결하고 window를 만든다.

### **b. update\_display\_state()** {#b.-update_display_state()}

void update\_display\_state(display\_state\_t \*state, const event\_t \*event, const frame\_result\_t \*top\_n\_result);

\* display\_state\_t: 화면 모드(기본/이벤트 발생), 표시할 점수, 이벤트 등 현재의 화면 상태를 저장하기 위한 타입

이벤트 판정 모듈에서 받은 결과로 화면 상태를 바꾼다. 상위 N개 점수를 갱신하고, 이벤트가 발생했으면 이벤트 발생 모드로 바꾼다. 실제 그리기는 화면 모드에 따라 render\_default() 또는 render\_alert()를 호출해서 한다.

### **c. render\_default()** {#c.-render_default()}

void render\_default(cairo\_t \*cairo\_handle, int width, int height, const display\_state\_t \*state);

\* cairo\_t: Cairo 라이브러리에서 사용되는 핸들 타입

이벤트가 없을 때의 기본 화면을 그린다. 실시간 상위 N개 클래스와 점수를 보여 준다.

### **d. render\_alert()** {#d.-render_alert()}

void render\_alert(cairo\_t \*cairo\_handle, int width, int height, const display\_state\_t \*state);

이벤트가 감지됐을 때의 이벤트 발생 화면을 그린다. 감지된 클래스, 점수, 시각을 보여 준다.

## **I. 사용자 알림 모듈** {#i.-사용자-알림-모듈}

### **a. connect\_to\_broker()** {#a.-connect_to_broker()}

mqtt\_connection\_t \*connect\_to\_broker(const char \*broker\_ip, uint16\_t broker\_port, const char \*client\_id);

\* mqtt\_connection\_t: broker와의 연결 관련 metadata를 저장하기 위한 타입

지정된 ip와 port의 broker에 연결한다. CONNECT 패킷을 보내고 CONNACK 응답을 기다린다.

### **b. build\_json()** {#b.-build_json()}

int build\_json(const event\_t \*event, const char \*device\_id, char \*message, size\_t max\_len);

감지된 이벤트를 사용자 기기로 보낼 JSON 문자열로 만든다. 형식은 아래와 같다.

{"device":"device-name","event\_id":id-for-event,"class":"event-type","score":score,"threshold":threshold,"time":"time of event"}

### **c. send\_to\_broker()** {#c.-send_to_broker()}

int send\_to\_broker(mqtt\_connection\_t \*connection, const char \*topic, const void \*message, size\_t len);

지정된 topic으로 메시지를 broker에 전송한다.

## **J. MQTT 브로커 모듈** {#j.-mqtt-브로커-모듈}

### **a. handle\_packet()** {#a.-handle_packet()}

int handle\_packet(broker\_t \*broker, session\_t \*session, const mqtt\_packet\_t \*packet);

\* broker\_t: 연결된 클라이언트 목록을 저장하기 위한 타입

\* session\_t: 클라이언트 하나의 연결 상태와 구독 topic을 저장하기 위한 타입

\* mqtt\_packet\_t: 수신한 MQTT 패킷의 종류와 내용을 저장하기 위한 타입

클라이언트가 보낸 MQTT 패킷 하나를 처리한다. 연결 요청(CONNECT), 구독(SUBSCRIBE), 발행(PUBLISH), 연결 유지(PINGREQ)에 응답한다.

### **b. broadcast\_to\_subscribers()** {#b.-broadcast_to_subscribers()}

int broadcast\_to\_subscribers(broker\_t \*broker, const char \*topic, const void \*message, size\_t len);

발행된 메시지를 해당 topic을 구독한 모든 클라이언트(사용자 기기)에 전달한다.
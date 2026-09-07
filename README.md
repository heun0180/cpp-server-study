# C++ Server Study

C++ 소켓 프로그래밍과 멀티스레드 서버 구조를 직접 구현하며 학습하기 위한 프로젝트입니다.

단순 예제 코드를 따라 작성하는 것보다 **TCP 연결부터 다중 클라이언트 처리, 동기화, 사용자 관리까지 기능을 단계적으로 추가하면서 서버가 어떻게 동작하는지 이해하는 것**을 목표로 진행하고 있습니다.

## 현재 구현 상태

현재는 **TCP 기반 1:N 실시간 채팅 서버와 닉네임 관리**까지 구현했습니다.

```text
Client A ─┐
Client B ─┼── TCP ── Server : 7777
Client C ─┘
```

서버는 접속한 클라이언트마다 별도의 수신 스레드를 생성하고, 한 클라이언트가 보낸 메시지를 다른 클라이언트들에게 전달합니다.

### 구현된 기능

- TCP 서버 / 클라이언트 소켓 생성
- `bind`, `listen`, `accept`, `connect`를 이용한 연결 처리
- 1:1 채팅
- 1:N 실시간 채팅
- 접속 클라이언트 목록 관리
- 클라이언트별 수신 스레드 처리
- 메시지 브로드캐스트
- 닉네임 등록
- 닉네임 중복 검사
- 사용자 접속 / 퇴장 알림
- 연결 종료 시 클라이언트 목록 제거
- `std::mutex`를 이용한 공유 데이터 동기화
- `std::lock_guard`를 이용한 RAII 방식 Lock 관리

## 학습한 내용

### TCP Socket

서버는 다음 순서로 클라이언트 연결을 처리합니다.

```text
socket()
   ↓
bind()
   ↓
listen()
   ↓
accept()
   ↓
recv() / send()
   ↓
close()
```

클라이언트는 다음 흐름으로 서버에 연결합니다.

```text
socket()
   ↓
connect()
   ↓
send() / recv()
   ↓
close()
```

### Multithreading

서버의 메인 스레드는 새로운 클라이언트의 접속을 계속 받으며, 접속한 클라이언트의 메시지 수신은 별도의 스레드에서 처리합니다.

```cpp
std::thread(
    receiveMessages,
    clientSocket,
    std::ref(clients),
    std::ref(clientsMutex)
).detach();
```

이를 통해 한 클라이언트의 입력을 기다리는 동안에도 다른 클라이언트의 메시지를 동시에 처리할 수 있도록 구성했습니다.

### 동기화

여러 클라이언트 스레드가 동시에 `clients` 목록을 조회하거나 수정할 수 있기 때문에 `std::mutex`를 사용했습니다.

```cpp
std::lock_guard<std::mutex> lock(clientsMutex);
```

직접 `lock()` / `unlock()`을 호출하는 대신 `std::lock_guard`를 사용하여 스코프를 벗어날 때 자동으로 Lock이 해제되도록 했습니다.

### Client 관리

클라이언트의 Socket과 닉네임을 함께 관리하기 위해 다음 구조체를 사용합니다.

```cpp
struct ClientInfo
{
    int socket;
    std::string nickname;
};
```

연결이 종료된 클라이언트는 `std::remove_if`와 `vector::erase`를 이용해 목록에서 제거합니다.

## 프로젝트 구조

```text
cpp-server-study/
├── README.md
└── src/
    ├── server.cpp
    └── client.cpp
```

- `server.cpp` : TCP 연결, 클라이언트 관리, 멀티스레드 및 메시지 전달
- `client.cpp` : 서버 연결, 닉네임 전송, 메시지 송수신

## 실행 환경

현재 코드는 POSIX Socket API를 사용하며 macOS 환경에서 학습하고 있습니다.

- C++17
- macOS
- clang++
- POSIX Socket API
- C++ Standard Library

## 빌드

프로젝트 루트에서 실행합니다.

### Server

```bash
clang++ -std=c++17 -pthread src/server.cpp -o server
```

### Client

```bash
clang++ -std=c++17 -pthread src/client.cpp -o client
```

## 실행

먼저 서버를 실행합니다.

```bash
./server
```

서버는 기본적으로 `7777` 포트에서 연결을 기다립니다.

```text
Server is listening on port 7777
```

다른 터미널에서 클라이언트를 실행합니다.

```bash
./client
```

여러 터미널에서 클라이언트를 실행하면 1:N 채팅을 확인할 수 있습니다.

```text
Client A ─┐
Client B ─┼─> Server ─> 다른 Client에게 메시지 전달
Client C ─┘
```

## 학습 진행 과정

- [x] TCP 서버 / 클라이언트 기본 연결
- [x] 1:1 채팅
- [x] 송신 / 수신을 분리한 실시간 채팅
- [x] 1:N 다중 클라이언트 처리
- [x] 메시지 브로드캐스트
- [x] Mutex를 이용한 동기화
- [x] 닉네임 / 사용자 관리
- [x] 닉네임 중복 방지
- [ ] 채팅방 구조
- [ ] Packet / Protocol 구조 설계
- [ ] Server / Client 클래스 구조화
- [ ] DB 연동
- [ ] 연결 및 예외 상황 안정성 개선
- [ ] 게임 서버 형태의 상태 동기화

## 앞으로 개선할 부분

현재 코드는 네트워크 서버의 기본 동작을 직접 이해하기 위한 단계이기 때문에 구조가 단순하게 작성되어 있습니다.

다음 단계에서는 다음 내용을 중심으로 개선할 예정입니다.

- 문자열 그대로 전송하는 방식에서 Packet 단위 통신으로 변경
- 메시지 경계 처리
- 채팅방 및 사용자 상태 관리
- Socket / Client / Server 책임을 클래스로 분리
- 스레드 생명주기 관리
- `send` / `recv` 실패 및 부분 전송 처리
- 비정상 연결 종료 대응
- DB를 이용한 사용자 데이터 관리
- 게임 서버에서 사용할 수 있는 상태 동기화 구조 학습

## 개발 목적

기존에는 C++ 기반 Windows 응용프로그램 개발을 주로 경험했습니다.

이 프로젝트에서는 C++ 경험을 서버 영역으로 확장하기 위해 Socket API, TCP/IP, 멀티스레딩, 동기화, 프로토콜 설계 등의 개념을 직접 구현하며 학습하고 있습니다.

완성된 결과물만 만드는 것보다 **기능을 하나씩 추가하면서 발생하는 문제를 직접 확인하고 서버 구조를 이해하는 과정**을 기록하는 것을 목표로 합니다.

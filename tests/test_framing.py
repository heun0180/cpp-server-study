import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor


ROOT = pathlib.Path(__file__).resolve().parents[1]
ADDRESS = ("127.0.0.1", 7777)
MAX_MESSAGE_SIZE = 4096
MAX_FRAME_SIZE = MAX_MESSAGE_SIZE + 99 + 3


def frame(message):
    if isinstance(message, str):
        message = message.encode("utf-8")
    return struct.pack("!I", len(message)) + message


def receive_all(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise AssertionError("프레임 수신 중 연결이 종료됨")
        data.extend(chunk)
    return bytes(data)


def receive_message(connection):
    length = struct.unpack("!I", receive_all(connection, 4))[0]
    assert 0 < length <= MAX_FRAME_SIZE, f"잘못된 수신 길이: {length}"
    return receive_all(connection, length)


def send_fragmented(connection, data):
    # 헤더와 UTF-8 문자 내부에서도 수신이 나뉘도록 바이트별로 보낸다.
    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for value in data:
        connection.sendall(bytes([value]))
        time.sleep(0.002)


def wait_until(predicate):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("응답 대기 시간 초과")


def expect_closed(connection):
    assert connection.recv(1) == b"", "잘못된 프레임 이후 연결이 유지됨"


def expect_no_message(connection):
    connection.settimeout(0.15)
    try:
        data = connection.recv(1)
    except socket.timeout:
        return
    finally:
        connection.settimeout(3)
    raise AssertionError(f"전달되면 안 되는 데이터: {data!r}")


def disconnect(connection):
    if connection.fileno() == -1:
        return
    try:
        connection.shutdown(socket.SHUT_WR)
        while connection.recv(4096):
            pass
    except OSError:
        pass
    finally:
        connection.close()


def test_client(build_dir):
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.settimeout(3)
        listener.bind(ADDRESS)
        listener.listen(1)

        invalid_data = [
            (struct.pack("!I", 0), False),
            (struct.pack("!I", MAX_FRAME_SIZE + 1), False),
            (struct.pack("!I", 0xFFFFFFFF), False),
            (b"\x00\x00", True),
            (struct.pack("!I", 10) + b"short", True),
        ]

        for index, (invalid, truncated) in enumerate([(None, False)] + invalid_data):
            log_path = build_dir / f"client-{index}.log"
            with log_path.open("wb") as output:
                client = subprocess.Popen(
                    [str(build_dir / "client")], stdin=subprocess.PIPE,
                    stdout=output, stderr=subprocess.STDOUT,
                )
                connection = None
                try:
                    connection, _ = listener.accept()
                    connection.settimeout(3)
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    client.stdin.write(b"native-client\n7\n")
                    client.stdin.flush()
                    assert receive_message(connection) == b"native-client"
                    assert receive_message(connection) == b"7"

                    if invalid is None:
                        send_fragmented(connection, frame("분할 수신"))
                        connection.sendall(frame("merged one") + frame("merged two"))
                        connection.sendall(frame(b"z" * MAX_FRAME_SIZE))
                        wait_until(lambda: b"z" * MAX_FRAME_SIZE in log_path.read_bytes())

                        client.stdin.write(b"\n" + b"x" * (MAX_MESSAGE_SIZE + 1) + b"\nhello\n")
                        client.stdin.flush()
                        assert receive_message(connection) == b"hello"
                        client.stdin.write(b"x" * MAX_MESSAGE_SIZE + b"\n")
                        client.stdin.flush()
                        assert receive_message(connection) == b"x" * MAX_MESSAGE_SIZE
                    else:
                        connection.sendall(invalid)
                        if truncated:
                            connection.close()
                            connection = None
                            wait_until(lambda: client.poll() is not None)
                        else:
                            expect_closed(connection)

                    client.stdin.close()
                    assert client.wait(timeout=3) == 0
                    if invalid is None:
                        text = log_path.read_bytes()
                        assert "Client: 분할 수신".encode() in text
                        assert b"Client: merged one\nClient: merged two\n" in text
                        assert b"Message too long" in text
                finally:
                    if connection is not None:
                        connection.close()
                    if client.poll() is None:
                        client.terminate()
                        client.wait(timeout=3)
                    if not client.stdin.closed:
                        client.stdin.close()

    print("PASS: 클라이언트 분할·병합 수신, 문자열 방 번호, 길이 제한, 잘린 프레임")


def test_connections(build_dir, register, alice, carol, server, server_log):
    names = [f"parallel-{index}" for index in range(4)]
    with ThreadPoolExecutor(max_workers=4) as executor:
        peers = list(executor.map(lambda name: register(name, "parallel-room"), names))
        for connection in peers:
            connection.settimeout(0.15)
            try:
                while True:
                    receive_message(connection)
            except socket.timeout:
                pass
            finally:
                connection.settimeout(3)

        list(executor.map(lambda peer: peer.sendall(frame("simultaneous")), peers))
        for index, connection in enumerate(peers):
            expected = {f"[{name}] simultaneous".encode() for name in names if name != names[index]}
            actual = {receive_message(connection) for _ in range(3)}
            assert actual == expected

    disconnect(peers[0])
    for connection in peers[1:]:
        assert receive_message(connection) == "parallel-0님이 퇴장했습니다.".encode()
    for connection in peers[1:]:
        disconnect(connection)
    expect_no_message(alice)
    expect_no_message(carol)
    print("PASS: 4개 클라이언트 동시 접속·동시 송신, 정상 종료 알림")

    reset = register("reset-client", "1")
    assert receive_message(alice) == "[reset-client]님이 접속했습니다.".encode()
    reset.close()
    assert receive_message(alice) == "reset-client님이 퇴장했습니다.".encode()
    assert server.poll() is None

    with (build_dir / "killed-client.log").open("wb") as output:
        client = subprocess.Popen(
            [str(build_dir / "client")], stdin=subprocess.PIPE,
            stdout=output, stderr=subprocess.STDOUT,
        )
        try:
            client.stdin.write(b"killed-client\n1\n")
            client.stdin.flush()
            wait_until(lambda: b"killed-client connected!" in server_log.read_bytes())
            assert receive_message(alice) == "[killed-client]님이 접속했습니다.".encode()
            client.stdin.write(b"before kill\n")
            client.stdin.flush()
            assert receive_message(alice) == b"[killed-client] before kill"
            client.kill()
            assert client.wait(timeout=3) < 0
            assert receive_message(alice) == "killed-client님이 퇴장했습니다.".encode()
        finally:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=3)
            client.stdin.close()

    replacement = register("killed-client", "1")
    assert receive_message(alice) == "[killed-client]님이 접속했습니다.".encode()
    replacement.sendall(frame("reconnected"))
    assert receive_message(alice) == b"[killed-client] reconnected"
    disconnect(replacement)
    assert receive_message(alice) == "killed-client님이 퇴장했습니다.".encode()

    for index in range(10):
        name = f"churn-{index}"
        connection = register(name, "churn-room")
        connection.close()
        wait_until(lambda: name.encode() + b" Client disconnected" in server_log.read_bytes())
    assert server.poll() is None
    expect_no_message(carol)
    print("PASS: TCP RST, 실제 클라이언트 강제 종료, 닉네임 재사용과 반복 연결 정리")

    log_path = build_dir / "server-end-client.log"
    with log_path.open("wb") as output:
        client = subprocess.Popen(
            [str(build_dir / "client")], stdin=subprocess.PIPE,
            stdout=output, stderr=subprocess.STDOUT,
        )
        try:
            # 개행 없는 입력과 열린 stdin을 남겨 입력 대기 중 종료를 확인한다.
            client.stdin.write(b"server-end-client\nserver-end-room\nunfinished")
            client.stdin.flush()
            wait_until(lambda: b"server-end-client connected!" in server_log.read_bytes())
            server.terminate()
            server.wait(timeout=3)
            assert client.wait(timeout=3) == 0
            assert b"Server disconnected:" in log_path.read_bytes()
        finally:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=3)
            client.stdin.close()
    print("PASS: 서버 종료 후 입력 없이 클라이언트 자동 종료·소켓 정리")


def test_registration(build_dir, connect, register, observer, server_log):
    pending = {}

    def pending_connection():
        started = time.monotonic()
        connection = connect()
        pending[connection] = started
        return connection

    idle = pending_connection()
    healthy = register("registration-healthy", "1")
    assert receive_message(observer) == "[registration-healthy]님이 접속했습니다.".encode()
    healthy.sendall(frame("while first client is idle"))
    assert receive_message(observer) == b"[registration-healthy] while first client is idle"
    disconnect(healthy)
    assert receive_message(observer) == "registration-healthy님이 퇴장했습니다.".encode()
    print("PASS: 등록 정보를 보내지 않는 연결과 별개로 후속 접속·등록·채팅 진행", flush=True)

    nickname_only = pending_connection()
    nickname_only.sendall(frame("nickname-only"))
    slow_nickname = pending_connection()
    nickname_data = frame("n" * 80)
    slow_nickname.sendall(nickname_data[:2])
    slow_room = pending_connection()
    room_data = frame("r" * 80)

    # 정상 채팅에는 등록 마감 시각이 계속 적용되지 않는지도 확인한다.
    late_start = time.monotonic()
    late = connect()
    next_tick = 1
    completed = {}
    while pending:
        now = time.monotonic()
        assert now - late_start < 18, "등록 제한 시간이 지나도 연결이 정리되지 않음"
        if now - late_start >= next_tick and next_tick <= 14:
            slow_nickname.sendall(nickname_data[next_tick + 1:next_tick + 2])
            if next_tick == 8:
                slow_room.sendall(frame("slow-room") + room_data[:1])
            elif next_tick > 8:
                index = next_tick - 8
                slow_room.sendall(room_data[index:index + 1])
            if next_tick == 12:
                late.sendall(frame("late-success") + frame("1") + frame("before deadline"))
                assert receive_message(observer) == "[late-success]님이 접속했습니다.".encode()
                assert receive_message(observer) == b"[late-success] before deadline"
            next_tick += 1

        ready, _, _ = select.select(list(pending), [], [], 0.05)
        for connection in ready:
            expect_closed(connection)
            elapsed = time.monotonic() - pending.pop(connection)
            assert 14.5 <= elapsed <= 17, f"등록 제한 시간 오류: {elapsed:.2f}초"
            completed[connection] = elapsed
            connection.close()

    assert idle in completed and nickname_only in completed
    assert slow_nickname in completed and slow_room in completed
    expect_no_message(observer)
    late.sendall(frame("after registration timeout window"))
    assert receive_message(observer) == b"[late-success] after registration timeout window"
    disconnect(late)
    assert receive_message(observer) == "late-success님이 퇴장했습니다.".encode()
    times = ", ".join(f"{value:.2f}초" for value in completed.values())
    print(f"PASS: 무입력·닉네임만 전송·분할 헤더/본문·필드 사이 지연의 전체 15초 제한 ({times})")

    reused = register("nickname-only", "1")
    assert receive_message(observer) == "[nickname-only]님이 접속했습니다.".encode()
    disconnect(reused)
    assert receive_message(observer) == "nickname-only님이 퇴장했습니다.".encode()

    log_path = build_dir / "registration-killed-client.log"
    with log_path.open("wb") as output:
        client = subprocess.Popen(
            [str(build_dir / "client")], stdin=subprocess.PIPE,
            stdout=output, stderr=subprocess.STDOUT,
        )
        try:
            client.stdin.write(b"registration-killed\n")
            client.stdin.flush()
            wait_until(lambda: b"Room ID:" in log_path.read_bytes())
            client.kill()
            assert client.wait(timeout=3) < 0
        finally:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=3)
            client.stdin.close()
    expect_no_message(observer)
    assert b"registration-killed connected!" not in server_log.read_bytes()
    print("PASS: 등록 중 실제 클라이언트 강제 종료 시 목록·입퇴장 알림 없음")

    contenders = [connect() for _ in range(8)]
    with ThreadPoolExecutor(max_workers=8) as executor:
        list(executor.map(lambda connection: connection.sendall(frame("same-nickname") + frame("1")), contenders))
    assert receive_message(observer) == "[same-nickname]님이 접속했습니다.".encode()
    winners = []
    for connection in contenders:
        connection.settimeout(1)
        try:
            message = receive_message(connection)
        except socket.timeout:
            winners.append(connection)
        else:
            assert message == "이미 사용 중인 닉네임입니다.".encode()
            expect_closed(connection)
            connection.close()
        finally:
            if connection.fileno() != -1:
                connection.settimeout(3)
    assert len(winners) == 1
    expect_no_message(observer)
    winners[0].sendall(frame("only one winner"))
    assert receive_message(observer) == b"[same-nickname] only one winner"
    disconnect(winners[0])
    assert receive_message(observer) == "same-nickname님이 퇴장했습니다.".encode()
    print("PASS: 같은 닉네임으로 8개 연결 동시 등록 시 한 개만 성공, 알림 중복 없음")


def test_server(build_dir):
    connections = []
    log_path = build_dir / "server.log"

    def connect():
        connection = socket.create_connection(ADDRESS, timeout=3)
        # 서버가 먼저 거부한 연결은 RST로 정리해 테스트 포트의 TIME_WAIT을 피한다.
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        connections.append(connection)
        return connection

    def register(nickname, room):
        connection = connect()
        connection.sendall(frame(nickname) + frame(room))
        wait_until(lambda: nickname.encode() + b" connected!" in log_path.read_bytes())
        return connection

    with log_path.open("wb") as output:
        server = subprocess.Popen(
            [str(build_dir / "server")], stdout=output, stderr=subprocess.STDOUT,
        )
        try:
            wait_until(lambda: b"Server is listening" in log_path.read_bytes() or server.poll() is not None)
            assert server.poll() is None, log_path.read_text()

            alice = register("alice", "1")
            test_registration(build_dir, connect, register, alice, log_path)
            bob = connect()
            bob.sendall(frame("bob") + frame("1") + frame("first") + frame("second"))
            assert receive_message(alice) == "[bob]님이 접속했습니다.".encode()
            assert receive_message(alice) == b"[bob] first"
            assert receive_message(alice) == b"[bob] second"
            expect_no_message(bob)
            disconnect(bob)
            assert receive_message(alice) == "bob님이 퇴장했습니다.".encode()
            print("PASS: 닉네임·방 번호·연속 채팅 프레임의 병합 수신, 입장·퇴장 알림")

            carol = register("carol", "2")
            split = connect()
            send_fragmented(split, frame("분할사용자") + frame("1") + frame("분할 메시지"))
            assert receive_message(alice) == "[분할사용자]님이 접속했습니다.".encode()
            assert receive_message(alice) == "[분할사용자] 분할 메시지".encode()
            expect_no_message(carol)
            disconnect(split)
            assert receive_message(alice) == "분할사용자님이 퇴장했습니다.".encode()
            expect_no_message(carol)
            print("PASS: 닉네임·방 번호·채팅 헤더와 본문의 분할 수신, 방 분리")

            duplicate = connect()
            duplicate.sendall(frame("alice") + frame("1"))
            assert receive_message(duplicate) == "이미 사용 중인 닉네임입니다.".encode()
            expect_closed(duplicate)
            duplicate.close()
            print("PASS: 닉네임 중복 거부 응답도 프레임으로 수신")

            for stage in ("nickname", "room", "chat"):
                limit = MAX_MESSAGE_SIZE if stage == "chat" else 99
                invalid_data = [
                    (struct.pack("!I", 0), False),
                    (struct.pack("!I", limit + 1), False),
                    (struct.pack("!I", 0xFFFFFFFF), False),
                    (b"\x00\x00", True),
                    (struct.pack("!I", 10) + b"short", True),
                ]
                for index, (invalid, truncated) in enumerate(invalid_data):
                    connection = connect()
                    prefix = b""
                    if stage != "nickname":
                        prefix += frame(f"invalid-{stage}-{index}")
                    if stage == "chat":
                        prefix += frame("invalid-room")
                    connection.sendall(prefix + invalid)
                    if truncated:
                        connection.shutdown(socket.SHUT_WR)
                    expect_closed(connection)
                    connection.close()
            assert server.poll() is None
            print("PASS: 각 등록 단계와 채팅의 길이 0·상한 초과·UINT32_MAX·잘린 프레임 거부")

            long_name = "n" * 99
            long_room = "r" * 99
            viewer = register("viewer", long_room)
            boundary = register(long_name, long_room)
            assert receive_message(viewer) == f"[{long_name}]님이 접속했습니다.".encode()
            boundary.sendall(frame(b"x" * MAX_MESSAGE_SIZE))
            assert receive_message(viewer) == f"[{long_name}] ".encode() + b"x" * MAX_MESSAGE_SIZE
            disconnect(boundary)
            assert receive_message(viewer) == f"{long_name}님이 퇴장했습니다.".encode()
            disconnect(viewer)
            print("PASS: 닉네임·방 번호·채팅 최대 길이와 최대 크기 서버 응답")

            healthy = register("healthy", "1")
            assert receive_message(alice) == "[healthy]님이 접속했습니다.".encode()
            healthy.sendall(frame("after invalid frames"))
            assert receive_message(alice) == b"[healthy] after invalid frames"
            expect_no_message(carol)
            print("PASS: 잘못된 프레임 처리 후에도 새 접속과 기존 채팅 유지")
            disconnect(healthy)
            assert receive_message(alice) == "healthy님이 퇴장했습니다.".encode()
            test_connections(build_dir, register, alice, carol, server, log_path)
        finally:
            for connection in connections:
                disconnect(connection)
            server.terminate()
            server.wait(timeout=3)


def main():
    with tempfile.TemporaryDirectory(prefix="cppserver-framing-") as temporary:
        build_dir = pathlib.Path(temporary)
        for target in ("server", "client"):
            subprocess.run([
                "clang++", "-std=c++17", "-pthread", "-Wall", "-Wextra",
                "-Wpedantic", "-Wconversion", str(ROOT / "src" / f"{target}.cpp"),
                "-o", str(build_dir / target),
            ], check=True)
        print("PASS: 서버·클라이언트 C++17 빌드")
        variants = [([], "protocol-test")]
        if sys.platform == "darwin":
            variants.append((["-DTEST_SO_NOSIGPIPE"], "protocol-test-nosigpipe"))
        for flags, target in variants:
            subprocess.run([
                "clang++", "-std=c++17", "-pthread", "-Wall", "-Wextra",
                "-Wpedantic", "-Wconversion", "-I", str(ROOT / "src"),
                *flags, str(ROOT / "tests" / "test_protocol.cpp"),
                "-o", str(build_dir / target),
            ], check=True)
            print(f"검증: {target}", flush=True)
            subprocess.run([str(build_dir / target)], check=True, timeout=15)
        test_client(build_dir)
        test_server(build_dir)


if __name__ == "__main__":
    main()

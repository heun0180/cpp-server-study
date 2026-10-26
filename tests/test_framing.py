import pathlib
import socket
import struct
import subprocess
import tempfile
import time


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
                            wait_until(lambda: b"Server disconnected" in log_path.read_bytes())
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
            duplicate.sendall(frame("alice"))
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
        test_client(build_dir)
        test_server(build_dir)


if __name__ == "__main__":
    main()

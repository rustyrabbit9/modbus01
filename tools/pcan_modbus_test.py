#!/usr/bin/env python3
"""Send Modbus RTU requests through a CAN-to-RS485 transparent converter."""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass

import can


if sys.platform == "win32":
    # Keep Chinese diagnostics readable in PowerShell, CLion and redirected logs.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")


SLAVE_ADDRESS = 0x01
READ_INPUT_REGISTERS = 0x04


@dataclass(frozen=True)
class Query:
    key: str
    label: str
    request_id: int
    response_id: int
    request: bytes
    scale: float
    unit: str
    signed: bool = False


QUERIES = (
    Query("temperature", "温度", 0x219, 0x331, bytes.fromhex("01 04 00 00 00 01 31 CA"), 0.1, "°C", True),
    Query("current", "电流", 0x220, 0x332, bytes.fromhex("01 04 00 01 00 01 60 0A"), 0.001, "A"),
    Query("voltage", "电压", 0x221, 0x333, bytes.fromhex("01 04 00 02 00 01 90 0A"), 0.01, "V"),
)


def parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"无效整数或十六进制数: {value}") from exc


def modbus_crc(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def hex_bytes(data: bytes | bytearray) -> str:
    return " ".join(f"{byte:02X}" for byte in data)


def print_bus_status(bus: can.BusABC, prefix: str = "PCAN 状态") -> bool | None:
    status_method = getattr(bus, "status", None)
    if not callable(status_method):
        return None

    status = status_method()
    status_value = int(status)
    status_string_method = getattr(bus, "status_string", None)
    description = status_string_method() if callable(status_string_method) else None
    print(f"[{prefix}] 0x{status_value:08X} {description or ''}".rstrip())
    return status_value == 0


def decode_response(data: bytes, query: Query) -> tuple[float | None, str | None]:
    if len(data) >= 5 and data[0] == SLAVE_ADDRESS and data[1] == (READ_INPUT_REGISTERS | 0x80):
        frame = data[:5]
        expected_crc = frame[-2] | (frame[-1] << 8)
        if modbus_crc(frame[:-2]) == expected_crc:
            return None, f"Modbus 异常响应，异常码 0x{frame[2]:02X}"

    if len(data) < 7:
        return None, None

    frame = data[:7]
    if frame[0:3] != bytes((SLAVE_ADDRESS, READ_INPUT_REGISTERS, 0x02)):
        return None, None

    expected_crc = frame[5] | (frame[6] << 8)
    if modbus_crc(frame[:5]) != expected_crc:
        return None, "收到疑似 Modbus 响应，但 CRC 校验失败"

    raw = int.from_bytes(frame[3:5], byteorder="big", signed=query.signed)
    return raw * query.scale, ""


def wait_for_response(
    bus: can.BusABC,
    query: Query,
    timeout: float,
    response_id: int | None,
) -> bool:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        message = bus.recv(timeout=min(0.1, deadline - time.monotonic()))
        if message is None:
            continue

        data = bytes(message.data)
        frame_type = "EXT" if message.is_extended_id else "STD"
        print(
            f"  [RX] {frame_type} ID=0x{message.arbitration_id:X} "
            f"DLC={message.dlc} DATA={hex_bytes(data)}"
        )

        if response_id is not None and message.arbitration_id != response_id:
            continue

        value, error = decode_response(data, query)
        if error:
            print(f"  [错误] {error}")
            return False
        if value is not None:
            print(f"  [成功] {query.label} = {value:g} {query.unit}")
            return True

    print(f"  [超时] {timeout:g} 秒内没有收到有效的{query.label}响应")
    return False


def selected_queries(name: str) -> tuple[Query, ...]:
    if name == "all":
        return QUERIES
    return tuple(query for query in QUERIES if query.key == name)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="通过 PEAK PCAN 发送三组 Modbus 请求并监听转换器回传"
    )
    parser.add_argument("--channel", default="PCAN_USBBUS1", help="PCAN 通道")
    parser.add_argument("--bitrate", type=int, default=500_000, help="CAN 波特率")
    parser.add_argument(
        "--tx-id",
        type=parse_int,
        help="覆盖三项默认发送 ID 0x219/0x220/0x221",
    )
    parser.add_argument(
        "--rx-id",
        type=parse_int,
        help="覆盖三项默认响应 ID 0x331/0x332/0x333",
    )
    parser.add_argument("--timeout", type=float, default=2.0, help="每次请求的等待秒数")
    parser.add_argument("--interval", type=float, default=0.2, help="三次请求之间的间隔秒数")
    parser.add_argument("--extended", action="store_true", help="使用 29 位扩展 CAN ID")
    parser.add_argument(
        "--query",
        choices=("all", "temperature", "current", "voltage"),
        default="all",
        help="选择查询项目",
    )
    return parser


def main() -> int:
    args = make_parser().parse_args()
    queries = selected_queries(args.query)

    print(
        f"打开 PEAK PCAN: channel={args.channel}, bitrate={args.bitrate}"
    )
    print("提示：请先关闭 PCAN-View，并确保转换器不是 Listen Only 模式。")

    try:
        bus = can.Bus(
            interface="pcan",
            channel=args.channel,
            bitrate=args.bitrate,
            receive_own_messages=False,
        )
    except Exception as exc:
        print(f"[失败] 无法打开 PCAN 通道: {exc}", file=sys.stderr)
        return 2

    successes = 0
    try:
        print_bus_status(bus, "打开后状态")
        for index, query in enumerate(queries):
            request_id = args.tx_id if args.tx_id is not None else query.request_id
            response_id = args.rx_id if args.rx_id is not None else query.response_id
            message = can.Message(
                arbitration_id=request_id,
                is_extended_id=args.extended,
                data=query.request,
            )
            print(
                f"\n[TX] 查询{query.label}: ID=0x{request_id:X} "
                f"DLC=8 DATA={hex_bytes(query.request)}，等待 ID=0x{response_id:X}"
            )
            try:
                bus.send(message, timeout=1.0)
            except can.CanError as exc:
                print(f"  [发送失败] {exc}")
                continue

            if wait_for_response(bus, query, args.timeout, response_id):
                successes += 1
            print_bus_status(bus, "请求后状态")

            if index + 1 < len(queries):
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n用户停止测试")
    finally:
        bus.shutdown()

    print(f"\n测试结果: {successes}/{len(queries)} 项收到有效响应")
    if successes == 0:
        print("若仍为 0，请先处理 CAN Error Passive、终端电阻、波特率及转换器映射设置。")
    return 0 if successes == len(queries) else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Verifica anúncio IdeiaRec por UUID, sem conectar/parear nem listar terceiros."""
import argparse
import asyncio
from bleak import BleakScanner

SERVICE = "7fd00001-6f6e-4d69-9b18-8e8c49b5c100"


async def scan(timeout):
    device = await BleakScanner.find_device_by_filter(
        lambda _device, advertisement: SERVICE in [s.lower() for s in advertisement.service_uuids],
        timeout=timeout, service_uuids=[SERVICE])
    if not device:
        print("Serviço IdeiaRec não encontrado; abra Parear celular no ESP32.")
        return 1
    print(f"Serviço IdeiaRec encontrado: {device.name or '(sem nome)'} | {device.address}")
    print(f"UUID: {SERVICE}. Somente anúncio; pareamento/TLS não foram testados.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    return asyncio.run(scan(args.timeout))


if __name__ == "__main__":
    raise SystemExit(main())

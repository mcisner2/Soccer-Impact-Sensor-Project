"""Tiny terminal dashboard so you can see the whole pipeline working.

  python main.py --mock                                  # fake sensor, no hardware needed
  python main.py --mock --start-session --raw            # full flow: START_CAL, wait for READY, START_SESSION, RAW_STREAM_ON
  python main.py --mock --drop-every 3                   # fake sensor that loses packets
  python main.py --real --start-session                  # real Group3Sensor over Bluetooth

The swap from fake to real is the single line that picks the manager below.
"""
import argparse
import asyncio

from ble_central import BleakBLEManager, ConnectionState, DashboardViewModel, MockBLEManager


async def main() -> None:
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mock", action="store_true", help="use the fake sensor")
    mode.add_argument("--real", action="store_true", help="use the real ESP32")
    ap.add_argument("--seconds", type=float, default=10.0, help="how long to run")
    ap.add_argument("--drop-every", type=int, default=0, help="(mock) lose every Nth packet")
    ap.add_argument("--start-session", action="store_true",
                    help="once connected: START_CAL, wait for READY, then START_SESSION (stand still for the cal!)")
    ap.add_argument("--raw", action="store_true", help="send RAW_STREAM_ON once connected")
    args = ap.parse_args()

    ble = (MockBLEManager(drop_every=args.drop_every, auto_start=not args.start_session)
           if args.mock else BleakBLEManager())
    vm = DashboardViewModel(ble)

    def render() -> None:
        pct = f"{vm.battery_percentage}%" if vm.battery_percentage is not None else "--"
        mv = f"{vm.battery_mv}mV" if vm.battery_mv is not None else "--"
        temp = f"{vm.temperature_c:.1f}C" if vm.temperature_c is not None else "--"
        ev = vm.recent_events[-1] if vm.recent_events else None
        ev_txt = f"{ev.event_class_name.lower()} {ev.peak_accel_g:.2f}g" if ev else "none yet"
        tel = vm.latest_telemetry
        tel_txt = (f"{tel.speed_m_s:.1f}m/s {tel.satellites}sats" if tel and tel.has_fix
                   else "no GPS fix")
        print(
            f"[{vm.status_message:<24}] dev={vm.device_state_name:<8} batt={pct:<4}({mv}) "
            f"temp={temp:<7} raw={'on' if vm.raw_stream_on else 'off'} "
            f"event={ev_txt:<20} gps={tel_txt:<16} missed={vm.missed_packet_count} bad={vm.parse_error_count}"
            + (f"  !! {vm.error_message}" if vm.error_message else "")
        )

    vm.subscribe(render)
    await vm.connect_to_sensor()

    if args.start_session or args.raw:
        while vm.connection_state != ConnectionState.SUBSCRIBED:
            await asyncio.sleep(0.1)
        if args.start_session:
            await vm.start_calibration()
            for _ in range(200):  # wait up to 20 s for calibration to finish (READY)
                if vm.device_state_name == "READY":
                    break
                await asyncio.sleep(0.1)
            await vm.start_session()
        if args.raw:
            await vm.set_raw_stream(True)

    await asyncio.sleep(args.seconds)
    await vm.disconnect_from_sensor()


if __name__ == "__main__":
    asyncio.run(main())

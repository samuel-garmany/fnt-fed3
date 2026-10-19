"""Run fnt's device tests against the firmware core instead of the simulator.

    make -C test test                # or: FNT=/path/to/fnt python3 test/run_sim_tests.py

fnt/tests/fed3_device_sim.py is the behaviour the firmware promises; here the
same checks drive the C++ sketch through test/host_hal.cpp.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FNT = os.environ.get("FNT", os.path.join(HERE, "..", "..", "fnt"))


def main():
    sys.path.insert(0, FNT)
    sys.path.insert(0, HERE)
    from fed3_core import CoreDevice

    spec = importlib.util.spec_from_file_location(
        "fed3_device_sim", os.path.join(FNT, "tests", "fed3_device_sim.py"))
    tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tests)
    print(f"running {spec.origin} against the firmware core")
    failed = tests.main(make=CoreDevice)

    # FNT's run engine, end to end, with the firmware core as every device.
    import types
    from fnt.fed3 import device_sim
    spec = importlib.util.spec_from_file_location(
        "fed3_run", os.path.join(FNT, "tests", "fed3_run.py"))
    run_tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(run_tests)
    run_tests.dev = types.SimpleNamespace(SimDevice=CoreDevice,
                                          SimTransport=device_sim.SimTransport)
    print(f"\nrunning {spec.origin} with the firmware core as the devices")
    return failed or run_tests.main()


if __name__ == "__main__":
    sys.exit(main())

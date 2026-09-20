import time
import ota


def wait_status(sock, predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while True:
        status = ota.request(sock, 2)
        if predicate(status):
            return status
        assert time.monotonic() < deadline, status
        time.sleep(.02)

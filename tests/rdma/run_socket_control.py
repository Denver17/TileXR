"""Launch the CPU-only rank pair; neither executable links an NPU runtime."""
import os
import socket
import subprocess
import sys

with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
env = dict(os.environ, TILEXR_COMM_ID=f"127.0.0.1:{port}")
processes = []
try:
    for rank in range(2):
        processes.append(subprocess.Popen([sys.argv[1], str(rank)], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    for rank, process in enumerate(processes):
        output, _ = process.communicate(timeout=20)
        print(output, end="")
        if process.returncode or f"CPU_CONTROL_PASS rank={rank}" not in output:
            raise SystemExit(1)
finally:
    for process in processes:
        if process.poll() is None:
            process.kill()
        process.wait()

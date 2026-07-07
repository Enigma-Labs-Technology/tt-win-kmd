#!/usr/bin/env python3
"""Host-side guest automation for the ttkmd test VM (password SSH via paramiko).

Usage:
  guest_cmd.py run  "powershell -Command Get-Date"     # run command, stream output
  guest_cmd.py push <local-dir> C:/tt/drop             # recursive upload
  guest_cmd.py wait-provisioned [timeout_s]            # poll for C:\\tt\\provisioned.txt

Lab credentials only; VM is bound to 127.0.0.1.
"""
import os
import stat
import sys
import time

import paramiko

HOST, PORT, USER, PASSWORD = "127.0.0.1", 2222, "ttdev", "ttdev!Lab1"


def connect(timeout=20):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, port=PORT, username=USER, password=PASSWORD,
              timeout=timeout, banner_timeout=timeout, auth_timeout=timeout,
              look_for_keys=False, allow_agent=False)
    return c


def run(cmd: str) -> int:
    c = connect()
    try:
        _, out, err = c.exec_command(cmd, timeout=1800)
        for line in out:
            sys.stdout.write(line)
        e = err.read().decode(errors="replace")
        if e:
            sys.stderr.write(e)
        return out.channel.recv_exit_status()
    finally:
        c.close()


def push(local_dir: str, remote_dir: str) -> int:
    c = connect()
    try:
        sftp = c.open_sftp()
        # Windows OpenSSH SFTP accepts forward slashes rooted at the drive.
        def ensure(p):
            try:
                sftp.stat(p)
            except FileNotFoundError:
                parent = p.rsplit("/", 1)[0]
                if parent and parent != p:
                    ensure(parent)
                sftp.mkdir(p)
        ensure(remote_dir.rstrip("/"))
        count = 0
        for root, _, files in os.walk(local_dir):
            rel = os.path.relpath(root, local_dir)
            rdir = remote_dir.rstrip("/") + ("" if rel == "." else "/" + rel.replace(os.sep, "/"))
            ensure(rdir)
            for f in files:
                lp = os.path.join(root, f)
                rp = rdir + "/" + f
                sftp.put(lp, rp)
                count += 1
        print(f"pushed {count} files to {remote_dir}")
        return 0
    finally:
        c.close()


def wait_provisioned(timeout_s: int = 5400) -> int:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            c = connect(timeout=8)
            try:
                _, out, _ = c.exec_command("type C:\\tt\\provisioned.txt")
                data = out.read().decode(errors="replace").strip()
                if "provisioned" in data:
                    print(data)
                    return 0
            finally:
                c.close()
        except Exception:
            pass
        time.sleep(30)
    print("timeout waiting for provisioning", file=sys.stderr)
    return 2


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    mode = sys.argv[1]
    if mode == "run":
        return run(sys.argv[2])
    if mode == "push":
        return push(sys.argv[2], sys.argv[3])
    if mode == "wait-provisioned":
        return wait_provisioned(int(sys.argv[2]) if len(sys.argv) > 2 else 5400)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())

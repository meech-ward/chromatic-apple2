#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual ESP32 BSD adapter/SSH engine against OpenSSH; test-only localhost keys.

Pass --mbedtls /path/to/esp-idf/components/mbedtls/mbedtls. No downloads/installs.
"""
import argparse
import os
from pathlib import Path
import subprocess
import time
from host_helpers import Client, CorruptEncryptedPacket, check_wire_rejection

SSH = Path(__file__).resolve().parents[1]
TEST = SSH / "tests"
BUILD = SSH.parents[2] / "build/tools/ssh-test"
PORT = 22228

def run(command, **kwargs):
    return subprocess.run([str(x) for x in command],check=True,**kwargs)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mbedtls",type=Path,required=True)
    parser.add_argument("--no-sanitize",action="store_true",help="run without compiler sanitizers")
    args=parser.parse_args();mbed=args.mbedtls.resolve();BUILD.mkdir(parents=True,exist_ok=True)
    flags=["-O1","-g",* ([] if args.no_sanitize else ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]),
           "-I"+str(mbed/"include"),"-I"+str(TEST),"-I"+str(SSH),
           '-DMBEDTLS_CONFIG_FILE="ssh_mbedtls_config.h"']
    objects=[]
    for name in ("aes","cipher","cipher_wrap","constant_time","gcm","platform_util","platform","sha256","block_cipher"):
        output=BUILD/(name+".o")
        run(["clang",*flags,"-c",mbed/"library"/(name+".c"),"-o",output]);objects.append(output)
    adapter=BUILD/"esp32-socket.o"
    run(["clang","-std=c11",*flags,"-DSSH_LISTEN_PORT="+str(PORT),"-DSSH_LISTEN_ADDRESS=INADDR_LOOPBACK","-I"+str(TEST/"stubs"),
         "-c",SSH/"ssh_platform_esp32.c","-o",adapter])
    vendor=[SSH/"vendor/staticnet"/p for p in ("crypt/CryptoEngine.cpp","contrib/tweetnacl_25519.cpp","contrib/base64.cpp")]
    run(["clang++","-std=c++17",*flags,SSH/"ssh_transport.cpp",TEST/"ssh_socket_host.cpp",*vendor,*objects,adapter,"-o",BUILD/"ssh-host"])
    run(["clang++","-std=c++17",*flags,TEST/"ssh_transport_fuzz.cpp",*vendor,*objects,"-o",BUILD/"ssh-fuzz"])
    env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0",UBSAN_OPTIONS="halt_on_error=1")
    run([BUILD/"ssh-fuzz"],env=env,timeout=60)
    askpass=BUILD/"askpass";askpass.write_text('#!/bin/sh\nprintf %s "$APPLE2_TEST_PASSWORD"\n');askpass.chmod(0o700)
    errors=BUILD/"server-errors.log"
    with errors.open("w+") as log:
        server=subprocess.Popen([str(BUILD/"ssh-host")],env=env,stdout=subprocess.PIPE,stderr=log)
        try:
            ready=server.stdout.readline().decode().strip();assert ready.startswith("READY "),ready
            check_wire_rejection(PORT)
            # The client permits ONLY password auth; no user SSH config/agent.
            options=dict(PubkeyAuthentication="no",IdentityAgent="none",NumberOfPasswordPrompts="1")
            bad=Client(PORT,askpass,password="definitely-wrong",**options)
            assert bad.process.wait(timeout=8)!=0;bad.process.communicate(timeout=2)
            time.sleep(.05)
            client=Client(PORT,askpass,**options)
            try:
                client.until(b"APPLE II ESP32 SSH TEST")
                payload=(b"CHROMATIC-SSH-TEST-"+bytes(range(32,127)))*100
                client.send(payload);client.until(payload,timeout=15)
            finally:client.close()
            time.sleep(.05)
            proxy=CorruptEncryptedPacket(PORT)
            damaged=Client(proxy.port,askpass,**options)
            assert damaged.process.wait(timeout=8)!=0
            damaged.process.communicate(timeout=2);proxy.close();assert proxy.corrupted
            # A fresh session still works after malformed encrypted traffic.
            time.sleep(.05);again=Client(PORT,askpass,**options)
            try:again.until(b"APPLE II ESP32 SSH TEST")
            finally:again.close()
        finally:
            server.terminate();server.communicate(timeout=5)
        log.flush();log.seek(0);diagnostics=log.read()
        assert not diagnostics,diagnostics
    print("PASS actual BSD socket adapter + OpenSSH: encrypted login, wrong password,11KiB flow control, tag corruption, reconnect")

if __name__=="__main__":main()

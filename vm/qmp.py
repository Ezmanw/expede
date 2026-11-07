#!/usr/bin/env python3
"""Drive the headless test VM over QMP: screenshots and pointer/keyboard input.

    qmp.py shot out.png
    qmp.py move X Y          (screen pixels, 1024x768)
    qmp.py click X Y [dbl]
    qmp.py key meta_l | alt-f4 | ...
"""
import json
import os
import socket
import sys
import time

W, H = 1024, 768
SOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'qmp.sock')


class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(SOCK)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                if 'error' in r:
                    raise RuntimeError(r['error'])
                return r['return']

    def move(self, x, y):
        self.cmd('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (W - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (H - 1))}}])

    def button(self, down):
        self.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])


def main():
    q = QMP()
    op = sys.argv[1]
    if op == 'shot':
        q.cmd('screendump', filename=os.path.abspath(sys.argv[2]), format='png')
    elif op == 'move':
        q.move(float(sys.argv[2]), float(sys.argv[3]))
    elif op == 'click':
        q.move(float(sys.argv[2]), float(sys.argv[3]))
        time.sleep(0.15)
        for _ in range(2 if len(sys.argv) > 4 else 1):
            q.button(True)
            time.sleep(0.05)
            q.button(False)
            time.sleep(0.08)
    elif op == 'drag':
        x0, y0, x1, y1 = map(float, sys.argv[2:6])
        q.move(x0, y0)
        time.sleep(0.15)
        q.button(True)
        for i in range(1, 11):
            time.sleep(0.03)
            q.move(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10)
        time.sleep(0.1)
        q.button(False)
    elif op == 'key':
        keys = [{'type': 'qcode', 'data': k} for k in sys.argv[2].split('-')]
        q.cmd('send-key', keys=keys)


if __name__ == '__main__':
    main()

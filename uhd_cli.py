#!/usr/bin/env python

#Usage: ./uhd_cli.py "cmd"
# >     ./uhd_cli.py "set rxfreq 2450e6"
# >     ./uhd_cli.py "get rxfreq"
#

import sys
import socket
import struct
from time import time

#TCP_IP = sys.argv[1]
#TCP_PORT = int(sys.argv[2])
TCP_IP = "node1-1.sb7.orbit-lab.org"
TCP_PORT = int(5123)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((TCP_IP, TCP_PORT))

COMMAND =  sys.argv[1]
s.send(COMMAND)
data = s.recv(256)

print data

s.close()

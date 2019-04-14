#!/usr/bin/python

import sys
import os
import subprocess

inp = []
mainpath = "/sys/fs/cgroup/cpu/"

for line in sys.stdin:
	inp = line[:-1].split(":")
	if line.startswith('create'):
		#path = mainpath + inp[1]
		#os.system("mkdir -p " + path)
		path = mainpath + inp[1] + '/' + inp[3]
		os.system("mkdir -p " + path)
	elif line.startswith('remove'):
		path = mainpath + inp[1] + '/' + inp[3]
		os.system("rmdir " + path)
	elif line.startswith('add'):
		path = mainpath + inp[1] + '/' + inp[3] + '/tasks'
		os.system("echo " + inp[4] + ' >> ' + path)
	elif line.startswith('set_limit'):
		path = mainpath + inp[1] + '/' + inp[3] + '/cpu.shares'
		os.system("echo " + inp[5] + ' > ' + path)

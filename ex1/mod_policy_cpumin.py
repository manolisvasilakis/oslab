#!/usr/bin/python

import sys
import os

temp = []
total_list = []
total = 0.0
virtual = 2000.0
elastic = 0

for line in sys.stdin:
	temp = line.split(":")
	temp[3] = int(temp[3])
	total += temp[3]
	if temp[3] <= 50:
		elastic += 1
	total_list.append(temp)

if total > virtual:
	os.system("echo score:-0.1")
else:
	os.system("echo score:0.1")

for app in total_list:
	if total <= virtual:
		if elastic == 0:
			os.system("echo set_limit:"+app[1]+":cpu.shares:"+str(app[3]))
		else:
			if app[3] <= 50:
				app[3] = app[3] + int((virtual - total) / elastic)
			os.system("echo set_limit:"+app[1]+":cpu.shares:"+str(app[3]))
	else:
		os.system("echo set_limit:"+app[1]+":cpu.shares:"+str(app[3]))

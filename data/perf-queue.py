#!/usr/bin/python3

import os
import sys
import re
import csv
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import ctypes as ct

def name2col(name):
	if name == 1:
		return 0
	if name == 2:
		return 1
	if name == 4:
		return 2
	if name == 6:
		return 3
	if name == 8:
		return 4
	if name == 12:
		return 5
	if name == 16:
		return 6
	if name == 24:
		return 7
	if name == 32:
		return 8
	if name == 48:
		return 9
	if name == 64:
		return 10
	if name == 80:
		return 11
	if name == 96:
		return 12
	return 13

def col2name(col):
	colnames = ['1', '2', '4', '6', '8', '12', '16', '24', '32', '48', '64', '80', '96']
	if col < 0 or col > 12:
		return 'COL:N/A'
	return colnames[col]

# [1294614.893791] fl  : threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005563711  hits:       4294672 missed: 0
# [1294626.157542] fl  : threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005781823  hits:       4284752 missed: 0
# [1294637.421682] fl  : threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10006509348  hits:       4244930 missed: 0
# [1294648.685802] fl  : threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10006549908  hits:       4047967 missed: 0
# [1294659.949530] fl  : threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005943977  hits:       4044674 missed: 0
# [1294671.213546] saca: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005932851  hits:     369641350 missed: 0
# [1294682.477826] saca: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005648037  hits:     372153087 missed: 0
# [1294693.741680] saca: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005701541  hits:     384725866 missed: 0
# [1294705.005661] saca: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005951192  hits:     384397828 missed: 0
# [1294716.269558] saca: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005954771  hits:     385393793 missed: 0
# [1294727.533822] sapc: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005838602  hits:     649406271 missed: 0
# [1294738.797628] sapc: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10005886075  hits:     636957223 missed: 0
# [1294750.061675] sapc: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10006449335  hits:     635126738 missed: 0
# [1294761.325841] sapc: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10006653030  hits:     636755983 missed: 0
# [1294772.589815] sapc: threads:96 preempt:0 max: 384 cycle: 0 a:0 b:1 delta: 10006576278  hits:     645420133 missed: 0

def find_substr(content, start, str):
	rc = -1
	# locating start
	while start < len(content):
		line = content[start]
		find = re.search(str, line)
		if find:
			rc = start
			break
		start = start + 1
	return rc

def find_insmod(content, s):
	return find_substr(content, s, "insmod")

def find_result(content, s):
	return find_substr(content, s, "threads:")

def process_rec(content, s, end):
	pat = r'.*\[\d+\.\d+\]\s+(.+)\s*:\sthreads:\s*(\d+)\spreempt:\s*(\d+)\smax:\s*(\d+)\scycle:\s*(\d+)\sa:(\d+)\sb:(\d+)\sdelta:\s*(\d+)\s+hits:\s*(\d+)\smissed:\s*(\d+).*'
	ts = []
	for i in range(0, 5):
		s = find_result(content, s)
		if s < 0 or s > end:
			break
		t = re.match(pat, content[s], re.M|re.I)
		if not t:
			break
		# print(t.group(1), t.group(2), t.group(3), t.group(4), t.group(5), t.group(6), t.group(7), t.group(8), t.group(9), t.group(10))
		vs = 10000000000.0 * int(t.group(9)) / int(t.group(8))
		vm = 10000000000.0 * int(t.group(10)) / int(t.group(8))
		ts.append([t.group(1), t.group(2), t.group(3), t.group(4), t.group(5), t.group(6), t.group(7), int(vs), int(vm)])
		s = s + 1

	for i in range(0, 5):
		b = 0
		for j in range(0, 5):
			if (ts[i][7] > ts[j][7]):
				b = b + 1
		if (b == 3):
			return ts[i][0], int(ts[i][1]), ts[i][2], ts[i][3], ts[i][4], ts[i][5], ts[i][6], ts[i][7], ts[i][8]

	return 0, 1, 2, 3, 4, 5, 6, 7, 0, 0

def row_matrix(mat, tag):
	colnames = ['1', '2', '4', '6', '8', '12', '16', '24', '32', '48', '64', '80', '96']
	for i in range(0, len(mat[:])):
		if mat[i][0] == tag:
			return i, mat
	cols = []
	cols.append(tag)
	for col in colnames:
		cols.append(0)
	mat.append(cols)
	return len(mat[:]) - 1, mat

def process_sec(content, start, data, miss):
	s = find_insmod(content, start)
	if s <= 0:
		return -1, data, miss

	e = find_insmod(content, s + 1)
	if e == -1:
		e = len(content) - 1
	if e > s + 5:
		print(s, " - ", e)
		n, t, p, m, c, a, b, p, x = process_rec(content, s, e)
		print(n, t, p, m, c, a, b, p, x)
		tag = n.strip() + ':' + m + '/' + a + '-' + b
		row, data = row_matrix(data, tag)
		data[row][int(name2col(t)) + 1] = int(p)
		print(row, int(name2col(t)) + 1, tag, p)
		row, miss = row_matrix(miss, tag)
		miss[row][int(name2col(t)) + 1] = int(x)
		print(row, int(name2col(t)) + 1, tag, x)
	return e, data, miss

def process_log(set, title, wcsvd, wcsvm):

	content = []
	matdata = []
	matmiss = []
	with open('perf.' + set + '.log', 'r') as fp:
		print('perf.' + set + '.log')
		for txt in fp.readlines():
			content.append(txt.rstrip('\n'))

		start = 0
		while start >= 0 and start < len(content):
			start, matdata, matmiss = process_sec(content, start, matdata, matmiss)
	fp.close()

	for row in range(len(matdata[:])):
		data = []
		for col in range(len(title)):
			data.append(matdata[row][col])
		wcsvd.writerow(data)

	for row in range(len(matmiss[:])):
		data = []
		for col in range(len(title)):
			data.append(matmiss[row][col])
		wcsvm.writerow(data)

def main():
	rc = -1
	sets = ['arm64']

	title = []
	title.append(' ')
	colnames = ['1', '2', '4', '6', '8', '12', '16', '24', '32', '48', '64', '80', '96']
	for col in colnames:
		title.append(col)

	for set in sets:
		csvd = 'perf.data.' + set + '.csv'
		csvm = 'perf.miss.' + set + '.csv'
		with open(csvd, 'w') as fcsvd:
			with open(csvm, 'w') as fcsvm:
				wcsvd = csv.writer(fcsvd)
				wcsvd.writerow(title)
				wcsvm = csv.writer(fcsvm)
				wcsvm.writerow(title)
				process_log(set, title, wcsvd, wcsvm)
				rc = 0
				fcsvm.close()
			fcsvd.close()
	return rc

# main begin
if __name__=='__main__':
	main()

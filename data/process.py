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

def name2row(name):
	if name == 96:
		return 0
	if name == 128:
		return 1
	if name == 192:
		return 2
	if name == 256:
		return 3
	if name == 384:
		return 4
	return 5

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
	if name == 96:
		return 11
	if name == 128:
		return 12
	if name == 192:
		return 13
	return 14

def row2name(row):
	rownames = ['96', '128', '192', '256', '384']
	if row < 0 or row > 4:
		return 'ROW:N/A'
	return rownames[row]

def col2name(col):
	colnames = ['1', '2', '4', '6', '8', '12', '16', '24', '32', '48', '64', '96', '128', '192']
	if col < 0 or col > 13:
		return 'COL:N/A'
	return colnames[col]

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

def find_perf(content, s):
	return find_substr(content, s, "syscalls:sys_enter_flock")

def find_sectag(content, s):
	return find_substr(content, s, "krp_insts=")

def find_missed(content, s):
	return find_substr(content, s, "sys_flock: missed: ")

# value = int(matched.group('value'))
#    20.014119528          1,249,626      syscalls:sys_enter_flock     
def process_rec(content, insts, ts1, v1, v2, v3, mi):
	s1 = re.match(r'\s+(\d+\.\d+)\s+(\d+[\d,]*)\s+.*', content[v1], re.M|re.I)
	s2 = re.match(r'\s+(\d+\.\d+)\s+(\d+[\d,]*)\s+.*', content[v2], re.M|re.I)
	s3 = re.match(r'\s+(\d+\.\d+)\s+(\d+[\d,]*)\s+.*', content[v3], re.M|re.I)
	m  = re.match(r'.+_sys_flock: missed:\s+(\d+).*',  content[mi], re.M|re.I)
	if not s1 or not s2 or not s3 or not m:
		print (s1, s2, s3, m)
		return 0, 0
	f1 = float(s1.group(2).replace(',', '')) * 10.0 / float(s1.group(1))
	f2 = float(s2.group(2).replace(',', '')) * 10.0 / (float(s2.group(1)) - float(s1.group(1)))
	f3 = float(s3.group(2).replace(',', '')) * 10.0 / (float(s3.group(1)) - float(s2.group(1)))

	if f1 > f2 and f1 < f3:
		rc = f1
	elif f2 > f1 and f2 < f3:
		rc = f2
	else:
		rc = f3
	print("process_rec: ", rc,  " from", f1, f2, f3)
	return int(rc), int(m.group(1))

def process_sec(content, start, perfdata, missdata):
	insts = re.match(r'krp_insts=(\d+).*', content[start], re.M|re.I)
	if insts:
		while find_substr(content, start, "flock") < start + 2:
			start = start + 1
		ts1 = content[start]
		ts2 = content[start + 1]
		if ts1 and ts2 and ts1 == ts2:
			v1 = find_perf(content, start + 2)
			v2 = find_perf(content, v1 + 1)
			v3 = find_perf(content, v2 + 1)
			mi = find_missed(content, v3 + 1)
			if v1 > 0 and v2 > 0 and v3 > 0 and mi > 0:
				v, m = process_rec(content, insts.group(1), ts1, v1, v2, v3, mi)
				row = name2row(int(insts.group(1)))
				col = name2col(int(ts1))
				perfdata[row][col] = v
				missdata[row][col] = m
				start = mi
		start = start + 1
	else:
		start = start + 1
	return find_sectag(content, start)


def process_log(log, set, wp, wm):
	perfdata = np.zeros((5, 14))
	missdata = np.zeros((5, 14))

	content = []
	with open(log, 'r') as fp:
		for txt in fp.readlines():
			content.append(txt.rstrip('\n'))

		start = 1
		while start < len(content):
			start = find_sectag(content, start)
			if start < 0:
				break
			start = process_sec(content, start, perfdata, missdata)
			if start < 0:
				break
	fp.close()

	print('\nmfile: ' + log)
	title = ''
	for col in range(14):
		title = title + ', ' + col2name(col)

	print('counts' + title)
	for row in range(5):
		data = []
		data.append(set + ':' + row2name(row))
		txt = set + ':' + row2name(row)
		for col in range(14):
			data.append(int(perfdata[row][col]))
			txt = txt + ', ' + str(int(perfdata[row][col]))
		wp.writerow(data)
		print(txt)
	
	print('missed' + title)
	for row in range(5):
		data = []
		data.append(set + ':' + row2name(row))
		txt = set + ':' + row2name(row)
		for col in range(14):
			data.append(int(missdata[row][col]))
			txt = txt + ', ' + str(int(missdata[row][col]))
		wm.writerow(data)
		print(txt)

def get_text_size(text, font_size, font_name):
    font = ImageFont.truetype(font_name, font_size)
    size = font.getsize(text)
    return size

def calc_y_pos(dv, row, ymin, ymax, size, fs):
	unit = (ymax - 0) / size[1]
	i = 0
	while i < row:
		if dv[row][-1] > dv[i][-1] + fs * unit:
			i = i + 1
		elif dv[i][-1] > dv[row][-1] + fs * unit:
			i = i + 1
		else:
			break
	if i >= row:
		return dv[row][-1] - unit
	return 0

def process_csv(arch, tag, title):

	linemarks = ['v', '^', 'o', 's', 'x']
	linecolors = ['black', 'dimgray', 'dimgrey', 'gray', 'grey',
	'forestgreen', 'limegreen', 'darkgreen', 'green', 'lime',
	'rosybrown', 'lightcoral', 'indianred', 'brown', 'firebrick',
	'navajowhite', 'blanchedalmond', 'papayawhip', 'moccasin', 'orange',
	'cornsilk', 'gold', 'lemonchiffon', 'khaki', 'palegoldenrod',
	'aquamarine', 'turquoise', 'lightseagreen', 'mediumturquoise', 'cyan',
	'lightblue', 'deepskyblue', 'skyblue', 'lightskyblue', 'steelblue',
  	'darkkhaki', 'olivedrab', 'beige', 'lightyellow', 'olive',
	'midnightblue', 'navy', 'darkblue', 'mediumblue', 'blue', 
	'magenta', 'orchid', 'mediumvioletred', 'deeppink', 'violet',
	'mediumpurple', 'rebeccapurple', 'blueviolet', 'indigo', 'purple']

	csvfperf = arch + '.perf.' + tag + '.csv'
	df = pd.read_csv(csvfperf)
	labs = []
	for item in df['data']:
		labs.append(item)
	df.drop('data', axis=1, inplace=True)
	dv = df.to_numpy()
	x = []
	for item in df:
		x.append(item)
	plt.style.use('fivethirtyeight')
	for row, name in enumerate(labs):
		lc = mcolors.CSS4_COLORS[linecolors[row % len(linecolors)]]
		plt.plot(x, dv[row], linestyle = '-', lw = 1.0, label = name, color = lc,
			 marker = linemarks[row % len(linemarks)], markersize = 3.7)
	plt.grid(True)
	plt.xlabel('threads')
	plt.ylabel('counts')
	xmin, xmax, ymin, ymax = plt.axis()
	plt.gca().set_xlim([xmin, xmax + (xmax - xmin) / 8])
	size = plt.gcf().get_size_inches() * plt.gcf().dpi
	for row, name in enumerate(labs):
		lc = mcolors.CSS4_COLORS[linecolors[row % len(linecolors)]]
		ypos = calc_y_pos(dv, row, ymin, ymax, size, 4)
		if ypos > 0:
			plt.text(x[-1], ypos, '  ' + name, fontsize=8, color=lc)

	plt.title(title, fontsize=18, ha="center")
	plt.legend(frameon=False)
	plt.show()

def process_tag(arch, tag):
	rc = -1
	sets = ['fl', 'flpc', 'op', 'op+', 'sa', 'sapc', 'rs', 'rs+', 'pc']
	csvperf = arch + '.perf.' + tag + '.csv'
	csvmiss = arch + '.miss.' + tag + '.csv'
	title = []
	title.append('data')
	for col in range(14):
		title.append(col2name(col))

	with open(csvperf, 'w') as fperf:
		with open(csvmiss, 'w') as fmiss:
			wperf = csv.writer(fperf)
			wmiss = csv.writer(fmiss)
			wperf.writerow(title)
			wmiss.writerow(title)
			for set in sets:
				log = arch + '.' + set + '.' + tag + '.log'
				process_log(log, set, wperf, wmiss)
			fmiss.close()
			rc = 0
		fperf.close()
	return rc

def main():
#	if process_tag('x86', 'n') == 0:
#		process_csv('x86', 'n', "Throughput of nonblocking sys_flock\nENV: Ubuntu 21.04, 5.13.0 QEMU 96 cores\n(Xeon Platinum 8260 2.4G 48C/96T, DDR4 2933MT/s)")

#	if process_tag('x86', 's') == 0:
#		process_csv('x86', 's', "Throughput of blocking sys_flock (usleep_range)\nENV: Ubuntu 21.04, 5.13.0 QEMU 96 cores\n(Xeon Platinum 8260 2.4G 48C/96T, DDR4 2933MT/s)")

	if process_tag('arm64', 'n') == 0:
		process_csv('arm64', 'n', "Throughput of nonblocking sys_flock\nENV: Ubuntu 20.04 - 5.13.6 QEMU 96 cores\n(HUAWEI TaiShan 2280V2 KP920 2.6G 96C, DDR4 2944 MT/s)")

	if process_tag('arm64', 's') == 0:
		process_csv('arm64', 's', "Throughput of blocking sys_flock (usleep_range)\nENV: Ubuntu 20.04 - 5.13.6 QEMU 96 cores\n(HUAWEI TaiShan 2280V2 KP920 2.6G 96C, DDR4 2944 MT/s)")

# main begin
if __name__=='__main__':
    main()

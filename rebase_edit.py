import sys

with open(sys.argv[1], 'r') as f:
    lines = f.readlines()

# fixup commits 2-5 (indices 1-4), squash 6-7 (indices 5-6), fixup 8-9 (indices 7-8)
for i, line in enumerate(lines):
    if line.startswith('pick '):
        if i in [1, 2, 3, 4, 7, 8]:
            line = line.replace('pick ', 'fixup ')
        elif i in [5, 6]:
            line = line.replace('pick ', 'squash ')
    print(line, end='')
# Drop the reverse hunk (the old contents) of binary file deletions in a `git diff --binary`
# patch; the forward hunk (literal 0) and the full index line stay, so `git apply` still checks
# the file it deletes by its blob hash.
import sys
src, dst = sys.argv[1], sys.argv[2]
out = open(dst, 'wb')
deleted = False
state = 0  # 0 normal, 1 in binary deletion before forward hunk end, 2 skipping reverse hunk
n = 0
with open(src, 'rb') as f:
    for line in f:
        if line.startswith(b'diff --git '):
            deleted = False; state = 0
        elif state == 0 and line.startswith(b'deleted file mode'):
            deleted = True
        elif state == 0 and deleted and line == b'GIT binary patch\n':
            state = 1
        elif state == 1 and line == b'\n':
            out.write(line); state = 2; n += 1; continue
        if state == 2:
            continue
        out.write(line)
print('binary deletions stripped:', n)

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Create an original sparse >5 GiB no-auth/raw-NAPS test package; no game bytes."""
from pathlib import Path
import struct
import sys
BS=65536
meta=5*1024**3
mount=meta+4*BS
# Whole raw payload consists of 256 KiB units; metadata is one final unit.
boundaries=[0,262144,meta,mount]
std_lengths=[];logical=0
while logical<mount:
 end=next(b for b in boundaries if b>logical)
 size=min(262144,end-logical);std_lengths.append((logical,size));logical+=size
records=[]
for pos,size in std_lengths:
 records.append(((1<<18)|((pos>>15)<<19)).to_bytes(9,'little'))
 records.append(bytes(9))
records.append((1<<18).to_bytes(9,'little'))
numu=(mount+262143)//262144
word0=2|(numu<<32);word1=(len(records)-2)<<24
naps=struct.pack('<QQ',word0,word1)+b''.join(v.to_bytes(5,'little')+bytes([0x40 if i==3 else 0]) for i,v in enumerate(boundaries))+bytes(((numu+8)>>3)*10)+b''.join(records)
outer_sb_block=mount//BS
outer_size=(outer_sb_block+4+(len(naps)+BS-1)//BS)*BS
cntbase=BS+outer_size
param=b'{"titleId":"PPSA00001","title":"Synthetic stream test"}'

def dirent(ino,type_,name):
 n=name.encode()+b'\0';size=(16+len(n)+7)&~7
 return struct.pack('<IIII',ino,type_,len(n),size)+n+bytes(size-16-len(n))

def outer_inode(mode,size,block,blocks):
 b=bytearray(0x2c8);struct.pack_into('<HHIqq',b,0,mode,1,0,size,size);struct.pack_into('<I',b,0x60,blocks)
 for i in range(12):struct.pack_into('<i',b,0x84+i*36,-1)
 struct.pack_into('<i',b,0x84,block)
 return b

def inner_inode(mode,size,offset):
 b=bytearray(0xa8);struct.pack_into('<HHIqq',b,0,mode,1,0,size,size);struct.pack_into('<Q',b,0x60,offset);return b

out=Path(sys.argv[1]);out.parent.mkdir(parents=True,exist_ok=True)
with out.open('wb') as f:
 f.truncate(cntbase+8192)
 def put(off,data):f.seek(off);f.write(data)
 h=bytearray(4096);h[:4]=b'\x7fFIH';struct.pack_into('<H',h,6,3)
 for off,val in [(0x10,BS),(0x18,outer_size),(0x20,BS+mount),(0x58,cntbase)]:struct.pack_into('<Q',h,off,val)
 put(0,h)
 sb=bytearray(1024);struct.pack_into('<QQ',sb,0,2,20130315);struct.pack_into('<H',sb,0x1c,13);struct.pack_into('<I',sb,0x20,BS)
 struct.pack_into('<QQQQ',sb,0x28,1,4,outer_size//BS,1);sb[0x370:0x380]=b'PPRPLAIN-NOAUTH!';struct.pack_into('<q',sb,0xd8,outer_sb_block+1)
 put(BS+mount,sb)
 root=dirent(1,3,'uroot');u=dirent(2,2,'pfs_image.dat')+dirent(3,2,'naps_pkg_layout.dat')
 table=b''.join([outer_inode(0x416d,len(root),outer_sb_block+2,1),outer_inode(0x416d,len(u),outer_sb_block+3,1),outer_inode(0x816d,mount,0,mount//BS),outer_inode(0x816d,len(naps),outer_sb_block+4,(len(naps)+BS-1)//BS)])
 put(BS+mount+BS,table);put(BS+mount+2*BS,root);put(BS+mount+3*BS,u);put(BS+mount+4*BS,naps)
 inner_sb=bytearray(128);struct.pack_into('<QQ',inner_sb,0,2,20130315);struct.pack_into('<I',inner_sb,0x20,BS);struct.pack_into('<Q',inner_sb,0x30,5)
 put(BS+meta,inner_sb)
 superdir=dirent(1,3,'uroot');userdir=dirent(2,2,'eboot.bin')+dirent(3,3,'data');datadir=dirent(4,2,'sparse.bin')
 itable=b''.join([inner_inode(0x4000,len(superdir),meta+2*BS),inner_inode(0x4000,len(userdir),meta+3*BS),inner_inode(0x8000,256,0),inner_inode(0x4000,len(datadir),meta+3*BS+128),inner_inode(0x8000,meta-262144,262144)])
 put(BS+meta+BS,itable);put(BS+meta+2*BS,superdir);put(BS+meta+3*BS,userdir);put(BS+meta+3*BS+128,datadir)
 put(BS,b'\x7fELF'+bytes(252))
 # Reads above 4 GiB and across a raw 256 KiB block boundary verify 64-bit offsets.
 for off in [0,262144-16,4*1024**3+123]:put(BS+262144+off,bytes(range(32)))
 cnt=bytearray(4096);cnt[:4]=b'\x7fCNT';struct.pack_into('>I',cnt,0x10,1);struct.pack_into('>I',cnt,0x18,4096)
 put(cntbase,cnt);put(cntbase+4096,struct.pack('>8I',0x2000,0,0,0,4128,len(param),0,0));put(cntbase+4128,param)
print(out)

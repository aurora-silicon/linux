# SPDX-License-Identifier: GPL-2.0-only
import os,sys,mmap,ctypes,threading,concurrent.futures,tempfile,subprocess,signal,zlib,gzip,lzma,hashlib,socket,select,json,sqlite3
ps=int(sys.argv[1]);assert os.sysconf('SC_PAGE_SIZE')==ps==mmap.PAGESIZE,(ps,mmap.PAGESIZE)
libc=ctypes.CDLL(None,use_errno=True);libc.prctl.argtypes=[ctypes.c_int,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong]
def passed(name):print(f'ok - distro Python {ps//1024}K {name}',flush=True)
passed('dynamic loader and page-size contract')
with mmap.mmap(-1, 2*ps, flags=0x08|mmap.MAP_ANONYMOUS) as droppable:
    assert droppable[:]==bytes(2*ps)
    droppable[:]=bytes([0x45])*(2*ps)
    child=os.fork()
    if child==0:os._exit(0 if droppable[:]==bytes(2*ps) else 9)
    assert os.waitpid(child,0)[1]==0
    droppable.madvise(mmap.MADV_DONTNEED)
    assert droppable[:]==bytes(2*ps)
libc.arc4random_buf.argtypes=[ctypes.c_void_p,ctypes.c_size_t]
random_buffer=ctypes.create_string_buffer(32)
random_samples=set()
for _ in range(128):
    libc.arc4random_buf(random_buffer,len(random_buffer));random_samples.add(random_buffer.raw)
assert len(random_samples)==128
passed('droppable write, fork wipe, discard and glibc random state')

blob=bytes(range(256))*4096
with tempfile.TemporaryDirectory() as tmp:
    path=tmp+'/mapped'
    with open(path,'w+b') as f:
        f.truncate(8*ps)
        with mmap.mmap(f.fileno(),2*ps,offset=ps) as m:
            m[:]=bytes([0x53])*(2*ps);m.flush()
            assert os.pread(f.fileno(),2*ps,ps)==m[:]
            m.resize(3*ps);m[2*ps:]=bytes([0x72])*ps;m.flush()
            assert os.pread(f.fileno(),ps,3*ps)==bytes([0x72])*ps
    passed('file mmap offset, flush and resize')
    with mmap.mmap(-1,4*ps,flags=mmap.MAP_PRIVATE|mmap.MAP_ANONYMOUS) as m:
        m[:]=bytes([0x39])*(4*ps)
        c=os.fork()
        if c==0:
            m[ps:2*ps]=bytes([0x91])*ps
            os._exit(0 if m[:ps]==bytes([0x39])*ps else 5)
        assert os.waitpid(c,0)[1]==0 and m[:]==bytes([0x39])*(4*ps)
        address=ctypes.addressof(ctypes.c_char.from_buffer(m))
        libc.mprotect.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int]
        assert libc.mprotect(address+ps,ps,0)==0
        c=os.fork()
        if c==0:
            m[ps]=0
            os._exit(6)
        status=os.waitpid(c,0)[1];assert os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGSEGV
        assert m[0]==0x39 and m[2*ps]==0x39
        assert libc.mprotect(address+ps,ps,mmap.PROT_READ|mmap.PROT_WRITE)==0
    passed('fork COW and independent protection fault')
    def compressed(i):
        d=blob+str(i).encode()
        for compress,decompress in [(zlib.compress,zlib.decompress),(gzip.compress,gzip.decompress),(lambda d:lzma.compress(d,preset=1),lzma.decompress)]:assert decompress(compress(d))==d
        return hashlib.sha256(d).hexdigest()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results=list(pool.map(compressed,range(8)))
    assert len(set(results))==8
    passed('threaded native extension allocation, compression and hashing')
    db=sqlite3.connect(tmp+'/db')
    db.execute('pragma journal_mode=wal');db.execute('pragma mmap_size=1048576')
    db.execute('create table entries (n integer primary key, data blob)')
    db.executemany('insert into entries values (?,?)',[(i,blob[:4096]) for i in range(128)]);db.commit()
    assert db.execute('select count(*) from entries').fetchone()[0]==128
    assert all(row[0]==blob[:4096] for row in db.execute('select data from entries'));db.close()
    passed('SQLite WAL and mapped database reads')
    left,right=socket.socketpair()
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        task=pool.submit(lambda:(right.sendall(blob),right.shutdown(socket.SHUT_WR)))
        out=bytearray()
        while True:
            assert select.select([left],[],[],5)[0]
            part=left.recv(997)
            if not part:break
            out.extend(part)
        task.result();assert bytes(out)==blob
    left.close();right.close();passed('socket polling and fragmented transfers')
    fd=os.memfd_create('cross-abi',0);os.ftruncate(fd,131072);os.pwrite(fd,b'parent',ps)
    # Execute the child in the other ABI, retaining the same shared file bytes.
    target=16384 if ps==4096 else 4096
    def select_abi():
        if libc.prctl(0x41555001,target,0,0,0):os._exit(7)
    code="import os,mmap,sys;fd=int(sys.argv[1]);off=int(sys.argv[2]);want=int(sys.argv[3]);assert os.sysconf('SC_PAGE_SIZE')==want;m=mmap.mmap(fd,131072);assert m[off:off+6]==b'parent';m[off:off+5]=b'child';m.close()"
    subprocess.run([sys.executable,'-I','-c',code,str(fd),str(ps),str(target)],pass_fds=(fd,),preexec_fn=select_abi,check=True)
    assert os.pread(fd,5,ps)==b'child';os.close(fd);passed('cross-ABI dynamic exec and memfd sharing')
    command="set -e; a=(); for ((i=0;i<2048;i++)); do a+=(\"value-$i\"); done; [[ ${#a[@]} == 2048 ]]; [[ ${a[2047]} == value-2047 ]]; printf 'bash-ok\\n'"
    def same_abi():
        if libc.prctl(0x41555001,ps,0,0,0):os._exit(8)
    result=subprocess.run(['/bin/bash','--noprofile','--norc','-c',command],preexec_fn=same_abi,stdout=subprocess.PIPE,check=True,text=True)
    assert result.stdout=='bash-ok\n';passed('distro Bash dynamic exec, allocation and pipes')
print(f'DISTRO PYTHON {ps} PASS',flush=True)

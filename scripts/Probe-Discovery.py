"""Bounded mDNS diagnostic for this receiver; no device pairing or media capture."""
import socket,struct,time,sys,json
interface=sys.argv[1]
name=sys.argv[2] if len(sys.argv)>2 else "AirMirrorLite._airplay._tcp.local"
query=struct.pack("!6H",0,0,1,0,0,0)+b"".join(bytes([len(p)])+p.encode() for p in name.split("."))+b"\0"+struct.pack("!HH",255,0x8001)
sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM,socket.IPPROTO_UDP)
sock.bind((interface,0));sock.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_IF,socket.inet_aton(interface));sock.settimeout(.5)
def read_name(data,pos,visited=None):
    visited=set() if visited is None else visited
    if pos in visited or len(visited)>32:raise ValueError("DNS cycle")
    visited.add(pos);parts=[]
    while True:
        n=data[pos];pos+=1
        if n==0:return ".".join(parts),pos
        if n&0xc0==0xc0:
            offset=((n&63)<<8)|data[pos];pos+=1
            rest,_=read_name(data,offset,visited);parts.append(rest);return ".".join(parts),pos
        if n>63 or pos+n>len(data):raise ValueError("DNS label")
        parts.append(data[pos:pos+n].decode("utf8","replace"));pos+=n
def parse(data):
    _,_,questions,answers,authority,additional=struct.unpack_from("!6H",data)
    if questions+answers+authority+additional>128:raise ValueError("DNS count")
    pos=12;records=[]
    for _ in range(questions):_,pos=read_name(data,pos);pos+=4
    for _ in range(answers+authority+additional):
        owner,pos=read_name(data,pos);typ,cls,ttl,n=struct.unpack_from("!HHIH",data,pos);pos+=10;body=data[pos:pos+n]
        if len(body)!=n:raise ValueError("DNS length")
        value=None
        if typ==1 and n==4:value=socket.inet_ntop(socket.AF_INET,body)
        elif typ==28 and n==16:value=socket.inet_ntop(socket.AF_INET6,body)
        elif typ==33:value={"port":struct.unpack_from("!H",body,4)[0],"host":read_name(data,pos+6)[0]}
        elif typ==12:value=read_name(data,pos)[0]
        elif typ==16:value="TXT present (values omitted)"
        if value is not None:records.append({"name":owner,"type":typ,"ttl":ttl,"value":value})
        pos+=n
    return records
seen=set();deadline=time.monotonic()+4
sock.sendto(query,("224.0.0.251",5353))
while time.monotonic()<deadline:
    try:data,source=sock.recvfrom(9000)
    except socket.timeout:continue
    try:
        for r in parse(data):
            text=json.dumps(r,sort_keys=True)
            if text not in seen:seen.add(text);print(text)
    except (ValueError,IndexError,struct.error):pass
sock.close()
if not seen:print("No mDNS answers received on the selected IPv4 interface.")
